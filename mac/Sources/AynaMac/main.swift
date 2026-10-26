import CoreGraphics
import Foundation

struct Options {
    var to: String?
    var port: UInt16 = Proto.videoPort
    var display = 0
    var fps = 60
    var bitrateMbps = 20.0
    var scale = 1.0
    var width: Int?
    var listDisplays = false
    var stats = false

    static func parse(_ args: [String]) -> Options {
        var o = Options()
        var i = 1
        func next() -> String {
            i += 1
            guard i < args.count else { usage(1) }
            return args[i]
        }
        while i < args.count {
            switch args[i] {
            case "-t", "--to": o.to = next()
            case "-p", "--port": o.port = UInt16(next()) ?? usage(1)
            case "-d", "--display": o.display = Int(next()) ?? usage(1)
            case "-f", "--fps": o.fps = max(1, min(240, Int(next()) ?? usage(1)))
            case "-b", "--bitrate": o.bitrateMbps = max(1, Double(next()) ?? usage(1))
            case "-s", "--scale": o.scale = max(0.25, min(4, Double(next()) ?? usage(1)))
            case "-w", "--width": o.width = Int(next()) ?? usage(1)
            case "-l", "--list": o.listDisplays = true
            case "--stats": o.stats = true
            case "-h", "--help": usage(0)
            default:
                fputs("Bilinmeyen seçenek: \(args[i])\n\n", stderr)
                usage(1)
            }
            i += 1
        }
        return o
    }
}

func usage(_ code: Int32) -> Never {
    print(
        """
        ayna-mac — Mac ekranını yerel ağdaki Android cihaza yansıtır

        Kullanım: ayna-mac [seçenekler]

          -t, --to IP        Alıcının IP'si (verilmezse ağda otomatik aranır)
          -p, --port N       Alıcı portu (varsayılan 47800)
          -d, --display N    Ekran numarası (varsayılan 0)
          -l, --list         Ekranları listele
          -f, --fps N        Kare hızı (varsayılan 60)
          -b, --bitrate N    Bit hızı, Mbps (varsayılan 20)
          -s, --scale X      Nokta çözünürlüğüne göre ölçek (varsayılan 1.0; Retina keskinliği için 2)
          -w, --width N      Çıkış genişliği (ölçeği geçersiz kılar)
              --stats        Saniyede bir istatistik yaz
        """)
    exit(code)
}

setvbuf(stdout, nil, _IOLBF, 0)
let options = Options.parse(CommandLine.arguments)

// App Nap ve zamanlayıcı birleştirmesi gecikme yaratmasın.
let activity = ProcessInfo.processInfo.beginActivity(
    options: [.userInitiated, .latencyCritical, .idleSystemSleepDisabled],
    reason: "Ayna ekran yansıtma")

if !CGPreflightScreenCaptureAccess() {
    _ = CGRequestScreenCaptureAccess()
    print(
        """
        Ekran Kaydı izni gerekli.
        Sistem Ayarları → Gizlilik ve Güvenlik → Ekran ve Sistem Ses Kaydı bölümünde
        bu programı çalıştırdığın terminal uygulamasını aç, terminali kapatıp yeniden başlat.
        """)
    exit(1)
}

let app = SenderApp(options: options)
Task {
    do {
        try await app.start()
    } catch {
        fputs("Hata: \(error)\n", stderr)
        exit(1)
    }
}

signal(SIGINT, { _ in exit(0) })
withExtendedLifetime(activity) {
    dispatchMain()
}
