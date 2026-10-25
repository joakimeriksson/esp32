/* SPDX-License-Identifier: GPL-3.0-only */
/* Scintilla on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5: the board through Waveshare's BSP, the
 * display turned to landscape, the codec at 44.1 kHz mono, the engine on core 1, the touch keyboard. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "audio_task.h"
#include "sc_fixed.h"
#include "ui.h"

void app_main(void)
{
    nvs_flash_init();
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    esp_codec_dev_handle_t codec = bsp_audio_codec_speaker_init();
    esp_codec_dev_sample_info_t fs = { .sample_rate = SC_FS, .channel = 1, .bits_per_sample = 16 };
    if (!codec || esp_codec_dev_open(codec, &fs) != 0) { printf("scintilla: no codec\n"); return; }
    esp_codec_dev_set_out_vol(codec, 85);
    audio_start(codec, 0);

    bsp_display_lock(0);
    ui_build(0);
    bsp_display_unlock();
    printf("scintilla: ready\n");
}
