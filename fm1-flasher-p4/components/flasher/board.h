/* SPDX-License-Identifier: GPL-3.0-only */
/* What the board gives the flasher: the USB host port the FM-1 goes on and the SD card. Each app
 * has a board.c for its board (fm1-flasher-p4/main/board.c: the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5
 * through its BSP); WiFi through the C6 (ESP-Hosted) is configured in the app's sdkconfig. */
#pragma once
#include "esp_err.h"
#include "sdmmc_cmd.h"

#define BOARD_SD_MOUNT_POINT "/sdcard"

/* the USB Host Library started on the port, with its power on */
void board_usb_host_start(void);
/* the card mounted at mount_point, once; never formats. On failure nothing is left held (a later
 * try can succeed when a card is put in) */
esp_err_t board_sd_mount(const char *mount_point, sdmmc_card_t **card);
