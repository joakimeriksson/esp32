/* SPDX-License-Identifier: GPL-3.0-only */
/* Scintilla on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5: the board through Waveshare's BSP, the
 * display turned to landscape, the codec at 44.1 kHz mono, the engine on core 1, the touch keyboard,
 * and MIDI over Bluetooth LE through the C6. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_ops.h"
#include "audio_task.h"
#include "sc_fixed.h"
#include "ui.h"
#include "events.h"
#include "ble_midi.h"

/* Landscape, 480 x 320. The LVGL adapter does not rotate SPI panels ("use esp_lcd_panel_swap_xy /
 * esp_lcd_panel_mirror in panel init"), and bsp_display_start registers 320 x 480, so this does its
 * steps with the panel turned: rows and columns exchanged, both axes mirrored (the BSP's portrait
 * mirrors X only), and touch swapped and mirrored to match. SC_LANDSCAPE_FLIP turns it the other way. */
#ifndef SC_LANDSCAPE_FLIP
#define SC_LANDSCAPE_FLIP 0
#endif
static void display_start_landscape(void)
{
    const esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&acfg));
    ESP_ERROR_CHECK(bsp_display_brightness_init());
    bsp_lcd_handles_t h;
    ESP_ERROR_CHECK(bsp_display_new_with_handles(NULL, &h));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(h.panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(h.panel, !SC_LANDSCAPE_FLIP, !SC_LANDSCAPE_FLIP));
    const esp_lv_adapter_display_config_t dcfg = {
        .panel = h.panel,
        .panel_io = h.io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_V_RES,
            .ver_res = BSP_LCD_H_RES,
            .buffer_height = 40,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&dcfg);
    assert(disp);
    /* esp_lcd_touch mirrors first, against the portrait size, then swaps: x = 479 - y0, y = x0 */
    const bsp_touch_config_t tcfg = { .flags = { .swap_xy = 1, .mirror_x = SC_LANDSCAPE_FLIP, .mirror_y = !SC_LANDSCAPE_FLIP } };
    esp_lcd_touch_handle_t tp;
    ESP_ERROR_CHECK(bsp_touch_new(&tcfg, &tp));
    const esp_lv_adapter_touch_config_t touch = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    assert(esp_lv_adapter_register_touch(&touch));
    ESP_ERROR_CHECK(esp_lv_adapter_start());
}

void app_main(void)
{
    nvs_flash_init();
    display_start_landscape();
    bsp_display_backlight_on();

    esp_codec_dev_handle_t codec = bsp_audio_codec_speaker_init();
    esp_codec_dev_sample_info_t fs = { .sample_rate = SC_FS, .channel = 1, .bits_per_sample = 16 };
    if (!codec || esp_codec_dev_open(codec, &fs) != 0) { printf("scintilla: no codec\n"); return; }
    esp_codec_dev_set_out_vol(codec, 85);
    events_init();
    audio_start(codec, 0);

    bsp_display_lock(0);
    ui_build(0);
    bsp_display_unlock();
    ble_midi_start("Scintilla");
    printf("scintilla: ready\n");
}
