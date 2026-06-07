#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/node-id.h"
#include "sys/log.h"
#include "cfs/cfs.h"
#include "ota_packet.h"
#include "firmware_data.h"   // ← KÜÇÜK OLAN (300 byte)
#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL  LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

static struct simple_udp_connection udp_conn;
static volatile int8_t ack_flag      = 0;
static uint16_t        current_block = 0;
static uint16_t        total_blocks  = 0;
static uint8_t         retries       = 0;
static ota_packet_t    tx_pkt;

static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  const ota_packet_t *r;
  if(datalen < sizeof(ota_packet_t)) return;
  r = (const ota_packet_t *)data;
  if(r->block_id != current_block) return;
  if(ack_flag != 0) return;
  if(r->type == OTA_PKT_ACK)  ack_flag =  1;
  if(r->type == OTA_PKT_NACK) ack_flag = -1;
}

/* Küçük array'den blok oluştur */
static uint8_t
build_packet_from_array(uint16_t blk)
{
  uint32_t offset = (uint32_t)blk * OTA_BLOCK_SIZE;
  uint8_t len = OTA_BLOCK_SIZE;
  if(offset + len > FIRMWARE_PAYLOAD_LEN)
    len = FIRMWARE_PAYLOAD_LEN - offset;

  memset(&tx_pkt, 0, sizeof(tx_pkt));
  tx_pkt.type     = OTA_PKT_DATA;
  tx_pkt.block_id = blk;
  tx_pkt.length   = len;
  memcpy(tx_pkt.data, firmware_payload + offset, len);
  tx_pkt.checksum = 0;
  tx_pkt.checksum = ota_packet_crc(&tx_pkt);
  return 1;
}

PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);

PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  static uint8_t finished;
  uip_ipaddr_t dest_ipaddr;

  PROCESS_BEGIN();

  finished = 0;
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  if(node_id == 2) {
    total_blocks = (FIRMWARE_PAYLOAD_LEN + OTA_BLOCK_SIZE - 1) / OTA_BLOCK_SIZE;
    LOG_INFO("Firmware array: %u byte, %u blok\n", FIRMWARE_PAYLOAD_LEN, total_blocks);
  }

  etimer_set(&periodic_timer, OTA_RETRY_INTERVAL);

  while(!finished) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(node_id != 2) {
      etimer_set(&periodic_timer, OTA_RETRY_INTERVAL);
      continue;
    }

    if(!NETSTACK_ROUTING.node_is_reachable() ||
       !NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
      LOG_INFO("RPL bekleniyor...\n");
      etimer_set(&periodic_timer, OTA_RETRY_INTERVAL);
      continue;
    }

    if(ack_flag == 1) {
      current_block++;
      retries  = 0;
      ack_flag = 0;
    } else if(ack_flag == -1) {
      ack_flag = 0;
    }

    if(current_block >= total_blocks) {
      memset(&tx_pkt, 0, sizeof(tx_pkt));
      tx_pkt.type     = OTA_PKT_DONE;
      tx_pkt.block_id = total_blocks;
      tx_pkt.checksum = ota_packet_crc(&tx_pkt);
      simple_udp_sendto(&udp_conn, &tx_pkt, sizeof(tx_pkt), &dest_ipaddr);
      LOG_INFO("=== OTA TAMAMLANDI ===\n");
      finished = 1;
      continue;
    }

    if(retries >= OTA_MAX_RETRIES) {
      LOG_ERR("Blok %u basarisiz!\n", current_block);
      finished = 1;
      continue;
    }

    if(!build_packet_from_array(current_block)) {
      LOG_ERR("Blok %u paketlenemedi!\n", current_block);
      finished = 1;
      continue;
    }

    LOG_INFO("[%u/%u] TX len=%u deneme=%u\n",
             current_block, total_blocks, tx_pkt.length, retries + 1);
    simple_udp_sendto(&udp_conn, &tx_pkt, sizeof(tx_pkt), &dest_ipaddr);
    retries++;
    etimer_set(&periodic_timer, OTA_RETRY_INTERVAL);
  }

  PROCESS_END();
}
