#include "encoder.h"

#include <mferror.h>
#include <oleauto.h>

#include <cstdio>
#include <cstring>

// codecapi.h GUID'leri bu çeviri biriminde tanımlansın.
#include <initguid.h>
#include <codecapi.h>

namespace {

bool hasNal(const uint8_t* p, size_t len, int type) {
    for (size_t i = 0; i + 3 < len; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if ((p[i + 3] & 0x1F) == type) return true;
            i += 2;
        }
    }
    return false;
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

MfEncoder::~MfEncoder() { shutdown(); }

HRESULT MfEncoder::setCodecU32(const GUID& key, UINT32 value) {
    if (!codecApi_) return E_NOINTERFACE;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    return codecApi_->SetValue(&key, &v);
}

HRESULT MfEncoder::setCodecBool(const GUID& key, bool value) {
    if (!codecApi_) return E_NOINTERFACE;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return codecApi_->SetValue(&key, &v);
}

bool MfEncoder::init(ID3D11Device* device, ID3D11DeviceContext* context, LUID adapter, int width, int height,
                     int fps, int bitrate, bool forceSoftware, OutputFn onOutput) {
    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;
    onOutput_ = std::move(onOutput);

    if (!forceSoftware) {
        if (activate(true, adapter) && configure() && start()) {
            hardware_ = true;
            return true;
        }
        fprintf(stderr, "Donanım kodlayıcı kullanılamadı, yazılım kodlayıcıya geçiliyor.\n");
        shutdown();
    }
    if (activate(false, adapter) && configure() && start()) {
        hardware_ = false;
        return true;
    }
    fprintf(stderr, "H.264 kodlayıcı başlatılamadı.\n");
    return false;
}

bool MfEncoder::activate(bool hardwareOnly, LUID adapter) {
    mft_.Reset();
    codecApi_.Reset();
    events_.Reset();
    async_ = false;
    gpuInput_ = false;

    MFT_REGISTER_TYPE_INFO inType{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    HRESULT hr;

    if (hardwareOnly) {
        // Önce yakalama yapılan ekran kartına ait kodlayıcıyı iste.
        ComPtr<IMFAttributes> ea;
        MFCreateAttributes(&ea, 1);
        ea->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapter), sizeof adapter);
        hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inType,
                      &outType, ea.Get(), &acts, &count);
        if (FAILED(hr) || count == 0) {
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           &inType, &outType, &acts, &count);
        }
    } else {
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &inType,
                       &outType, &acts, &count);
    }
    if (FAILED(hr) || count == 0) return false;

    for (UINT32 i = 0; i < count && !mft_; ++i) {
        if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft_)))) {
            wchar_t* fn = nullptr;
            UINT32 fnLen = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &fnLen))) {
                name_ = narrow(fn);
                CoTaskMemFree(fn);
            }
        }
    }
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (!mft_) return false;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
        UINT32 isAsync = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            async_ = true;
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

        UINT32 d3dAware = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware)) && d3dAware) {
            UINT token = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &deviceManager_)) &&
                SUCCEEDED(deviceManager_->ResetDevice(device_, token)) &&
                SUCCEEDED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                               reinterpret_cast<ULONG_PTR>(deviceManager_.Get())))) {
                gpuInput_ = true;
            }
        }
    }
    mft_.As(&codecApi_);

    HRESULT sh = mft_->GetStreamIDs(1, &inId_, 1, &outId_);
    if (FAILED(sh)) {
        inId_ = 0;
        outId_ = 0;
    }
    return true;
}

bool MfEncoder::configure() {
    // Bazı kodlayıcılar bu ayarları ancak türler belirlenmeden önce kabul eder.
    setCodecBool(CODECAPI_AVLowLatencyMode, true);
    // CBR + yaklaşık 2 karelik VBV tamponu: anahtar kareler de küçük kalır, ağda ani patlama olmaz.
    if (FAILED(setCodecU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR))) {
        setCodecU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_LowDelayVBR);
    }
    setCodecU32(CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(bitrate_));
    setCodecU32(CODECAPI_AVEncCommonMaxBitRate, static_cast<UINT32>(bitrate_));
    setCodecU32(CODECAPI_AVEncCommonBufferSize, static_cast<UINT32>(bitrate_ / fps_ * 2));
    setCodecU32(CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(fps_ * 10));
    setCodecU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    setCodecU32(CODECAPI_AVEncCommonQualityVsSpeed, 30);

    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate_));
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    HRESULT hr = mft_->SetOutputType(outId_, out.Get(), 0);
    if (FAILED(hr)) {
        // High profili desteklemeyen kodlayıcılar için Main dene.
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = mft_->SetOutputType(outId_, out.Get(), 0);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "Kodlayıcı çıkış türü reddedildi: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    hr = mft_->SetInputType(inId_, in.Get(), 0);
    if (FAILED(hr)) {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetInputAvailableType(inId_, i, &t))) break;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, width_, height_);
            MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps_, 1);
            hr = mft_->SetInputType(inId_, t.Get(), 0);
            if (SUCCEEDED(hr)) break;
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "Kodlayıcı giriş türü (NV12) reddedildi: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    setCodecU32(CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(bitrate_));
    refreshOutputInfo();
    refreshSequenceHeader();

    if (!gpuInput_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) return false;
    }
    return true;
}

