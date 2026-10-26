# Yansı protokolü (v1)

Tamamen UDP. Alıcı (Android) sunucu gibi davranır; gönderici (Mac/Windows) ona doğrudan video yollar.

| Port  | Yön                    | İçerik                          |
|-------|------------------------|---------------------------------|
| 47800 | gönderici → alıcı      | video paketleri                 |
| 47800 | alıcı → gönderici      | kontrol paketleri (aynı soket)  |
| 47801 | alıcı → yayın (bcast)  | keşif sinyali, saniyede bir     |

Tüm çok baytlı sayılar big-endian.

## Video paketi (gönderici → alıcı)

```
0  u8  'Y' (0x59)
1  u8  'S' (0x53)
2  u8  tip = 0x01
3  u8  bayraklar: bit0 = anahtar kare (IDR, SPS/PPS içerir)
4  u32 kare no (her karede +1, taşınca sarar)
8  u16 parça no (0..adet-1)
10 u16 parça adedi
12 u16 genişlik
14 u16 yükseklik
16 ... yük: H.264 Annex B baytları
```

Son parça hariç her parçanın yükü tam **1200 bayt**. Alıcı parçayı `parça_no * 1200` konumuna yazar.

## FEC paketi (gönderici → alıcı)

Kareler 10'ar veri paketlik gruplara ayrılır; 2 veya daha fazla paketli her grubun ardından bir eşlik paketi gelir.

```
0..15  video başlığıyla aynı; tip = 0x02, parça no = grup no
16 u16 gruptaki yük uzunluklarının XOR'u
18 ... 1200 bayt: gruptaki yüklerin XOR'u (kısa yükler sıfırla tamamlanır)
```

Gruptan tek paket kaybolursa alıcı onu XOR ile yeniden oluşturur. Eski alıcılar tip 0x02'yi yok sayar.

## Gönderim hızı

Göndericiler paketleri varsayılan 250 Mbps'e yayar (`--pace`). Kablolu bilgisayarın büyük kareleri hat hızında
patlatması Wi-Fi erişim noktasında kayba yol açıyordu. Gönderici kayıp sonrası anahtar kareyi en sık 300 ms'de bir üretir.

## Kontrol paketleri (alıcı → gönderici)

Başlık: `'Y' 'S' tip 0x00`, ardından yük.

| Tip  | Ad          | Yük                                  |
|------|-------------|--------------------------------------|
| 0x10 | IDR isteği  | yok                                  |
| 0x11 | keepalive   | UTF-8 cihaz adı                       |
| 0x12 | ping        | u64 alıcı saati (ns)                  |
| 0x13 | pong        | gönderici ping'i olduğu gibi geri yollar |

Alıcı 500 ms'de bir keepalive + ping gönderir. Gönderici her ping'e pong döndüğü için ekran durağanken (kare gelmezken) de bağlantı canlı sayılır.

## Kayıp yönetimi

Bir kare eksik tamamlanırsa veya kare numarasında boşluk varsa alıcı sonraki P-karelerini atar, IDR ister (en fazla 250 ms'de bir) ve ilk anahtar karede çözmeye devam eder. Bozuk görüntü göstermek yerine kısa bir donma tercih edilir.

## Keşif

Alıcı `YANSI1|<cihaz adı>|<video portu>` metnini 47801'e yayınlar. Gönderici `--to` verilmezse 3 sn dinler ve bulduğu ilk alıcıya bağlanır.
