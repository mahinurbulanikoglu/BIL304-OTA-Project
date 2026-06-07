#ifndef OTA_PACKET_H
#define OTA_PACKET_H

#include <stdint.h>

#define OTA_BLOCK_SIZE      64
#define UDP_SERVER_PORT     5678
#define UDP_CLIENT_PORT     8765
#define OTA_MAX_RETRIES     5
#define OTA_RETRY_INTERVAL  (CLOCK_SECOND * 2)

#define OTA_PKT_DATA   0x01
#define OTA_PKT_ACK    0x02
#define OTA_PKT_NACK   0x03
#define OTA_PKT_DONE   0x04

typedef struct __attribute__((packed)) {
  uint8_t  type;
  uint16_t block_id;
  uint8_t  length;
  uint8_t  data[OTA_BLOCK_SIZE];
  uint16_t checksum;
} ota_packet_t;

static inline uint16_t
ota_crc16(const uint8_t *buf, uint16_t len)
{
  uint16_t crc = 0xFFFF;
  uint16_t i;
  int bit;
  for(i = 0; i < len; i++) {
    crc ^= (uint16_t)buf[i] << 8;
    for(bit = 0; bit < 8; bit++) {
      if(crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else             crc <<= 1;
    }
  }
  return crc;
}

static inline uint16_t
ota_packet_crc(const ota_packet_t *p)
{
  return ota_crc16((const uint8_t *)p,
                   sizeof(p->type) + sizeof(p->block_id) +
                   sizeof(p->length) + OTA_BLOCK_SIZE);
}

#endif /* OTA_PACKET_H */
