// yansi-win — Windows ekranını yerel ağdaki Android cihaza yansıtır.
#include "net.h"
// net.h winsock2.h'yi windows.h'den önce getirmeli
#include <windows.h>
#include <avrt.h>
#include <mfapi.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "capture.h"
#include "convert.h"
#include "encoder.h"
#include "protocol.h"

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI onConsoleCtrl(DWORD) {
    g_running = false;
    return TRUE;
}

struct Options {
    std::string to;
    uint16_t port = proto::kVideoPort;
    int display = 0;
    int fps = 60;
    double mbps = 20.0;
    int width = 0;  // 0: otomatik (en fazla 1920)
    bool list = false;
    bool software = false;
    bool stats = false;
    double paceMbps = 250.0;
};

[[noreturn]] void usage(int code) {
    printf(
        "yansi-win — Windows ekranını yerel ağdaki Android cihaza yansıtır\n\n"
        "Kullanım: yansi-win [seçenekler]\n\n"
        "  -t, --to IP        Alıcının IP'si (verilmezse ağda otomatik aranır)\n"
        "  -p, --port N       Alıcı portu (varsayılan 47800)\n"
        "  -d, --display N    Ekran numarası (varsayılan 0)\n"
        "  -l, --list         Ekranları listele\n"
        "  -f, --fps N        Kare hızı (varsayılan 60)\n"
        "  -b, --bitrate N    Bit hızı, Mbps (varsayılan 20)\n"
        "  -w, --width N      Çıkış genişliği (varsayılan: ekran, en fazla 1920)\n"
        "      --pace N       Gönderim hızı sınırı, Mbps (varsayılan 250, 0 = sınırsız)\n"
        "      --software     Donanım kodlayıcıyı kullanma\n"
        "      --stats        Saniyede bir istatistik yaz\n");
    exit(code);
}

Options parse(int argc, char** argv) {
    Options o;
    auto next = [&](int& i) -> const char* {
        if (++i >= argc) usage(1);
        return argv[i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-t" || a == "--to") o.to = next(i);
        else if (a == "-p" || a == "--port") o.port = static_cast<uint16_t>(atoi(next(i)));
        else if (a == "-d" || a == "--display") o.display = atoi(next(i));
        else if (a == "-f" || a == "--fps") o.fps = std::max(1, std::min(240, atoi(next(i))));
        else if (a == "-b" || a == "--bitrate") o.mbps = std::max(1.0, atof(next(i)));
        else if (a == "-w" || a == "--width") o.width = atoi(next(i));
        else if (a == "-l" || a == "--list") o.list = true;
        else if (a == "--software") o.software = true;
        else if (a == "--pace") o.paceMbps = std::max(0.0, atof(next(i)));
        else if (a == "--stats") o.stats = true;
        else if (a == "-h" || a == "--help") usage(0);
        else {
            fprintf(stderr, "Bilinmeyen seçenek: %s\n\n", a.c_str());
            usage(1);
        }
    }
    if (o.port == 0) o.port = proto::kVideoPort;
    return o;
}

int64_t now100ns() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 10000000.0 / static_cast<double>(freq.QuadPart));
}

struct Stats {
    std::atomic<int> frames{0};
    std::atomic<int64_t> bytes{0};
    std::atomic<int> keys{0};
    std::atomic<int> skipped{0};
};

enum class RunResult { Rebuild, Stop };

