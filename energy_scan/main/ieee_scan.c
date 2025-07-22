#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "ieee_scan.h"

#define TAG          "EDSCAN"

static QueueHandle_t s_result_q; // holds {uint8_t ch, int8_t pwr}
static scan_mode_t s_scan_mode = SCAN_MODE_SWEEP;
uint8_t s_single_channel = CH_FIRST;

QueueHandle_t ieee_scan_get_queue(void) { return s_result_q; }

void ieee_scan_set_mode(scan_mode_t mode, uint8_t channel) {
    s_scan_mode = mode;
    if (s_scan_mode == SCAN_MODE_SINGLE_CHANNEL) {
        s_single_channel = channel;
    }
}

/* --- ISR callback from driver (weak symbol) ------------------------------ */
void IRAM_ATTR esp_ieee802154_energy_detect_done(int8_t power_dbm)
{
    static uint8_t cur_ch = CH_FIRST;
    ed_point_t pt;
    BaseType_t woke = pdFALSE;

    if (s_scan_mode == SCAN_MODE_SWEEP) {
        pt.ch = cur_ch;
        pt.pwr = power_dbm;
        xQueueSendFromISR(s_result_q, &pt, &woke);

        if (++cur_ch <= CH_LAST) {
            ESP_ERROR_CHECK(esp_ieee802154_set_channel(cur_ch));
            esp_ieee802154_energy_detect(ED_WIN_SYM);
        } else {
            cur_ch = CH_FIRST;
        }
    } else { // SCAN_MODE_SINGLE_CHANNEL
        pt.ch = s_single_channel;
        pt.pwr = power_dbm;
        xQueueSendFromISR(s_result_q, &pt, &woke);
        esp_ieee802154_energy_detect(ED_WIN_SYM);
    }

    if (woke == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* --- radio sweeper task -------------------------------------------------- */
static void ieee_sweep_task(void *arg)
{
    esp_ieee802154_enable();                // power up radio
    for (;;) {
        if (s_scan_mode == SCAN_MODE_SWEEP) {
            esp_ieee802154_set_channel(CH_FIRST);
        } else {
            esp_ieee802154_set_channel(s_single_channel);
        }
        esp_ieee802154_energy_detect(ED_WIN_SYM);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ieee_scan_start(void)
{
    s_result_q = xQueueCreate(CH_LAST - CH_FIRST + 1, sizeof(ed_point_t));
    xTaskCreatePinnedToCore(ieee_sweep_task, "edscan", 4096, NULL, 5, NULL, 0);
}
