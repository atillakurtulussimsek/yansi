# Yansı

Mac veya Windows ekranını yerel ağdaki Android cihaza düşük gecikmeyle yansıtır.

```
Mac  : ScreenCaptureKit → VideoToolbox H.264 (düşük gecikme RC) ─┐
                                                                ├─ UDP ─→ Android: MediaCodec (low-latency) → SurfaceView
Win  : DXGI Desktop Duplication → GPU NV12 → MF donanım H.264 ──┘
```

Tampon yok, B-kare yok, TCP yok. Kayıpta bozuk görüntü yerine kısa donma + anında IDR isteği.
Protokol: [PROTOCOL.md](PROTOCOL.md).

## Kurulum

Derlenmiş dosyalar her push'ta GitHub Actions → **Artifacts** altında çıkar: `yansi-android` (APK), `yansi-mac`, `yansi-win`.

### Android
APK'yı kur, uygulamayı aç. Ekranda tabletin IP'si görünür; uygulama açık kaldığı sürece bilgisayar onu bulabilir.
Ekrana dokununca istatistikler (fps, bit hızı, alım→ekran süresi, RTT) açılır/kapanır.

### Mac
```bash
tar xzf yansi-mac.tar.gz
xattr -d com.apple.quarantine yansi-mac   # GitHub'dan indirildiyse
./yansi-mac
```
Ya da kaynaktan: `cd mac && swift build -c release && .build/release/yansi-mac`

İlk çalıştırmada **Ekran Kaydı** izni ister: Sistem Ayarları → Gizlilik ve Güvenlik → Ekran ve Sistem Ses Kaydı → terminal uygulamasını aç, terminali yeniden başlat.

### Windows
```
yansi-win.exe
```
İlk çalıştırmada Windows Güvenlik Duvarı sorarsa **Özel ağlar** için izin ver (otomatik keşif ve IDR istekleri için gerekli).

## Seçenekler (iki göndericide de aynı)

| Seçenek | Açıklama |
|---|---|
| `--to IP` | Alıcı IP'si; verilmezse 3 sn ağda aranır |
| `--display N` / `--list` | Ekran seçimi / listeleme |
| `--fps N` | Varsayılan 60 |
| `--bitrate N` | Mbps, varsayılan 20 |
| `--width N` | Çıkış genişliği |
| `--scale X` | (yalnız Mac) Nokta çözünürlüğüne göre ölçek; Retina keskinliği için `2` |
| `--software` | (yalnız Windows) donanım kodlayıcıyı devre dışı bırak |
| `--stats` | Saniyede bir istatistik |

Varsayılan çözünürlük: Mac'te ekranın nokta çözünürlüğü (ör. 14" MBP'de 1512×982), Windows'ta ekran genişliği en fazla 1920.

## Gecikme için öneriler

- Tablet **5 GHz** (ya da 6 GHz) ağda olsun; 2,4 GHz'de gecikme ve kayıp artar.
- Bilgisayar kabloyla modeme bağlıysa en iyi sonuç; ikisi de Wi-Fi'deyse aynı erişim noktasında olsunlar.
- Yazı ağırlıklı ekranlarda bulanıklık varsa önce `--bitrate 30`, sonra çözünürlüğü artır (`--scale 1.5`).
- Ölçüm: tablet istatistiğindeki *alım→ekran* süresi ağ sonrası kısımdır. Uçtan uca gecikmeyi görmek için bilgisayarda kronometre açıp ikisini aynı karede fotoğrafla.

## Bilinen sınırlar (v0.1)

- Yalnızca yansıtma; dokunmatik kontrol ve ses yok.
- Windows'ta döndürülmüş (dikey) ekranlar ve HDR açık ekranlar henüz düzgün desteklenmiyor.
- Paketleme sırasında hata düzeltme (FEC) yok; yoğun kayıplı ağda donmalar olabilir.
