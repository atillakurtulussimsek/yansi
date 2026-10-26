#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

bool netInit();

struct FoundReceiver {
    std::string ip;
    std::string name;
    uint16_t port = 0;
};

// Android alıcısının yayınladığı keşif sinyallerini dinler.
std::vector<FoundReceiver> discoverReceivers(int ms);

// Kareleri 1200 baytlık UDP parçalarına bölüp yollar; alıcıdan gelen kontrol paketlerini dinler.
class FrameSender {
public:
    ~FrameSender();
    bool open(const std::string& ip, uint16_t port);
    // Tek bir iş parçacığından çağrılmalı.
    void sendFrame(const uint8_t* data, size_t len, bool key, uint16_t width, uint16_t height);
    void startControl(std::function<void()> onIdrRequest);
    void stop();

private:
    void controlLoop();
    void sendRaw(const uint8_t* p, int len);

    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in dest_{};
    uint32_t frameId_ = 0;
    std::vector<uint8_t> packet_;
    std::thread control_;
    std::atomic<bool> running_{false};
    std::function<void()> onIdr_;
    bool announced_ = false;
};
