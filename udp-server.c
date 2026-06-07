#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"
#include "cfs/cfs.h"
#include "ota_packet.h"
#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL  LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define OTA_FILENAME    "firmware.bin"

static struct simple_udp_connection udp_conn;
static volatile uint8_t cb_new_packet = 0;
static ota_packet_t     cb_pkt;
static uip_ipaddr_t     cb_sender;
static uint16_t         expected_block = 0;
static int              cfs_fd         = -1;
static uint32_t         image_crc32    = 0;
static uint32_t         image_size     = 0;
static uint8_t          ota_done       = 0;

static uint32_t
crc32_update(uint32_t crc, const uint8_t *buf, uint16_t len)
{
  uint16_t i; int bit;
  crc = ~crc;
  for(i = 0; i < len; i++) {
    crc ^= buf[i];
    for(bit = 0; bit < 8; bit++) {
      if(crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
      else         crc >>= 1;
    }
  }
  return ~crc;
}

static void
send_response(uint8_t type, uint16_t block_id)
{
  ota_packet_t resp;
  memset(&resp, 0, sizeof(resp));
  resp.type     = type;
  resp.block_id = block_id;
  resp.checksum = ota_packet_crc(&resp);
  simple_udp_sendto(&udp_conn, &resp, sizeof(resp), &cb_sender);
}

static uint8_t
verify_image(uint32_t expected_crc)
{
  int fd; uint8_t buf[OTA_BLOCK_SIZE]; int n; uint32_t crc = 0;
  fd = cfs_open(OTA_FILENAME, CFS_READ);
  if(fd < 0) return 0;
  while((n = cfs_read(fd, buf, sizeof(buf))) > 0)
    crc = crc32_update(crc, buf, (uint16_t)n);
  cfs_close(fd);
  return (crc == expected_crc) ? 1 : 0;
}

static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  const ota_packet_t *inc;
  if(datalen < sizeof(ota_packet_t)) return;
  if(cb_new_packet) {
    inc = (const ota_packet_t *)data;
    LOG_WARN("DROP blok=%u\n", inc->block_id);
    return;
  }
  memcpy(&cb_pkt, data, sizeof(ota_packet_t));
  uip_ipaddr_copy(&cb_sender, sender_addr);
  cb_new_packet = 1;
}

static void
process_packet(void)
{
  ota_packet_t work;
  uint16_t rx_crc, calc_crc;
  int written;

  memcpy(&work, &cb_pkt, sizeof(ota_packet_t));
  cb_new_packet = 0;

  if(work.type == OTA_PKT_DONE) {
    LOG_INFO("DONE alindi.\n");
    if(cfs_fd >= 0) { cfs_close(cfs_fd); cfs_fd = -1; }
    LOG_INFO("=== IMAJ DOGRULAMA ===\n");
    if(verify_image(image_crc32)) {
      LOG_INFO("================================================\n");
      LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
      LOG_INFO("Dosya: %s Boyut: %lu CRC32: 0x%08lX\n",
               OTA_FILENAME, (unsigned long)image_size,
               (unsigned long)image_crc32);
      LOG_INFO("================================================\n");
    } else {
      LOG_ERR("CRC32 UYUMSUZ.\n");
      cfs_remove(OTA_FILENAME);
    }
    ota_done = 1;
    return;
  }

  if(work.type != OTA_PKT_DATA) return;

  if(work.block_id != expected_block) {
    LOG_WARN("Sira disi beklenen=%u gelen=%u\n", expected_block, work.block_id);
    if(work.block_id < expected_block) send_response(OTA_PKT_ACK, work.block_id);
    else                               send_response(OTA_PKT_NACK, work.block_id);
    return;
  }

  if(work.length == 0 || work.length > OTA_BLOCK_SIZE) {
    send_response(OTA_PKT_NACK, work.block_id);
    return;
  }

  rx_crc        = work.checksum;
  work.checksum = 0;
  calc_crc      = ota_packet_crc(&work);
  work.checksum = rx_crc;

  if(rx_crc != calc_crc) {
    LOG_WARN("CRC16 hata blok %u\n", work.block_id);
    send_response(OTA_PKT_NACK, work.block_id);
    return;
  }

  written = cfs_write(cfs_fd, work.data, work.length);
  if(written != (int)work.length) {
    LOG_ERR("CFS yazma hatasi blok %u\n", work.block_id);
    return;
  }

  image_crc32 = crc32_update(image_crc32, work.data, work.length);
  image_size += work.length;
  expected_block++;
  LOG_INFO("Blok %u OK | %lu byte\n", work.block_id, (unsigned long)image_size);
  send_response(OTA_PKT_ACK, work.block_id);
}

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);

PROCESS_THREAD(udp_server_process, ev, data)
{
  static struct etimer poll_timer;
  static struct etimer timeout_timer;
  static uint8_t cfs_ok;

  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();
  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);

  cfs_remove(OTA_FILENAME);
  cfs_fd = cfs_open(OTA_FILENAME, CFS_WRITE);
  cfs_ok = (cfs_fd >= 0) ? 1 : 0;

  if(cfs_ok) {
    LOG_INFO("OTA alici hazir.\n");
    etimer_set(&timeout_timer, CLOCK_SECOND * 300);
    etimer_set(&poll_timer, CLOCK_SECOND / 4);

    while(!ota_done) {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&poll_timer) ||
                               etimer_expired(&timeout_timer));
      if(etimer_expired(&timeout_timer)) {
        LOG_ERR("OTA zaman asimi!\n");
        break;
      }
      if(cb_new_packet) {
        process_packet();
        etimer_reset(&timeout_timer);
      }
      etimer_reset(&poll_timer);
    }

    if(!ota_done) {
      LOG_ERR("OTA tamamlanamadi!\n");
    }
    if(cfs_fd >= 0) {
      cfs_close(cfs_fd);
      cfs_fd = -1;
    }
  } else {
    LOG_ERR("CFS acilamadi!\n");
  }

  PROCESS_END();
}
