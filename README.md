# BIL304 — OTA Firmware Projesi

## Demo Videosu

[YouTube video linki buraya eklenecek]

---

## Proje Hakkında

Contiki-NG ve Cooja kullanılarak Z1 düğümleri arasında OTA firmware aktarım mekanizması geliştirildi. Üç düğümlü bir simülasyon kuruldu:

| Düğüm | Rol | Firmware |
|-------|-----|----------|
| Node 1 | Alıcı — RPL ağ kökü | udp-server.z1 |
| Node 2 | Gönderici | udp-client.z1 |
| Node 3 | Komşu / yönlendirici | udp-client.z1 |

Node 2 ve Node 3'e aynı firmware yüklenmektedir. Aynı firmware kullanılmasına rağmen `node_id` kontrolü sayesinde yalnızca Node 2 OTA göndericisi olarak çalışmaktadır:

```c
if(node_id == 2) {
    total_blocks = (FIRMWARE_PAYLOAD_LEN + OTA_BLOCK_SIZE - 1) / OTA_BLOCK_SIZE;
    // OTA gönderim buradan başlar
}
```

---

## Paket Formatı

Her OTA paketi `ota_packet.h` içinde tanımlanmış sabit boyutlu bir yapı taşır:

```c
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint16_t block_id;
    uint8_t  length;
    uint8_t  data[OTA_BLOCK_SIZE];
    uint16_t checksum;
} ota_packet_t;
```

| Alan | Açıklama |
|------|----------|
| type | DATA, ACK, NACK veya DONE |
| block_id | Blok numarası |
| length | Veri uzunluğu |
| data | Firmware verisi (64 byte) |
| checksum | CRC-16 değeri |

---

## Firmware Hazırlama

Başlangıçta `new-firmware.z1` dosyasının tamamını (129.760 byte) gönderici düğümün içine gömmek denendi, ancak Z1 platformunun flash sınırı nedeniyle şu hata alındı:

```
program too large
```

Bunun üzerine firmware ELF dosyası binary formata dönüştürüldü ve ilk 4096 byte'lık kısım kullanılarak OTA mekanizması test edildi:

```bash
msp430-objcopy -O binary new-firmware.z1 firmware.bin
dd if=firmware.bin of=firmware_chunk.bin bs=1 count=4096
xxd -i firmware_chunk.bin | sed 's/firmware_chunk_bin/firmware_payload/g' > firmware_data.h
```

Üretilen dizide `static const uint8_t` kullanmak zorunluydu; aksi durumda dizi RAM'e yerleşiyor ve şu hata alınıyordu:

```
region `ram' overflowed by 2184 bytes
```

---

## Gönderim Protokolü

```
#define OTA_BLOCK_SIZE     64
#define OTA_MAX_RETRIES     5
#define OTA_RETRY_INTERVAL  (CLOCK_SECOND * 2)
```

Her blok için akış şu şekilde işler:

```
Gönderici                   Alıcı
    |                          |
    |---[block_id, data, CRC]->|
    |                          | CRC-16 doğrula
    |<--------[ACK]------------|
    |                          |
    | (hata varsa)             |
    |<--------[NACK]-----------|
    | → aynı blok tekrar gönderilir (max 5 deneme)
```

Başlangıçta bekleme süresi 5 saniye olarak ayarlanmıştı. 64 blok × 5 saniye = 320 saniye, sunucunun 300 saniyelik zaman aşımını geçiyordu ve blok 45'ten itibaren transfer kesiliyordu. Süre 2 saniyeye indirilerek sorun çözüldü.

---

## Bütünlük Doğrulaması

İki ayrı katmanda bütünlük kontrolü yapılmaktadır:

- **CRC-16** — Her blok gönderilmeden önce hesaplanır, alıcı tarafında doğrulanır. Uyuşmazlık varsa NACK gönderilir.
- **CRC-32** — Tüm bloklar alındıktan sonra kaydedilen dosya baştan sona okunarak doğrulanır. Uyuşmazlık varsa dosya silinir.

---

## Depolama

Alıcı düğüm gelen blokları Contiki-NG'nin Coffee dosya sistemi (CFS) aracılığıyla `firmware.bin` adlı dosyaya yazar:

```c
cfs_fd = cfs_open("firmware.bin", CFS_WRITE);
cfs_write(cfs_fd, work.data, work.length);
cfs_close(cfs_fd);
```

---

## Simülasyon Sonucu

```
Firmware size : 4096 byte
Block size    : 64 byte
Total blocks  : 64

✓ Tüm bloklar alındı
✓ CRC32 doğrulaması geçildi (0x9BDCF5EF)
✓ firmware.bin başarıyla yazıldı
```
