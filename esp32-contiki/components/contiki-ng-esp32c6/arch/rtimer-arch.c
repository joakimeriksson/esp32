#include "contiki.h"
#include "rtimer-arch.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "rtimer";
static esp_timer_handle_t rtimer_hdl;

/* ---------- internal helpers ------------------------------------- */
static void IRAM_ATTR rtimer_alarm_cb(void *arg)
{
  /* Contiki expects this to run from interrupt context.
     esp_timer fires from a high-prio task, but that’s usually fine.
     If you later need real ISR, switch to GPTimer (Option B). */
  rtimer_run_next();
}

/* ---------- required API ----------------------------------------- */
void rtimer_arch_init(void)
{
  esp_timer_create_args_t args = {
    .callback = rtimer_alarm_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "contiki_rt"
  };
  esp_err_t err = esp_timer_create(&args, &rtimer_hdl);
  if(err != ESP_OK) {
    ESP_LOGE(TAG, "create failed (%d)", err);
  }
}

rtimer_clock_t rtimer_arch_now(void)
{
  /* esp_timer returns µs since boot */
  return (rtimer_clock_t)esp_timer_get_time();
}

void rtimer_arch_schedule(rtimer_clock_t t)
{
  rtimer_clock_t now = rtimer_arch_now();
  uint64_t delay_us  = (t > now) ? (t - now) : 1;   /* never 0 → fire asap */

  esp_timer_stop(rtimer_hdl);              /* cancel previous alarm      */
  esp_timer_start_once(rtimer_hdl, delay_us);
}