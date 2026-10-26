#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct OutputInfo {
    int index;
    std::wstring adapter;
    std::wstring device;
    int width;
    int height;
};

std::vector<OutputInfo> listOutputs();

// DXGI Desktop Duplication ile ekran yakalama. İmleç yakalanan görüntüye işlenir.
class DesktopCapture {
public:
    // false: ekran bulunamadı ya da çoğaltma başlatılamadı
    bool init(int outputIndex);
    // 1: görüntü değişti, 0: değişiklik yok, -1: erişim kaybedildi (reinit gerekir)
    int acquire(UINT timeoutMs);
    // Çoğaltmayı yeniden oluşturur. Boyut değiştiyse sizeChanged true olur.
    bool reinit(bool& sizeChanged);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    ID3D11Texture2D* frame() const { return composite_.Get(); }
    LUID adapterLuid() const { return luid_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool createDuplication(bool& sizeChanged);
    bool createTextures();
    void compositeCursor();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    ComPtr<ID3D11Texture2D> desktop_;    // son masaüstü görüntüsü (imleçsiz)
    ComPtr<ID3D11Texture2D> composite_;  // imleç işlenmiş görüntü
    ComPtr<ID3D11Texture2D> cursorStage_;
    LUID luid_{};
    int width_ = 0, height_ = 0;
    int stageW_ = 0, stageH_ = 0;

    std::vector<uint8_t> shape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo_{};
    bool pointerVisible_ = false;
    POINT pointerPos_{};
};
