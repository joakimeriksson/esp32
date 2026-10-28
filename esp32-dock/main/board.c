/* SPDX-License-Identifier: GPL-3.0-only */
/* The Waveshare ESP32-P4-WIFI6 (no display), with plain ESP-IDF, no BSP: the P4's USB 2.0 OTG
 * high-speed port as the host (the 4-pin MX1.25 header; the FM-1 is powered from its VBUS), the TF
 * card on SDMMC slot 0 (the IO MUX pins, GPIO 39-44) behind a load switch on GPIO 45 (low: on).
 * The C6 is on SDIO slot 1 with its reset on GPIO 54: ESP-Hosted's defaults, in sdkconfig.
 *
 * Built with -DDOCK_BOARD=lcd35 it is the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 instead (the
 * screen left off): the same ports, the card powered from the chip's LDO 4. */
#include "board.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#ifdef DOCK_BOARD_LCD35
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "board";

/* the USB Host Library's own events (the clients are usb_midi.c's) */
static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) ESP_ERROR_CHECK(usb_host_device_free_all());
    }
}

void board_usb_host_start(void)
{
    const usb_host_config_t cfg = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL) != pdTRUE) abort();
}

esp_err_t board_sd_mount(const char *mount_point, sdmmc_card_t **card)
{
    const esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 64 * 1024 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;   /* the C6 is on slot 1: a failed mount deinits only this one */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   /* 20 MHz: 40 has failed to start a card on the LCD board */
#ifdef DOCK_BOARD_LCD35
    const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t pwr;
    esp_err_t r = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr);
    if (r != ESP_OK) return r;
    host.pwr_ctrl_handle = pwr;
#else
    static bool powered;
    if (!powered) {   /* SD_VDD_EN, low: the card gets its 3.3 V; a moment for it to come up */
        const gpio_config_t io = { .pin_bit_mask = 1ULL << 45, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(45, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        powered = true;
        ESP_LOGI(TAG, "TF card powered (GPIO 45 low)");
    }
#endif
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();   /* slot 0: the IO MUX pins, nothing to give */
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t r2 = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mc, card);
#ifdef DOCK_BOARD_LCD35
    if (r2 != ESP_OK) sd_pwr_ctrl_del_on_chip_ldo(pwr);   /* released, or no later try can get it */
#endif
    return r2;
}
