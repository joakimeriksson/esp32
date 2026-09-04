/* nullnet-xlevel: cross-level broadcast probe (E6 stage 1).
 * Same source builds for the ESP32-C6 (Contiki-NG on ESP-IDF) and for
 * sky/cooja targets in Contiki-NG. Payload is a fixed 4-byte little-endian
 * counter so 16-bit and 32-bit platforms interoperate; every received
 * frame is logged with its length, source and counter. */
#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "sys/log.h"
#include <string.h>
#include <stdint.h>
#define LOG_MODULE "XLVL"
#define LOG_LEVEL LOG_LEVEL_INFO
#ifndef XLEVEL_SEND_INTERVAL
#define XLEVEL_SEND_INTERVAL (5 * CLOCK_SECOND)
#endif
PROCESS(nullnet_xlevel_process, "nullnet cross-level probe");
AUTOSTART_PROCESSES(&nullnet_xlevel_process);
static uint8_t payload[4];
static void
input_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  uint32_t count = 0;
  const uint8_t *b = data;
  if(len >= 4) {
    count = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  }
  LOG_INFO("rx len %u count %lu from ", (unsigned)len, (unsigned long)count);
  LOG_INFO_LLADDR(src);
  LOG_INFO_("\n");
}
PROCESS_THREAD(nullnet_xlevel_process, ev, data)
{
  static struct etimer periodic_timer;
  static uint32_t count = 0;
  PROCESS_BEGIN();
  nullnet_buf = payload;
  nullnet_len = sizeof(payload);
  nullnet_set_input_callback(input_callback);
  LOG_INFO("started, interval %lu ticks, addr ", (unsigned long)XLEVEL_SEND_INTERVAL);
  LOG_INFO_LLADDR(&linkaddr_node_addr);
  LOG_INFO_("\n");
  etimer_set(&periodic_timer, XLEVEL_SEND_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
    payload[0] = count & 0xff; payload[1] = (count >> 8) & 0xff;
    payload[2] = (count >> 16) & 0xff; payload[3] = (count >> 24) & 0xff;
    nullnet_len = sizeof(payload);
    LOG_INFO("tx count %lu\n", (unsigned long)count);
    NETSTACK_NETWORK.output(NULL);
    count++;
    etimer_reset(&periodic_timer);
  }
  PROCESS_END();
}
