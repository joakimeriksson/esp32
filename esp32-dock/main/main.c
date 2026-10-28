/* SPDX-License-Identifier: GPL-3.0-only */
/* esp32-dock: fm1-flasher-p4 without a screen. The FM-1 on the P4's USB host port browses the
 * library (the Firmware Hub's firmwares, fm1-dock's modules, the SD card's copies) on its own
 * screen, through a browser the P4 sends it (a probe, run from the FM-1's RAM, nothing written);
 * what it picks is installed or deployed. A console on UART0 for the rest (WiFi, the lists, an
 * install by hand). */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "flasher.h"
#include "net.h"

void console_start(void);

#ifdef DOCK_BROWSER_EMBEDDED
extern const uint8_t browser_start[] asm("_binary_browser_dock_start");
extern const uint8_t browser_end[] asm("_binary_browser_dock_end");
#endif

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
    printf("esp32-dock: started after %s\n", reset_reason());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
#ifdef DOCK_BROWSER_EMBEDDED
    flasher_library_probe(browser_start, browser_end - browser_start);
#endif
    flasher_library(true);
    net_start();
    flasher_start();
    console_start();
    printf("esp32-dock: ready (help lists the commands)\n");
}
