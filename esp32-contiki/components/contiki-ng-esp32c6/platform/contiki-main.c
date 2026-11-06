#include <assert.h>
#include "contiki.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "contiki-net.h"
#include "dev/watchdog.h"
#include "esp_system.h"     /* esp_read_mac() */
#include "esp_mac.h"
#include "net/linkaddr.h"
#include "net/net-debug.h"

/*---------------------------------------------------------------------------*/
/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "Main"
#define LOG_LEVEL LOG_LEVEL_MAIN
/*---------------------------------------------------------------------------*/

void contiki_task(void *arg)
{
  clock_init();          /* your very first driver */
  rtimer_arch_init();          

  esp_task_wdt_add(NULL);

  vTaskDelay(pdMS_TO_TICKS(10));

  process_init();
  process_start(&etimer_process, NULL);

  ctimer_init();
  watchdog_init();

  /* Setup MAC address */
  uint8_t eui64[8];
  esp_read_mac(eui64, ESP_MAC_IEEE802154);
  /* Set ff fe in the middle */
  eui64[3] = 0xff;
  eui64[4] = 0xfe; /* IEEE 802.15.4 EUI-64 format */

  LOG_INFO("MAC address: [");
  net_debug_lladdr_print((const uip_lladdr_t *)eui64);
  LOG_INFO_("] \n\n");

  linkaddr_set_node_addr((linkaddr_t *)eui64); 

  netstack_init();

  LOG_INFO("Starting " CONTIKI_VERSION_STRING "\n");
  LOG_DBG("TARGET=%s", CONTIKI_TARGET_STRING);
  LOG_DBG_("\n");
  LOG_INFO("- Routing: %s\n", NETSTACK_ROUTING.name);
  LOG_INFO("- Net: %s\n", NETSTACK_NETWORK.name);
  LOG_INFO("- MAC: %s\n", NETSTACK_MAC.name);
  LOG_INFO("- 802.15.4 Channel: %u\n", IEEE802154_DEFAULT_CHANNEL);
  LOG_INFO("- 802.15.4 PANID: 0x%04x\n", IEEE802154_CONF_PANID);

#if NETSTACK_CONF_WITH_IPV6
  {
    uip_ds6_addr_t *lladdr;
    memcpy(&uip_lladdr.addr, &linkaddr_node_addr, sizeof(uip_lladdr.addr));
    process_start(&tcpip_process, NULL);

    lladdr = uip_ds6_get_link_local(-1);
    LOG_INFO("Tentative link-local IPv6 address: ");
    LOG_INFO_6ADDR(lladdr != NULL ? &lladdr->ipaddr : NULL);
    LOG_INFO_("\n");
  }
#endif /* NETSTACK_CONF_WITH_IPV6 */

  /* Start the Contiki processes */
  autostart_start(autostart_processes);


  watchdog_start();
  while(1) {
    /* run all pending events until none are left */
    while(process_run()) ;

    /* drive the e-timer engine so software timers expire */
    etimer_request_poll();

    /* feed the watchdog                                  */
    watchdog_periodic();

    /* let the idle task run (and save power)             */
    vTaskDelay(pdMS_TO_TICKS(10));  /* 10 ms ≈ 100 Hz scheduler tick   */
  }
}

// Put this in the other "default" app_main function
//void app_main(void)
//{
//  xTaskCreate(contiki_task, "contiki", 8192, NULL, 5, NULL);
//  ESP_LOGI(TAG, "Contiki task started");
//}