import Darwin
import Foundation

// MARK: - Soket yardımcıları

func makeAddress(_ ip: String, _ port: UInt16) -> sockaddr_in? {
    var a = sockaddr_in()
    a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    a.sin_family = sa_family_t(AF_INET)
    a.sin_port = port.bigEndian
    guard inet_pton(AF_INET, ip, &a.sin_addr) == 1 else { return nil }
    return a
}

func ipString(_ a: sockaddr_in) -> String {
    var addr = a.sin_addr
    var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
    inet_ntop(AF_INET, &addr, &buf, socklen_t(INET_ADDRSTRLEN))
    return String(cString: buf)
}

func setReceiveTimeout(_ fd: Int32, ms: Int) {
    var tv = timeval(tv_sec: ms / 1000, tv_usec: Int32((ms % 1000) * 1000))
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
}

func receiveFrom(_ fd: Int32, _ buf: UnsafeMutableRawPointer, _ capacity: Int) -> (Int, sockaddr_in) {
    var from = sockaddr_in()
    var len = socklen_t(MemoryLayout<sockaddr_in>.size)
    let n = withUnsafeMutablePointer(to: &from) { p in
        p.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, buf, capacity, 0, $0, &len) }
    }
    return (n, from)
}

@discardableResult
func sendTo(_ fd: Int32, _ buf: UnsafeRawPointer, _ len: Int, _ addr: sockaddr_in) -> Int {
    var a = addr
    return withUnsafePointer(to: &a) { p in
        p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            sendto(fd, buf, len, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
        }
    }
}

// MARK: - Keşif

struct FoundReceiver {
    let ip: String
    let name: String
    let port: UInt16
}

/// Android alıcısının yayınladığı `YANSI1|ad|port` sinyallerini dinler.
func discoverReceivers(seconds: Double) -> [FoundReceiver] {
    let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
    guard fd >= 0 else { return [] }
    defer { close(fd) }

    var one: Int32 = 1
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, socklen_t(MemoryLayout<Int32>.size))

    var addr = sockaddr_in()
    addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_port = Proto.discoveryPort.bigEndian
    addr.sin_addr.s_addr = 0  // INADDR_ANY
    let bound = withUnsafePointer(to: &addr) { p in
        p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
        }
    }
    guard bound == 0 else {
        fputs("Keşif portu (\(Proto.discoveryPort)) açılamadı: \(String(cString: strerror(errno)))\n", stderr)
        return []
    }
    setReceiveTimeout(fd, ms: 200)

    var found: [String: FoundReceiver] = [:]
    var order: [String] = []
    var buf = [UInt8](repeating: 0, count: 512)
    let start = Date()
    var firstSeen: Date?

    while Date().timeIntervalSince(start) < seconds {
        // İlk alıcı bulunduktan sonra diğerleri için 0,6 sn daha bekle.
        if let f = firstSeen, Date().timeIntervalSince(f) > 0.6 { break }
        let (n, from) = buf.withUnsafeMutableBytes { receiveFrom(fd, $0.baseAddress!, $0.count) }
        guard n > 0, let text = String(bytes: buf[0..<n], encoding: .utf8) else { continue }
        let parts = text.split(separator: "|", omittingEmptySubsequences: false)
        guard parts.count >= 3, parts[0] == "YANSI1", let port = UInt16(parts[2]) else { continue }
        let ip = ipString(from)
        if found[ip] == nil {
            order.append(ip)
            if firstSeen == nil { firstSeen = Date() }
        }
        found[ip] = FoundReceiver(ip: ip, name: String(parts[1]), port: port)
    }
    return order.compactMap { found[$0] }
}

// MARK: - Gönderici

/// Kareleri 1200 baytlık UDP parçalarına bölüp yollar; alıcıdan gelen kontrol paketlerini dinler.
final class FrameSender {
    private let fd: Int32
    private let dest: sockaddr_in
    private var frameId: UInt32 = 0
    private let packet: UnsafeMutablePointer<UInt8>
    private let parity: UnsafeMutablePointer<UInt8>
    private var announced = false
    private var nsPerByte = 0.0
    private var nextNs: UInt64 = 0

    var onIdrRequest: (() -> Void)?
    var onReceiver: ((String) -> Void)?

    init(ip: String, port: UInt16) throws {
        guard let d = makeAddress(ip, port) else { throw YansiError("Geçersiz IP adresi: \(ip)") }
        dest = d
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw YansiError("UDP soketi açılamadı") }

