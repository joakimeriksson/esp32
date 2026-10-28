/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-flasher-p4: installs FM-1 firmwares (the Firmware Hub's, fm1-dock, the official one, the SD
 * card's) from the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5, the FM-1 on the P4's USB host port. The
 * board through Waveshare's BSP, the display turned to landscape as in scintilla-p4-lcd35. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_ops.h"
#include "esp_system.h"
#include "flasher.h"
#include "net.h"
#include "ui.h"

/* Landscape, 480 x 320: the panel turned (rows and columns exchanged, both axes mirrored) and touch
 * to match; scintilla-p4-lcd35/main/main.c has the reasoning. FLASHER_LANDSCAPE_FLIP turns it the
 * other way. */
#ifndef FLASHER_LANDSCAPE_FLIP
#define FLASHER_LANDSCAPE_FLIP 0
#endif
static void display_start_landscape(void)
{
    const esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&acfg));
    ESP_ERROR_CHECK(bsp_display_brightness_init());
    bsp_lcd_handles_t h;
    ESP_ERROR_CHECK(bsp_display_new_with_handles(NULL, &h));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(h.panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(h.panel, !FLASHER_LANDSCAPE_FLIP, !FLASHER_LANDSCAPE_FLIP));
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
    const bsp_touch_config_t tcfg = { .flags = { .swap_xy = 1, .mirror_x = FLASHER_LANDSCAPE_FLIP, .mirror_y = !FLASHER_LANDSCAPE_FLIP } };
    esp_lcd_touch_handle_t tp;
    ESP_ERROR_CHECK(bsp_touch_new(&tcfg, &tp));
    const esp_lv_adapter_touch_config_t touch = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    assert(esp_lv_adapter_register_touch(&touch));
    ESP_ERROR_CHECK(esp_lv_adapter_start());
}

/* why the board started: a brownout points at the power (the FM-1 and the screen on one USB port) */
static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_BROWNOUT: return "BROWNOUT (the supply dipped)";
    case ESP_RST_PANIC: return "a crash (panic)";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "a watchdog";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_EXT: return "the reset pin";
    default: return "other";
    }
}

void app_main(void)
{
    printf("fm1-flasher: started after %s\n", reset_reason());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    display_start_landscape();
    bsp_display_backlight_on();
    esp_lv_adapter_lock(-1);   /* -1 waits; 0 would not */
    ui_build();
    bsp_display_unlock();
    net_start();
    flasher_start();
    printf("fm1-flasher: ready\n");
}
