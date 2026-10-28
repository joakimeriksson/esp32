/* SPDX-License-Identifier: GPL-3.0-only */
/* The screen (480 x 320 landscape, LVGL): the FM-1 on the USB port at the top; tabs with the
 * firmwares (fm1-dock, the official one, the Hub's), the SD card's packages and WiFi; a firmware's
 * page: fetch and check it, accept the risk, install, the steps as they go.
 * ui_build runs under the display lock; the others are called from the worker and take it. */
#pragma once
#include "flasher.h"
#include "net.h"

void ui_build(void);
void ui_device(const dev_info_t *d);
void ui_net(net_state_t st, const char *ssid);
void ui_catalog(bool ok, bool offline, const char *err);
/* a save (or a remove) of e: on the card now when ok */
void ui_saved(const fw_entry_t *e, bool on_card, const char *why);
void ui_save_progress(int done, int total, const char *name);
void ui_save_done(int saved, int failed, bool stopped);
void ui_download(size_t got, size_t total);
void ui_prepared(const fw_package_t *p, bool ok, const char *why);
void ui_install_step(fm1_step_t step, int n, const char *text);
void ui_install_done(fm1_err_t e, const char *msg);
/* the modules: fm1-dock's page's and the card's (ui_modules), the FM-1's (ui_dock), a deploy / play /
 * delete (ui_mod_progress, ui_mod_done), a save to the card (ui_mod_saved) */
void ui_modules(bool ok, bool offline, const char *err);
void ui_dock(void);
void ui_mod_progress(int pct);
void ui_mod_done(bool ok, const char *msg);
void ui_mod_saved(const module_entry_t *m, bool on_card, const char *why);
void ui_wifi_list(const wifi_ap_record_t *aps, int n);
/* the library (the FM-1 browsing the P4): what happens, one line at a time */
void ui_library(const char *text);
