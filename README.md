# BIL304-OTA-Project
# BIL304 — OTA (Over-The-Air) Firmware Güncelleme Projesi

## 📹 Demo Videosu

[YouTube video linki buraya eklenecek]

---

## Proje Hakkında

Bu projede Contiki-NG işletim sistemi üzerinde çalışan Z1 (MSP430F2617) düğümleri arasında bir OTA firmware aktarım mekanizması geliştirilmiştir. Cooja simülatörü kullanılarak üç düğümlü bir senaryo kurulmuştur.

| Düğüm | Rol | Firmware |
|-------|-----|----------|
| Node 1 | Alıcı — RPL ağ kökü | udp-server.z1 |
| Node 2 | Gönderici | udp-client.z1 |
| Node 3 | Komşu / yönlendirici | udp-client.z1 |

Node 2 ve Node 3'e aynı firmware yüklenmektedir. Kaynak kodunda `node_id == 2` koşuluyla gönderim işlemi yalnızca Node 2 tarafından yürütülür:

```c
if(node_id == 2) {
    // OTA gönderim işlemi buradan başlar
    total_blocks = (FIRMWARE_PAYLOAD_LEN + OTA_BLOCK_SIZE - 1) / OTA_BLOCK_SIZE;
}
```

---

## Paket Formatı

OTA transferinde her paket sabit boyutlu bir yapı taşır. Bu yapı `ota_packet.h` içinde tanımlanmıştır:

```c
typedef struct __attribute__((packed)) {
    uint8_t  type;                   // Paket türü
    uint16_t block_id;               // Blok sıra numarası
    uint8_t  length;                 // Taşınan veri uzunluğu
    uint8_t  data[OTA_BLOCK_SIZE];   // Firmware verisi (64 byte)
    uint16_t checksum;               // CRC-16 bütünlük kodu
} ota_packet_t;
```

| Alan | Boyut | Açıklama |
|------|-------|----------|
| type | 1 byte | DATA (0x01), ACK (0x02), NACK (0x03), DONE (0x04) |
| block_id | 2 byte | Blok sıra numarası, 0'dan başlar |
| length | 1 byte | Bu blokta taşınan gerçek veri miktarı (max 64 byte) |
| data | 64 byte | Firmware verisi |
| checksum | 2 byte | CRC-16/CCITT paket bütünlük kodu |

---

## Firmware İmajının Hazırlanması

Aktarılacak `new-firmware.z1` dosyası ELF formatında olup disk üzerinde yaklaşık 129.760 byte yer kaplamaktadır. Bu dosyanın tamamını gönderici düğümün içine gömmek denenmiş ancak Z1'in ~92 KB'lık flash sınırı nedeniyle şu hatayla karşılaşılmıştır:

```
program too large
```

Bunun nedeni ELF dosyasının büyük bölümünün debug sembolleri ve sembol tablosundan oluşmasıdır. Gerçek çalıştırılabilir içerik yaklaşık 72 KB'dır. Bu nedenle aşağıdaki adımlar izlenmiştir:

**1. ELF'ten ham binary üretildi:**
```bash
msp430-objcopy -O binary new-firmware.z1 firmware.bin
# Sonuç: 72.056 byte
```

**2. İlk 4096 byte alındı:**
```bash
dd if=firmware.bin of=firmware_chunk.bin bs=1 count=4096
```

**3. Hex dizisine dönüştürüldü:**
```bash
xxd -i firmware_chunk.bin | sed 's/firmware_chunk_bin/firmware_payload/g' > firmware_data.h
```

Üretilen dizide `static const uint8_t` kullanılması kritik önem taşımaktadır. `const` niteleyicisi olmadan dizi RAM'e yerleşir ve şu hata alınır:

```
region `ram' overflowed by 2184 bytes
```

`const` ile dizi flash'a yerleşir ve sorun çözülür.

---

## Gönderim Protokolü

Firmware verisi 64 byte'lık bloklara bölünmüştür. Her blok için şu adımlar uygulanır:

1. Blok verisi pakete kopyalanır
2. Paket üzerinden CRC-16 hesaplanır
3. UDP ile alıcıya gönderilir
4. ACK beklenir; gelmezse yeniden iletilir

```c
#define OTA_BLOCK_SIZE    64
#define OTA_MAX_RETRIES   5
#define OTA_RETRY_INTERVAL (CLOCK_SECOND * 2)
```

Başlangıçta bekleme süresi 5 saniye olarak ayarlanmıştı. 64 blok × 5 saniye = 320 saniye, sunucunun 300 saniyelik zaman aşımını aşıyordu ve blok 45'ten sonra transfer kesiliyordu. Bekleme süresi 2 saniyeye indirilince sorun çözüldü.

**Yeniden iletim akışı:**

```
Gönderici                        Alıcı
    |                               |
    |---[block_id, data, CRC-16]--->|
    |                               | CRC-16 doğrula
    |<----------[ACK]---------------|
    |                               |
    | (CRC hatalıysa)               |
    |<----------[NACK]--------------|
    |                               |
    | (5 denemede ACK gelmezse)     |
    | → transfer durdurulur         |
```

---

## Bütünlük Doğrulaması

Projede iki ayrı katmanda bütünlük kontrolü uygulanmıştır:

### CRC-16 — Blok Doğrulama

Her blok gönderilmeden önce CRC-16/CCITT algoritmasıyla hesaplanır. Alıcı gelen paketi aynı algoritmayla kontrol eder; uyuşmazlık varsa NACK gönderir.

```c
static inline uint16_t ota_crc16(const uint8_t *buf, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}
```

### CRC-32 — İmaj Doğrulama

Tüm bloklar alındıktan sonra, kaydedilen dosya baştan sona okunarak CRC-32 hesaplanır. Bu değer, gönderici tarafından iletilen beklenen değerle karşılaştırılır. Uyuşmazlık varsa dosya silinir.

```c
static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint16_t len) {
    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc >>= 1;
        }
    }
    return ~crc;
}
```

---

## Kalıcı Depolama — Coffee File System (CFS)

Alıcı düğüm, gelen blokları Contiki-NG'nin Coffee dosya sistemi (CFS) aracılığıyla `firmware.bin` adlı dosyaya yazar.

```c
cfs_fd = cfs_open(OTA_FILENAME, CFS_WRITE);

// Her blok geldiğinde:
written = cfs_write(cfs_fd, work.data, work.length);

// Transfer tamamlandığında:
cfs_close(cfs_fd);
```

---

## Simülasyon Sonucu

Cooja simülatöründe gerçekleştirilen testte 64 blokun tamamı kayıpsız iletilmiş, CRC-32 doğrulaması geçilmiş ve firmware CFS'e kalıcı olarak kaydedilmiştir:

```
[INFO: App] Firmware array: 4096 byte, 64 blok
[INFO: App] [0/64]  TX len=64 deneme=1
[INFO: App] Blok 0 OK | 64 byte
...
[INFO: App] [63/64] TX len=64 deneme=1
[INFO: App] Blok 63 OK | 4096 byte
[INFO: App] === OTA TAMAMLANDI ===
[INFO: App] DONE alindi.
[INFO: App] === IMAJ DOGRULAMA ===
[INFO: App] Yuklenmeye hazir yeni firmware alimi tamamlandi.
[INFO: App] Dosya: firmware.bin  Boyut: 4096  CRC32: 0x9BDCF5EF
```
