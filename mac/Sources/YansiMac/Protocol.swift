import Foundation

/// Protokol sabitleri — ayrıntı için PROTOCOL.md.
enum Proto {
    static let videoPort: UInt16 = 47800
    static let discoveryPort: UInt16 = 47801

    static let magic0: UInt8 = 0x59  // 'Y'
    static let magic1: UInt8 = 0x53  // 'S'

    static let tVideo: UInt8 = 0x01
    static let tIdrRequest: UInt8 = 0x10
    static let tKeepalive: UInt8 = 0x11
    static let tPing: UInt8 = 0x12
    static let tPong: UInt8 = 0x13

    static let flagKey: UInt8 = 0x01

    static let headerSize = 16
    static let payloadSize = 1200

    static func writeVideoHeader(
        _ p: UnsafeMutablePointer<UInt8>, key: Bool, frameId: UInt32,
        fragment: UInt16, count: UInt16, width: UInt16, height: UInt16
    ) {
        p[0] = magic0
        p[1] = magic1
        p[2] = tVideo
        p[3] = key ? flagKey : 0
        p[4] = UInt8(truncatingIfNeeded: frameId >> 24)
        p[5] = UInt8(truncatingIfNeeded: frameId >> 16)
        p[6] = UInt8(truncatingIfNeeded: frameId >> 8)
        p[7] = UInt8(truncatingIfNeeded: frameId)
        p[8] = UInt8(truncatingIfNeeded: fragment >> 8)
        p[9] = UInt8(truncatingIfNeeded: fragment)
        p[10] = UInt8(truncatingIfNeeded: count >> 8)
        p[11] = UInt8(truncatingIfNeeded: count)
        p[12] = UInt8(truncatingIfNeeded: width >> 8)
        p[13] = UInt8(truncatingIfNeeded: width)
        p[14] = UInt8(truncatingIfNeeded: height >> 8)
        p[15] = UInt8(truncatingIfNeeded: height)
    }
}

struct YansiError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}
