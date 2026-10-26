#pragma once
// Protokol sabitleri — ayrıntı için PROTOCOL.md
#include <cstdint>

namespace proto {

constexpr uint16_t kVideoPort = 47800;
constexpr uint16_t kDiscoveryPort = 47801;

constexpr uint8_t kMagic0 = 0x41;  // 'A'
constexpr uint8_t kMagic1 = 0x59;  // 'Y'

constexpr uint8_t kVideo = 0x01;
constexpr uint8_t kIdrRequest = 0x10;
constexpr uint8_t kKeepalive = 0x11;
constexpr uint8_t kPing = 0x12;
constexpr uint8_t kPong = 0x13;

constexpr uint8_t kFlagKey = 0x01;

constexpr int kHeader = 16;
constexpr int kPayload = 1200;

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

inline void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

}  // namespace proto