void MfEncoder::refreshOutputInfo() {
    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(outId_, &info))) {
        providesSamples_ =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outBufSize_ = info.cbSize;
    }
    if (outBufSize_ == 0) outBufSize_ = static_cast<DWORD>(width_ * height_ * 3 / 2);
}

void MfEncoder::refreshSequenceHeader() {
    ComPtr<IMFMediaType> cur;
    if (FAILED(mft_->GetOutputCurrentType(outId_, &cur))) return;
    UINT32 size = 0;
    if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
        seqHeader_.resize(size);
        if (FAILED(cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader_.data(), size, nullptr))) seqHeader_.clear();
    }
}

bool MfEncoder::start() {
    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return false;
    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return false;
    if (async_) {
        if (FAILED(mft_.As(&events_))) return false;
        needInput_ = 0;
        running_ = true;
        eventThread_ = std::thread([this] { eventLoop(); });
    }
    return true;
}

void MfEncoder::eventLoop() {
    while (running_) {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(events_->GetEvent(0, &ev))) break;
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            std::lock_guard<std::mutex> lk(mutex_);
            ++needInput_;
        } else if (type == METransformHaveOutput) {
            pullOutput();
        }
    }
}

bool MfEncoder::encode(ID3D11Texture2D* nv12, int64_t time100ns, bool forceKey) {
    if (!mft_ || !nv12) return false;

    if (async_) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (needInput_ <= 0) return false;
        --needInput_;
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (gpuInput_) {
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer))) return false;
        ComPtr<IMF2DBuffer> b2;
        DWORD len = 0;
        if (SUCCEEDED(buffer.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buffer->SetCurrentLength(len);
    } else {
        // Yazılım kodlayıcı: NV12'yi sistem belleğine kopyala.
        context_->CopyResource(staging_.Get(), nv12);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        const DWORD ySize = static_cast<DWORD>(width_ * height_);
        const DWORD total = ySize * 3 / 2;
        if (FAILED(MFCreateMemoryBuffer(total, &buffer))) {
            context_->Unmap(staging_.Get(), 0);
            return false;
        }
        BYTE* dst = nullptr;
        buffer->Lock(&dst, nullptr, nullptr);
        const BYTE* src = static_cast<const BYTE*>(m.pData);
        for (int y = 0; y < height_; ++y) memcpy(dst + y * width_, src + y * m.RowPitch, width_);
        const BYTE* uv = src + static_cast<size_t>(m.RowPitch) * height_;
        for (int y = 0; y < height_ / 2; ++y) memcpy(dst + ySize + y * width_, uv + y * m.RowPitch, width_);
        buffer->Unlock();
        buffer->SetCurrentLength(total);
        context_->Unmap(staging_.Get(), 0);
    }

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(time100ns);
    sample->SetSampleDuration(10000000LL / fps_);

    if (forceKey) setCodecU32(CODECAPI_AVEncVideoForceKeyFrame, 1);

    if (!async_) {
        while (pullOutput()) {
        }
    }
    HRESULT hr = mft_->ProcessInput(inId_, sample.Get(), 0);
    if (FAILED(hr)) {
        if (async_) {
            std::lock_guard<std::mutex> lk(mutex_);
            ++needInput_;
        }
        return false;
    }
    if (!async_) {
        while (pullOutput()) {
        }
    }
    return true;
}

// Bir çıktı örneği çeker. Çıktı alındıysa true.
bool MfEncoder::pullOutput() {
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = outId_;
    ComPtr<IMFSample> own;
    if (!providesSamples_) {
        ComPtr<IMFMediaBuffer> b;
        if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(outBufSize_, &b))) return false;
        own->AddBuffer(b.Get());
        ob.pSample = own.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();

    IMFSample* produced = ob.pSample;
    const bool mftOwned = providesSamples_ && produced != nullptr;

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (mftOwned) produced->Release();
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, &t))) mft_->SetOutputType(outId_, t.Get(), 0);
        refreshOutputInfo();
        refreshSequenceHeader();
        return true;
    }
    if (FAILED(hr)) {
        if (mftOwned) produced->Release();
        return false;
    }
    if (produced) emit(produced);
    if (mftOwned) produced->Release();
    return true;
}

void MfEncoder::emit(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(sample->ConvertToContiguousBuffer(&mb))) return;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(mb->Lock(&p, nullptr, &len))) return;

    UINT32 clean = 0;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    const bool key = clean != 0 || hasNal(p, len, 5);

    if (key && !hasNal(p, len, 7)) {
        if (seqHeader_.empty()) refreshSequenceHeader();
        if (!seqHeader_.empty()) {
            scratch_.assign(seqHeader_.begin(), seqHeader_.end());
            scratch_.insert(scratch_.end(), p, p + len);
            if (onOutput_) onOutput_(scratch_.data(), scratch_.size(), true);
            mb->Unlock();
            return;
        }
    }
    if (onOutput_) onOutput_(p, len, key);
    mb->Unlock();
}

void MfEncoder::shutdown() {
    running_ = false;
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        ComPtr<IMFShutdown> sd;
        if (SUCCEEDED(mft_.As(&sd))) sd->Shutdown();
    }
    if (eventThread_.joinable()) {
        if (events_) {
            eventThread_.join();
        } else {
            eventThread_.detach();
        }
    }
    events_.Reset();
    codecApi_.Reset();
    mft_.Reset();
    deviceManager_.Reset();
    staging_.Reset();
}
