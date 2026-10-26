import CoreGraphics
import CoreMedia
import CoreVideo
import Foundation
import ScreenCaptureKit

final class Counter {
    private let lock = NSLock()
    private var frames = 0
    private var bytes = 0
    private var keys = 0

    func add(bytes b: Int, key: Bool) {
        lock.lock()
        frames += 1
        bytes += b
        if key { keys += 1 }
        lock.unlock()
    }

    func take() -> (frames: Int, bytes: Int, keys: Int) {
        lock.lock()
        defer {
            frames = 0
            bytes = 0
            keys = 0
            lock.unlock()
        }
        return (frames, bytes, keys)
    }
}

/// ScreenCaptureKit ile ekranı yakalar, VideoToolbox ile kodlar, UDP ile yollar.
final class SenderApp: NSObject, SCStreamOutput, SCStreamDelegate {
    private let opts: Options
    private var stream: SCStream?
    private var encoder: H264Encoder?
    private var sender: FrameSender?
    private let queue = DispatchQueue(label: "yansi.capture", qos: .userInteractive)
    private var statsTimer: DispatchSourceTimer?
    private let counter = Counter()

    // Yalnızca `queue` üzerinde kullanılır
    private var lastPixelBuffer: CVPixelBuffer?
    private var pendingKey = true
    private var lastForcedKey = 0.0

    init(options: Options) {
        opts = options
    }

    func start() async throws {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        let displays = content.displays

        if opts.listDisplays {
            for (i, d) in displays.enumerated() {
                print("[\(i)] \(d.width)x\(d.height) (nokta)  id=\(d.displayID)")
            }
            exit(0)
        }
        guard displays.indices.contains(opts.display) else {
            throw YansiError("Ekran bulunamadı: \(opts.display). Listelemek için: yansi-mac --list")
        }
        let display = displays[opts.display]

        let target = try resolveTarget()

        // Çıkış boyutu: varsayılan olarak ekranın nokta (point) çözünürlüğü
        var w: Int
        var h: Int
        if let fixed = opts.width {
            w = fixed
            h = Int((Double(fixed) * Double(display.height) / Double(display.width)).rounded())
        } else {
            w = Int((Double(display.width) * opts.scale).rounded())
            h = Int((Double(display.height) * opts.scale).rounded())
        }
        w = min(w, 4096) & ~1
        h = min(h, 4096) & ~1

        let sender = try FrameSender(ip: target.ip, port: target.port)
        let w16 = UInt16(w)
        let h16 = UInt16(h)
        let counter = self.counter
        let encoder = try H264Encoder(
            width: w, height: h, fps: opts.fps, bitrate: Int(opts.bitrateMbps * 1_000_000)
        ) { data, key in
            sender.sendFrame(data, key: key, width: w16, height: h16)
            counter.add(bytes: data.count, key: key)
        }
        self.sender = sender
        self.encoder = encoder

        sender.onIdrRequest = { [weak self] in self?.requestKeyframe() }
        sender.onReceiver = { name in print("Alıcı bağlandı: \(name)") }
        sender.startControl()

        let filter = SCContentFilter(display: display, excludingWindows: [])
        let cfg = SCStreamConfiguration()
        cfg.width = w
        cfg.height = h
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(opts.fps))
        cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        cfg.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
        cfg.queueDepth = 5
        cfg.showsCursor = true
        cfg.capturesAudio = false

        let stream = SCStream(filter: filter, configuration: cfg, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await stream.startCapture()
        self.stream = stream

        print(
            "Yansıtılıyor → \(target.name) (\(target.ip))  \(w)x\(h) @\(opts.fps) fps, \(opts.bitrateMbps) Mbps"
        )
        print("Durdurmak için Ctrl+C.")
        if opts.stats { startStats() }
    }

    private func resolveTarget() throws -> FoundReceiver {
        if let ip = opts.to {
            return FoundReceiver(ip: ip, name: ip, port: opts.port)
        }
        print("Alıcı aranıyor (Android'de Yansı açık olmalı)…")
        let found = discoverReceivers(seconds: 3)
        guard let first = found.first else {
            throw YansiError(
                "Alıcı bulunamadı. Tablette Yansı'yı aç ve aynı ağda olduğundan emin ol, ya da IP ver: yansi-mac --to 192.168.1.50"
            )
        }
        if found.count > 1 {
            print("Birden fazla alıcı bulundu:")
            for r in found { print("  \(r.ip)  \(r.name)") }
            print("İlki seçildi; başkası için --to kullan.")
        }
        return first
    }

    private func requestKeyframe() {
        queue.async {
            let now = ProcessInfo.processInfo.systemUptime
            guard now - self.lastForcedKey > 0.1 else { return }
            self.lastForcedKey = now
            // Ekran durağansa yeni kare gelmez; son kareyi anahtar kare olarak yeniden kodla.
            if let pb = self.lastPixelBuffer {
                self.encoder?.encode(pb, forceKey: true)
            } else {
                self.pendingKey = true
            }
        }
    }

    private func startStats() {
        let t = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [counter] in
            let s = counter.take()
            let mbps = Double(s.bytes) * 8 / 1_000_000
            print(String(format: "%3d fps  %5.1f Mbps  anahtar kare: %d", s.frames, mbps, s.keys))
        }
        t.resume()
        statsTimer = t
    }

    // MARK: SCStreamOutput

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, CMSampleBufferIsValid(sampleBuffer) else { return }
        guard
            let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false)
                as? [[SCStreamFrameInfo: Any]],
            let raw = attachments.first?[.status] as? Int,
            let status = SCFrameStatus(rawValue: raw),
            status == .complete,
            let pb = CMSampleBufferGetImageBuffer(sampleBuffer)
        else { return }

        lastPixelBuffer = pb
        let key = pendingKey
        pendingKey = false
        encoder?.encode(pb, forceKey: key)
    }

    // MARK: SCStreamDelegate

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        fputs("Yakalama durdu: \(error.localizedDescription)\n", stderr)
        exit(1)
    }
}