// Tek bir yakalama → dönüştürme → kodlama hattı. Ekran boyutu değişirse Rebuild döner.
RunResult runPipeline(const Options& opt, FrameSender& sender, std::atomic<bool>& idrRequested, Stats& stats) {
    DesktopCapture cap;
    if (!cap.init(opt.display)) return RunResult::Stop;

    int ow = cap.width(), oh = cap.height();
    int target = opt.width > 0 ? opt.width : std::min(ow, 1920);
    if (target != ow) {
        oh = static_cast<int>(std::lround(static_cast<double>(oh) * target / ow));
        ow = target;
    }
    ow &= ~1;
    oh &= ~1;
    const uint16_t w16 = static_cast<uint16_t>(ow), h16 = static_cast<uint16_t>(oh);

    Nv12Converter conv;
    if (!conv.init(cap.device(), cap.context(), cap.frame(), cap.width(), cap.height(), ow, oh, opt.fps)) {
        return RunResult::Stop;
    }

    MfEncoder enc;
    const int bitrate = static_cast<int>(opt.mbps * 1000000.0);
    bool ok = enc.init(cap.device(), cap.context(), cap.adapterLuid(), ow, oh, opt.fps, bitrate, opt.software,
                       [&](const uint8_t* d, size_t n, bool key) {
                           sender.sendFrame(d, n, key, w16, h16);
                           stats.frames++;
                           stats.bytes += static_cast<int64_t>(n);
                           if (key) stats.keys++;
                       });
    if (!ok) return RunResult::Stop;

    printf("Yansıtılıyor: %dx%d → %dx%d @%d fps, %.0f Mbps, kodlayıcı: %s (%s)\n", cap.width(), cap.height(), ow,
           oh, opt.fps, opt.mbps, enc.name().empty() ? "?" : enc.name().c_str(),
           enc.hardware() ? "donanım" : "yazılım");
    if (!enc.hardware()) {
        printf(
            "UYARI: Donanım kodlayıcı bulunamadı, yazılım kodlayıcı kullanılıyor; gecikme yüksek olur.\n"
            "       Uzak masaüstü (RDP) oturumu veya sanal makinede ekran kartı kodlayıcısı yoktur.\n"
            "       Ekran kartı sürücüsünün güncel olduğundan emin ol; gerekirse --width 1280 --fps 30 dene.\n");
    }
    printf("Durdurmak için Ctrl+C.\n");

    const int64_t interval = 10000000LL / opt.fps;
    const int64_t keyCooldown = 3000000;  // 300 ms: kayıp anında anahtar kare yağmurunu önle
    int64_t lastKey = 0;
    int64_t nextDue = 0;
    bool dirty = true;
    bool forceKey = true;

    while (g_running) {
        int64_t now = now100ns();
        UINT wait = 100;
        if (dirty) {
            int64_t remain = nextDue - now;
            wait = remain > 0 ? static_cast<UINT>(remain / 10000) : 0;
        }

        if (idrRequested.load()) wait = std::min<UINT>(wait, 10);
        int r = cap.acquire(wait);
        if (r < 0) {
            // Çözünürlük değişimi, UAC/kilit ekranı vb.
            bool sizeChanged = false;
            bool recovered = false;
            for (int tries = 0; tries < 50 && g_running; ++tries) {
                Sleep(100);
                if (cap.reinit(sizeChanged)) {
                    recovered = true;
                    break;
                }
            }
            if (!recovered) {
                fprintf(stderr, "Ekran yakalama yeniden başlatılamadı, hat yeniden kuruluyor…\n");
                return RunResult::Rebuild;
            }
            if (sizeChanged) {
                printf("Ekran boyutu değişti, yeniden başlatılıyor…\n");
                return RunResult::Rebuild;
            }
            dirty = true;
            forceKey = true;
            continue;
        }
        if (r == 1) dirty = true;

        now = now100ns();
        if (idrRequested.load() && now - lastKey >= keyCooldown) {
            idrRequested = false;
            forceKey = true;
            dirty = true;  // ekran durağan olsa da anahtar kare üret
        }

        if (dirty && now >= nextDue) {
            ID3D11Texture2D* nv12 = conv.convert();
            if (nv12 && enc.encode(nv12, now, forceKey)) {
                if (forceKey) lastKey = now;
                dirty = false;
                forceKey = false;
                nextDue = now + interval;
            } else {
                stats.skipped++;
                Sleep(1);
            }
        }
    }
    return RunResult::Stop;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt = parse(argc, argv);

    if (opt.list) {
        for (const auto& o : listOutputs()) {
            printf("[%d] %dx%d  %ls  (%ls)\n", o.index, o.width, o.height, o.device.c_str(), o.adapter.c_str());
        }
        return 0;
    }

    if (!netInit()) {
        fprintf(stderr, "Winsock başlatılamadı\n");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        fprintf(stderr, "Media Foundation başlatılamadı (Windows N sürümlerinde Media Feature Pack gerekir)\n");
        return 1;
    }
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

    std::string ip = opt.to;
    uint16_t port = opt.port;
    std::string name = ip;
    if (ip.empty()) {
        printf("Alıcı aranıyor (Android'de Yansı açık olmalı)…\n");
        auto found = discoverReceivers(3000);
        if (found.empty()) {
            fprintf(stderr,
                    "Alıcı bulunamadı. Tablette Yansı'yı aç, aynı ağda olduğundan emin ol\n"
                    "(Windows Güvenlik Duvarı izni de gerekebilir) ya da IP ver: yansi-win --to 192.168.1.50\n");
            return 1;
        }
        if (found.size() > 1) {
            printf("Birden fazla alıcı bulundu:\n");
            for (const auto& r : found) printf("  %s  %s\n", r.ip.c_str(), r.name.c_str());
            printf("İlki seçildi; başkası için --to kullan.\n");
        }
        ip = found[0].ip;
        port = found[0].port;
        name = found[0].name + " (" + ip + ")";
    }

    FrameSender sender;
    if (!sender.open(ip, port)) return 1;
    sender.setPacing(opt.paceMbps);
    printf("Hedef: %s\n", name.c_str());

    std::atomic<bool> idrRequested{false};
    sender.startControl([&] { idrRequested = true; });

    // Zamanlayıcı çözünürlüğü ve iş parçacığı önceliği: gecikmeyi düşürür.
    timeBeginPeriod(1);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    Stats stats;
    std::thread statsThread;
    if (opt.stats) {
        statsThread = std::thread([&] {
            while (g_running) {
                Sleep(1000);
                int f = stats.frames.exchange(0);
                int64_t b = stats.bytes.exchange(0);
                int k = stats.keys.exchange(0);
                int s = stats.skipped.exchange(0);
                printf("%3d fps  %5.1f Mbps  anahtar kare: %d  atlanan: %d\n", f, b * 8 / 1e6, k, s);
            }
        });
    }

    while (g_running) {
        if (runPipeline(opt, sender, idrRequested, stats) == RunResult::Stop) break;
        Sleep(200);
    }

    g_running = false;
    if (statsThread.joinable()) statsThread.join();
    sender.stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    timeEndPeriod(1);
    MFShutdown();
    CoUninitialize();
    WSACleanup();
    return 0;
}
