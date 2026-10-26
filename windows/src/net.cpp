#include "net.h"

#include <mstcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "protocol.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

bool netInit() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static void setTimeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}

// UDP'de karşı port kapalıysa Windows recvfrom'u WSAECONNRESET ile bozar; bunu kapat.
static void disableConnReset(SOCKET s) {
    BOOL f = FALSE;
    DWORD br = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &f, sizeof f, nullptr, 0, &br, nullptr, nullptr);
}

std::vector<FoundReceiver> discoverReceivers(int ms) {
    std::vector<FoundReceiver> out;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return out;

    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(proto::kDiscoveryPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        fprintf(stderr, "Keşif portu (%u) açılamadı: %d\n", proto::kDiscoveryPort, WSAGetLastError());
        closesocket(s);
        return out;
    }
    setTimeout(s, 200);
    disableConnReset(s);

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    clock::time_point firstSeen{};
    bool any = false;
    char buf[512];

    while (clock::now() - start < std::chrono::milliseconds(ms)) {
        if (any && clock::now() - firstSeen > std::chrono::milliseconds(600)) break;
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string text(buf, n);
        // YANSI1|ad|port
        size_t p1 = text.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : text.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        if (text.substr(0, p1) != "YANSI1") continue;

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        FoundReceiver r;
        r.ip = ip;
        r.name = text.substr(p1 + 1, p2 - p1 - 1);
        r.port = static_cast<uint16_t>(std::atoi(text.substr(p2 + 1).c_str()));
        if (r.port == 0) r.port = proto::kVideoPort;

        auto it = std::find_if(out.begin(), out.end(), [&](const FoundReceiver& x) { return x.ip == r.ip; });
        if (it == out.end()) {
            out.push_back(r);
            if (!any) {
                any = true;
                firstSeen = clock::now();
            }
        }
    }
    closesocket(s);
    return out;
}

FrameSender::~FrameSender() { stop(); }

bool FrameSender::open(const std::string& ip, uint16_t port) {
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dest_.sin_addr) != 1) {
        fprintf(stderr, "Geçersiz IP adresi: %s\n", ip.c_str());
        return false;
    }
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) return false;

    int sndbuf = 4 << 20;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof sndbuf);
    disableConnReset(sock_);

    // Kontrol paketlerini alabilmek için gönderimden önce yerel porta bağlan.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof local);

    packet_.resize(proto::kHeader + proto::kPayload);
    parity_.resize(proto::kHeader + 2 + proto::kPayload);
    return true;
}

void FrameSender::sendRaw(const uint8_t* p, int len) {
    for (int tries = 0; tries < 50; ++tries) {
        int r = sendto(sock_, reinterpret_cast<const char*>(p), len, 0, reinterpret_cast<const sockaddr*>(&dest_),
                       sizeof dest_);
        if (r != SOCKET_ERROR) return;
        int e = WSAGetLastError();
        if (e != WSAENOBUFS && e != WSAEWOULDBLOCK) return;
        std::this_thread::yield();
    }
}

static int64_t nowNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(freq));
}

void FrameSender::setPacing(double mbps) { nsPerByte_ = mbps > 0 ? 8000.0 / mbps : 0.0; }

// Kablolu bilgisayar büyük kareleri hat hızında patlatınca Wi-Fi erişim noktası paket düşürür.
// Paketleri hedef hıza yayarak bunu önlüyoruz.
void FrameSender::pace(size_t bytes) {
    if (nsPerByte_ <= 0) return;
    int64_t now = nowNs();
    if (nextNs_ < now) nextNs_ = now;
    while (nowNs() < nextNs_) YieldProcessor();
    nextNs_ += static_cast<int64_t>(static_cast<double>(bytes) * nsPerByte_);
}

static void writeHeader(uint8_t* p, uint8_t type, bool key, uint32_t id, uint16_t frag, uint16_t count,
                        uint16_t w, uint16_t h) {
    p[0] = proto::kMagic0;
    p[1] = proto::kMagic1;
    p[2] = type;
    p[3] = key ? proto::kFlagKey : 0;
    proto::put32(p + 4, id);
    proto::put16(p + 8, frag);
    proto::put16(p + 10, count);
    proto::put16(p + 12, w);
    proto::put16(p + 14, h);
}

void FrameSender::sendFrame(const uint8_t* data, size_t len, bool key, uint16_t width, uint16_t height) {
    if (sock_ == INVALID_SOCKET || len == 0) return;
    const size_t count = (len + proto::kPayload - 1) / proto::kPayload;
    if (count > 0xFFFF) return;
    const uint32_t id = frameId_++;
    uint8_t* pkt = packet_.data();
    uint8_t* par = parity_.data();
    uint8_t* parBody = par + proto::kHeader + 2;
    const size_t groups = (count + proto::kFecGroup - 1) / proto::kFecGroup;

    for (size_t g = 0; g < groups; ++g) {
        const size_t first = g * proto::kFecGroup;
        const size_t last = std::min(count, first + proto::kFecGroup);
        memset(parBody, 0, proto::kPayload);
        uint16_t lenXor = 0;

        for (size_t i = first; i < last; ++i) {
            const size_t off = i * proto::kPayload;
            const size_t n = std::min<size_t>(proto::kPayload, len - off);
            writeHeader(pkt, proto::kVideo, key, id, static_cast<uint16_t>(i), static_cast<uint16_t>(count), width,
                        height);
            memcpy(pkt + proto::kHeader, data + off, n);
            for (size_t b = 0; b < n; ++b) parBody[b] ^= data[off + b];
            lenXor ^= static_cast<uint16_t>(n);
            pace(proto::kHeader + n);
            sendRaw(pkt, static_cast<int>(proto::kHeader + n));
        }

        // Grup başına bir eşlik paketi: gruptaki tek kayıp alıcıda yeniden oluşturulur.
        if (last - first >= 2) {
            writeHeader(par, proto::kFec, key, id, static_cast<uint16_t>(g), static_cast<uint16_t>(count), width,
                        height);
            proto::put16(par + proto::kHeader, lenXor);
            const size_t n = proto::kHeader + 2 + proto::kPayload;
            pace(n);
            sendRaw(par, static_cast<int>(n));
        }
    }
}

void FrameSender::startControl(std::function<void()> onIdrRequest) {
    onIdr_ = std::move(onIdrRequest);
    setTimeout(sock_, 500);
    running_ = true;
    control_ = std::thread([this] { controlLoop(); });
}

void FrameSender::stop() {
    running_ = false;
    if (control_.joinable()) control_.join();
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

void FrameSender::controlLoop() {
    uint8_t buf[1500];
    while (running_) {
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 4) continue;
        if (from.sin_addr.s_addr != dest_.sin_addr.s_addr) continue;
        if (buf[0] != proto::kMagic0 || buf[1] != proto::kMagic1) continue;
        switch (buf[2]) {
            case proto::kIdrRequest:
                if (onIdr_) onIdr_();
                break;
            case proto::kKeepalive:
                if (!announced_) {
                    announced_ = true;
                    std::string name(reinterpret_cast<char*>(buf + 4), n - 4);
                    printf("Alıcı bağlandı: %s\n", name.c_str());
                }
                break;
            case proto::kPing:
                buf[2] = proto::kPong;
                sendto(sock_, reinterpret_cast<const char*>(buf), n, 0, reinterpret_cast<const sockaddr*>(&from),
                       sizeof from);
                break;
            default:
                break;
        }
    }
}
