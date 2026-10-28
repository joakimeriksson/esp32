/* SPDX-License-Identifier: GPL-3.0-only */
/* The Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 through its BSP: the OTG high-speed port as the USB
 * host (the FM-1 is powered from it), the SD card on slot 0 with the chip's LDO 4. */
#include "board.h"
#include "bsp/esp-bsp.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

void board_usb_host_start(void)
{
    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
}

/* The BSP's bsp_sdcard_mount, but at the default 20 MHz (its 40 MHz high speed has failed to start
 * the card here: send_scr timing out) and with the card's LDO released when the mount fails (the
 * BSP keeps it, and every later try fails to get it). */
esp_err_t board_sd_mount(const char *mount_point, sdmmc_card_t **card)
{
    const esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 64 * 1024 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;   /* the C6's SDIO is slot 1: a failed mount deinits only this one */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t pwr;
    esp_err_t r = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr);
    if (r != ESP_OK) return r;
    host.pwr_ctrl_handle = pwr;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   /* the BSP has none; weak, but the card has its own */
    r = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mc, card);
    if (r != ESP_OK) sd_pwr_ctrl_del_on_chip_ldo(pwr);
    return r;
}
