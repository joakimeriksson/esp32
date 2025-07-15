#include "contiki.h"
#include "esp_task_wdt.h"
#include "dev/watchdog.h"

void watchdog_init(void)
{
}

void watchdog_start(void)
{
//  esp_task_wdt_init(5, true);  /* 5 seconds timeout, panic on timeout */
//  esp_task_wdt_add(NULL);       /* add the current task to the WDT */
}

void
watchdog_periodic(void)
{
  esp_task_wdt_reset();   /* feed IDF’s task WDT */
}