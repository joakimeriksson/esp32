#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "ieee_scan.h"

#define TAG          "EDSCAN"

static QueueHandle_t s_result_q; // holds {uint8_t ch, int8_t pwr}

QueueHandle_t ieee_scan_get_queue(void) { return s_result_q; }

/* --- ISR callback from driver (weak symbol) ------------------------------ */
void IRAM_ATTR esp_ieee802154_energy_detect_done(int8_t power_dbm)
{
    static uint8_t cur_ch = CH_FIRST;
    ed_point_t pt = { .ch = cur_ch, .pwr = power_dbm };
    BaseType_t woke = pdFALSE;
    xQueueSendFromISR(s_result_q, &pt, &woke);

    /* go to next channel or finish */
    if (++cur_ch <= CH_LAST) {
        ESP_ERROR_CHECK(esp_ieee802154_set_channel(cur_ch));
        esp_ieee802154_energy_detect(ED_WIN_SYM);
    } else {
        cur_ch = CH_FIRST;
    }
    /* context-switch if we unblocked something higher priority */
    if (woke == pdTRUE) {
        portYIELD_FROM_ISR();    // <-- correct name, no argument
    }
}

/* --- radio sweeper task -------------------------------------------------- */
static void ieee_sweep_task(void *arg)
{
    esp_ieee802154_enable();                // power up radio
    for (;;) {
        /* kick off a fresh sweep */
        esp_ieee802154_set_channel(CH_FIRST);
        esp_ieee802154_energy_detect(ED_WIN_SYM);

        vTaskDelay(pdMS_TO_TICKS(20));     // 4 sweeps / second ≈ 64 ms radio time
    }
}

void ieee_scan_start(void)
{
    s_result_q = xQueueCreate(CH_LAST - CH_FIRST + 1, sizeof(ed_point_t));
    xTaskCreatePinnedToCore(ieee_sweep_task, "edscan", 4096, NULL, 5, NULL, 0);
}