        var sndbuf: Int32 = 4 << 20
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
        // DSCP EF → Wi-Fi WMM'de video kuyruğu
        var tos: Int32 = 0xB8
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, socklen_t(MemoryLayout<Int32>.size))
        var nosig: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, socklen_t(MemoryLayout<Int32>.size))

        packet = .allocate(capacity: Proto.headerSize + Proto.payloadSize)
        parity = .allocate(capacity: Proto.headerSize + 2 + Proto.payloadSize)
    }

    /// Gönderim hızı sınırı (Mbps); 0 = sınırsız.
    func setPacing(mbps: Double) {
        nsPerByte = mbps > 0 ? 8000.0 / mbps : 0
    }

    private func pace(_ bytes: Int) {
        guard nsPerByte > 0 else { return }
        let now = DispatchTime.now().uptimeNanoseconds
        if nextNs < now { nextNs = now }
        while DispatchTime.now().uptimeNanoseconds < nextNs {}
        nextNs += UInt64(Double(bytes) * nsPerByte)
    }

    /// VideoToolbox çıktı iş parçacığından çağrılır (tek iş parçacığı).
    func sendFrame(_ data: Data, key: Bool, width: UInt16, height: UInt16) {
        let count = (data.count + Proto.payloadSize - 1) / Proto.payloadSize
        guard count > 0, count <= Int(UInt16.max) else { return }
        let id = frameId
        frameId &+= 1

        let total = data.count
        let parityBody = parity + Proto.headerSize + 2
        let groups = (count + Proto.fecGroup - 1) / Proto.fecGroup

        data.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
            guard let raw = src.baseAddress else { return }
            let base = raw.assumingMemoryBound(to: UInt8.self)
            for g in 0..<groups {
                let first = g * Proto.fecGroup
                let last = min(count, first + Proto.fecGroup)
                memset(parityBody, 0, Proto.payloadSize)
                var lenXor: UInt16 = 0

                for i in first..<last {
                    let off = i * Proto.payloadSize
                    let len = min(Proto.payloadSize, total - off)
                    Proto.writeVideoHeader(
                        packet, key: key, frameId: id, fragment: UInt16(i), count: UInt16(count),
                        width: width, height: height)
                    memcpy(packet + Proto.headerSize, base + off, len)
                    for b in 0..<len { parityBody[b] ^= base[off + b] }
                    lenXor ^= UInt16(len)
                    pace(Proto.headerSize + len)
                    sendRaw(packet, Proto.headerSize + len)
                }

                // Grup başına bir XOR eşlik paketi: tek kayıp alıcıda yeniden oluşturulur.
                if last - first >= 2 {
                    Proto.writeVideoHeader(
                        parity, type: Proto.tFec, key: key, frameId: id, fragment: UInt16(g),
                        count: UInt16(count), width: width, height: height)
                    parity[Proto.headerSize] = UInt8(truncatingIfNeeded: lenXor >> 8)
                    parity[Proto.headerSize + 1] = UInt8(truncatingIfNeeded: lenXor)
                    let n = Proto.headerSize + 2 + Proto.payloadSize
                    pace(n)
                    sendRaw(parity, n)
                }
            }
        }
    }

    private func sendRaw(_ p: UnsafeRawPointer, _ len: Int) {
        var tries = 0
        while true {
            if sendTo(fd, p, len, dest) >= 0 { return }
            let e = errno
            if (e == ENOBUFS || e == EAGAIN) && tries < 50 {
                tries += 1
                usleep(200)
                continue
            }
            return
        }
    }

    func startControl() {
        setReceiveTimeout(fd, ms: 500)
        let t = Thread { [weak self] in self?.controlLoop() }
        t.name = "yansi-control"
        t.qualityOfService = .userInteractive
        t.start()
    }

    private func controlLoop() {
        var buf = [UInt8](repeating: 0, count: 1500)
        while true {
            let (n, from) = buf.withUnsafeMutableBytes { receiveFrom(fd, $0.baseAddress!, $0.count) }
            guard n >= 4 else { continue }
            guard from.sin_addr.s_addr == dest.sin_addr.s_addr else { continue }
            guard buf[0] == Proto.magic0, buf[1] == Proto.magic1 else { continue }
            switch buf[2] {
            case Proto.tIdrRequest:
                onIdrRequest?()
            case Proto.tKeepalive:
                if !announced {
                    announced = true
                    let name = String(bytes: buf[4..<n], encoding: .utf8) ?? "?"
                    onReceiver?(name)
                }
            case Proto.tPing:
                buf[2] = Proto.tPong
                buf.withUnsafeBytes { _ = sendTo(fd, $0.baseAddress!, n, from) }
            default:
                break
            }
        }
    }
}
