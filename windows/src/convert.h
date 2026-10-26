#pragma once
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// GPU üzerinde BGRA → NV12 (BT.709, sınırlı aralık) dönüşümü ve ölçekleme.
class Nv12Converter {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* input, int inW, int inH,
              int outW, int outH, int fps);
    // Girdiyi halkadaki bir sonraki NV12 dokusuna dönüştürür; hata olursa nullptr.
    ID3D11Texture2D* convert();

private:
    static constexpr int kRing = 6;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> enum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorInputView> inView_;
    ComPtr<ID3D11Texture2D> tex_[kRing];
    ComPtr<ID3D11VideoProcessorOutputView> outView_[kRing];
    int next_ = 0;
};
