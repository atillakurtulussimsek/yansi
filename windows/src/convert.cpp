#include "convert.h"

#include <cstdio>

static bool fail(const char* what, HRESULT hr) {
    fprintf(stderr, "Renk dönüştürücü: %s (0x%08lx)\n", what, static_cast<unsigned long>(hr));
    return false;
}

bool Nv12Converter::init(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* input, int inW,
                         int inH, int outW, int outH, int fps) {
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&vdev_));
    if (FAILED(hr)) return fail("ID3D11VideoDevice yok (ekran kartı sürücüsü?)", hr);
    hr = context->QueryInterface(IID_PPV_ARGS(&vctx_));
    if (FAILED(hr)) return fail("ID3D11VideoContext yok", hr);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {static_cast<UINT>(fps), 1};
    cd.InputWidth = inW;
    cd.InputHeight = inH;
    cd.OutputFrameRate = {static_cast<UINT>(fps), 1};
    cd.OutputWidth = outW;
    cd.OutputHeight = outH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    hr = vdev_->CreateVideoProcessorEnumerator(&cd, &enum_);
    if (FAILED(hr)) return fail("CreateVideoProcessorEnumerator", hr);

    UINT support = 0;
    if (FAILED(enum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        return fail("NV12 çıkış desteklenmiyor", E_FAIL);
    }

    hr = vdev_->CreateVideoProcessor(enum_.Get(), 0, &vp_);
    if (FAILED(hr)) return fail("CreateVideoProcessor", hr);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.FourCC = 0;
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    iv.Texture2D.MipSlice = 0;
    iv.Texture2D.ArraySlice = 0;
    hr = vdev_->CreateVideoProcessorInputView(input, enum_.Get(), &iv, &inView_);
    if (FAILED(hr)) return fail("CreateVideoProcessorInputView", hr);

    for (int i = 0; i < kRing; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = outW;
        td.Height = outH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        hr = device->CreateTexture2D(&td, nullptr, &tex_[i]);
        if (FAILED(hr)) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = device->CreateTexture2D(&td, nullptr, &tex_[i]);
        }
        if (FAILED(hr)) return fail("NV12 dokusu oluşturulamadı", hr);

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ov.Texture2D.MipSlice = 0;
        hr = vdev_->CreateVideoProcessorOutputView(tex_[i].Get(), enum_.Get(), &ov, &outView_[i]);
        if (FAILED(hr)) return fail("CreateVideoProcessorOutputView", hr);
    }

    // Girdi: tam aralık RGB. Çıkış: BT.709, 16-235.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.RGB_Range = 0;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.YCbCr_Matrix = 1;
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &inCs);
    vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outCs);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    RECT src{0, 0, inW, inH};
    RECT dst{0, 0, outW, outH};
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &dst);
    return true;
}

ID3D11Texture2D* Nv12Converter::convert() {
    const int i = next_;
    next_ = (next_ + 1) % kRing;
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = inView_.Get();
    HRESULT hr = vctx_->VideoProcessorBlt(vp_.Get(), outView_[i].Get(), 0, 1, &st);
    if (FAILED(hr)) return nullptr;
    return tex_[i].Get();
}
