#include "contiki.h"
#include "esp_timer.h"

static uint64_t boot_us;

void clock_init(void)
{
  boot_us = esp_timer_get_time();
}

clock_time_t clock_time(void)                /* tick = 1 ms */
{
  return (clock_time_t)((esp_timer_get_time() - boot_us) / 1000);
}

unsigned long clock_seconds(void)
{
  return (unsigned long)(esp_timer_get_time() / 1000000ULL);
}

void clock_delay_usec(uint16_t dt) {         /* busy-wait */
  uint64_t target = esp_timer_get_time() + dt;
  while(esp_timer_get_time() < target) ;
}