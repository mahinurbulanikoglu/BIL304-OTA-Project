# BIL304 — OTA Firmware Projesi

## Demo Videosu

[YouTube video linki buraya eklenecek]

---

## Proje Hakkında

Bu projede Contiki-NG ve Cooja kullanılarak Z1 düğümleri arasında OTA (Over-The-Air) firmware güncelleme mekanizması geliştirildi. Simülasyonda üç düğüm kullanıldı:

- Node 1: Alıcı ve RPL ağ kökü (udp-server.z1)
- Node 2: Gönderici (udp-client.z1)
- Node 3: Yönlendirici / komşu düğüm (udp-client.z1)

Node 2 ve Node 3 aynı firmware ile çalışıyor. Ancak `node_id` kontrolü sayesinde sadece Node 2 OTA gönderimini başlatıyor:

```c
if(node_id == 2) {
    total_blocks = (FIRMWARE_PAYLOAD_LEN + OTA_BLOCK_SIZE - 1) / OTA_BLOCK_SIZE;
    // OTA gönderim burada başlıyor
}
Paket Yapısı

OTA haberleşmesinde sabit boyutlu bir paket yapısı kullanıldı. Bu yapı ota_packet.h içinde tanımlı:

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint16_t block_id;
    uint8_t  length;
    uint8_t  data[OTA_BLOCK_SIZE];
    uint16_t checksum;
} ota_packet_t;
type: Paket türü (DATA, ACK, NACK, DONE)
block_id: Veri bloğunun numarası
length: Veri uzunluğu
data: Firmware verisi
checksum: CRC-16 kontrol değeri
Firmware Hazırlama Süreci

Başta tüm firmware dosyası doğrudan gönderilmeye çalışıldı ancak Z1 platformunun flash sınırı nedeniyle:

program too large

hatası alındı.

Bu yüzden firmware binary’e çevrilip sadece ilk 4096 byte kullanıldı:

msp430-objcopy -O binary new-firmware.z1 firmware.bin
dd if=firmware.bin of=firmware_chunk.bin bs=1 count=4096
xxd -i firmware_chunk.bin > firmware_data.h

Burada veriyi static const uint8_t olarak tutmak gerekiyordu. Aksi halde RAM taşması oluşuyordu:

region `ram' overflowed
Gönderim Mantığı

OTA aktarımı bloklar halinde yapılır:

#define OTA_BLOCK_SIZE     64
#define OTA_MAX_RETRIES     5
#define OTA_RETRY_INTERVAL  (CLOCK_SECOND * 2)

Akış şu şekildedir:

Gönderici paketi yollar
Alıcı CRC-16 kontrolü yapar
Doğruysa ACK gönderir
Hatalıysa NACK gönderir
Gönderici aynı bloğu tekrar yollar (max 5 deneme)

Başlangıçta 5 saniye olan bekleme süresi toplam transfer süresini 300 saniyeyi aştığı için bağlantı kesiliyordu. Bu yüzden 2 saniyeye düşürüldü ve problem çözüldü.

Veri Bütünlüğü

İki aşamalı kontrol kullanıldı:

CRC-16: Her blok için kontrol edilir, hata varsa tekrar gönderim yapılır
CRC-32: Tüm firmware indirildikten sonra dosya bütünlüğü doğrulanır
Dosya Kaydetme

Alıcı düğüm gelen veriyi Contiki-NG Coffee File System (CFS) ile diske yazar:

cfs_fd = cfs_open("firmware.bin", CFS_WRITE);
cfs_write(cfs_fd, work.data, work.length);
cfs_close(cfs_fd);
Simülasyon Sonucu
Firmware size : 4096 byte
Block size    : 64 byte
Total blocks  : 64

✓ Tüm bloklar alındı
✓ CRC32 doğrulaması başarılı (0x9BDCF5EF)
✓ firmware.bin oluşturuldu
