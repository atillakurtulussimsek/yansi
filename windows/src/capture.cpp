#include "capture.h"

#include <d3d11_4.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

bool findOutput(int index, ComPtr<IDXGIAdapter1>& adapterOut, ComPtr<IDXGIOutput>& outputOut) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    int n = 0;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            out->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            if (n == index) {
                adapterOut = adapter;
                outputOut = out;
                return true;
            }
            ++n;
        }
    }
    return false;
}

}  // namespace

std::vector<OutputInfo> listOutputs() {
    std::vector<OutputInfo> list;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return list;
    int n = 0;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            out->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            OutputInfo info;
            info.index = n++;
            info.adapter = ad.Description;
            info.device = d.DeviceName;
            info.width = d.DesktopCoordinates.right - d.DesktopCoordinates.left;
            info.height = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
            list.push_back(info);
        }
    }
    return list;
}

bool DesktopCapture::init(int outputIndex) {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> out;
    if (!findOutput(outputIndex, adapter, out)) {
        fprintf(stderr, "Ekran bulunamadı: %d (listelemek için --list)\n", outputIndex);
        return false;
    }
    if (FAILED(out.As(&output_))) return false;

    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    luid_ = ad.AdapterLuid;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11 aygıtı oluşturulamadı: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    // Media Foundation kodlayıcı aynı aygıtı kendi iş parçacıklarında kullanır.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    bool changed = false;
    return createDuplication(changed);
}

bool DesktopCapture::createDuplication(bool& sizeChanged) {
    sizeChanged = false;
    dupl_.Reset();
    HRESULT hr = output_->DuplicateOutput(device_.Get(), &dupl_);
    if (FAILED(hr)) {
        fprintf(stderr, "Ekran çoğaltma başlatılamadı: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dupl_->GetDesc(&dd);
    int w = static_cast<int>(dd.ModeDesc.Width);
    int h = static_cast<int>(dd.ModeDesc.Height);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        fprintf(stderr, "uyarı: döndürülmüş ekranlar henüz desteklenmiyor, görüntü yan çıkabilir\n");
    }
    if (w != width_ || h != height_ || !composite_) {
        sizeChanged = composite_ != nullptr;
        width_ = w;
        height_ = h;
        if (!createTextures()) return false;
    }
    return true;
}

bool DesktopCapture::reinit(bool& sizeChanged) { return createDuplication(sizeChanged); }

bool DesktopCapture::createTextures() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desktop_.Reset();
    composite_.Reset();
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &desktop_))) return false;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &composite_))) return false;
    return true;
}

int DesktopCapture::acquire(UINT timeoutMs) {
    if (!dupl_) return -1;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (FAILED(hr)) {
        dupl_.Reset();
        return -1;
    }

    bool changed = false;
    if (fi.LastPresentTime.QuadPart != 0 && res) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(res.As(&tex))) {
            context_->CopyResource(desktop_.Get(), tex.Get());
            changed = true;
        }
    }
    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        pointerVisible_ = fi.PointerPosition.Visible != FALSE;
        if (pointerVisible_) pointerPos_ = fi.PointerPosition.Position;
        changed = true;
    }
    if (fi.PointerShapeBufferSize > 0) {
        shape_.resize(fi.PointerShapeBufferSize);
        UINT required = 0;
        if (FAILED(dupl_->GetFramePointerShape(static_cast<UINT>(shape_.size()), shape_.data(), &required,
                                               &shapeInfo_))) {
            shape_.clear();
        }
        changed = true;
    }
    dupl_->ReleaseFrame();

    if (changed) compositeCursor();
    return changed ? 1 : 0;
}

// Masaüstü çoğaltması imleci görüntüye işlemez; imleç altındaki küçük bölgeyi CPU'da harmanlıyoruz.
void DesktopCapture::compositeCursor() {
    context_->CopyResource(composite_.Get(), desktop_.Get());
    if (!pointerVisible_ || shape_.empty()) return;

    const UINT type = shapeInfo_.Type;
    const int sw = static_cast<int>(shapeInfo_.Width);
    const int sh = type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? static_cast<int>(shapeInfo_.Height / 2)
                                                                       : static_cast<int>(shapeInfo_.Height);
    const int pitch = static_cast<int>(shapeInfo_.Pitch);
    if (sw <= 0 || sh <= 0) return;

    const int x0 = pointerPos_.x, y0 = pointerPos_.y;
    const int cx0 = std::max(x0, 0), cy0 = std::max(y0, 0);
    const int cx1 = std::min(x0 + sw, width_), cy1 = std::min(y0 + sh, height_);
    if (cx1 <= cx0 || cy1 <= cy0) return;
    const int rw = cx1 - cx0, rh = cy1 - cy0;

    if (!cursorStage_ || stageW_ < sw || stageH_ < sh) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = std::max(sw, 32);
        td.Height = std::max(sh, 32);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        cursorStage_.Reset();
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &cursorStage_))) return;
        stageW_ = static_cast<int>(td.Width);
        stageH_ = static_cast<int>(td.Height);
    }

    D3D11_BOX box{static_cast<UINT>(cx0), static_cast<UINT>(cy0), 0, static_cast<UINT>(cx1),
                  static_cast<UINT>(cy1), 1};
    context_->CopySubresourceRegion(cursorStage_.Get(), 0, 0, 0, 0, composite_.Get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context_->Map(cursorStage_.Get(), 0, D3D11_MAP_READ_WRITE, 0, &m))) return;

    for (int y = 0; y < rh; ++y) {
        uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m.pData) + y * m.RowPitch);
        const int sy = (cy0 - y0) + y;
        for (int x = 0; x < rw; ++x) {
            const int sx = (cx0 - x0) + x;
            uint32_t d = row[x];
            if (type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                uint32_t s;
                memcpy(&s, &shape_[sy * pitch + sx * 4], 4);
                const uint32_t a = s >> 24;
                if (a == 0) continue;
                if (a == 255) {
                    d = s;
                } else {
                    uint32_t out = 0;
                    for (int c = 0; c < 24; c += 8) {
                        uint32_t sc = (s >> c) & 0xFF, dc = (d >> c) & 0xFF;
                        out |= ((sc * a + dc * (255 - a)) / 255) << c;
                    }
                    d = out;
                }
                row[x] = d | 0xFF000000;
            } else if (type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                const uint8_t mask = static_cast<uint8_t>(0x80 >> (sx % 8));
                const bool andBit = (shape_[sx / 8 + sy * pitch] & mask) != 0;
                const bool xorBit = (shape_[sx / 8 + (sy + sh) * pitch] & mask) != 0;
                const uint32_t and32 = andBit ? 0xFFFFFFFF : 0xFF000000;
                const uint32_t xor32 = xorBit ? 0x00FFFFFF : 0x00000000;
                row[x] = (d & and32) ^ xor32;
            } else if (type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                uint32_t s;
                memcpy(&s, &shape_[sy * pitch + sx * 4], 4);
                row[x] = (s & 0xFF000000) ? ((d ^ s) | 0xFF000000) : (s | 0xFF000000);
            }
        }
    }
    context_->Unmap(cursorStage_.Get(), 0);

    D3D11_BOX src{0, 0, 0, static_cast<UINT>(rw), static_cast<UINT>(rh), 1};
    context_->CopySubresourceRegion(composite_.Get(), 0, static_cast<UINT>(cx0), static_cast<UINT>(cy0), 0,
                                    cursorStage_.Get(), 0, &src);
}
