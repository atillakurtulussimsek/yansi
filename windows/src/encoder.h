#pragma once
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// Media Foundation H.264 kodlayıcı. Önce ekran kartının donanım kodlayıcısını (NVENC/QSV/AMF)
// D3D11 dokularıyla dener; olmazsa Microsoft yazılım kodlayıcısına düşer.
// Çıktı Annex B; anahtar karelerde SPS/PPS bulunur.
class MfEncoder {
public:
    using OutputFn = std::function<void(const uint8_t* data, size_t len, bool key)>;

    ~MfEncoder();
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, LUID adapter, int width, int height, int fps,
              int bitrate, bool forceSoftware, OutputFn onOutput);
    // false: kodlayıcı meşgul, kare atlandı (gecikme birikmesin diye bekletmiyoruz).
    bool encode(ID3D11Texture2D* nv12, int64_t time100ns, bool forceKey);
    void shutdown();

    bool hardware() const { return hardware_; }
    const std::string& name() const { return name_; }

private:
    bool activate(bool hardwareOnly, LUID adapter);
    bool configure();
    bool start();
    void eventLoop();
    bool pullOutput();
    void emit(IMFSample* sample);
    void refreshSequenceHeader();
    void refreshOutputInfo();
    HRESULT setCodecU32(const GUID& key, UINT32 value);
    HRESULT setCodecBool(const GUID& key, bool value);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codecApi_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    ComPtr<ID3D11Texture2D> staging_;
    DWORD inId_ = 0, outId_ = 0;

    int width_ = 0, height_ = 0, fps_ = 60, bitrate_ = 0;
    bool async_ = false;
    bool gpuInput_ = false;
    bool hardware_ = false;
    bool providesSamples_ = false;
    DWORD outBufSize_ = 0;
    std::string name_;
    std::vector<uint8_t> seqHeader_;
    std::vector<uint8_t> scratch_;
    OutputFn onOutput_;

    std::thread eventThread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    int needInput_ = 0;
};
