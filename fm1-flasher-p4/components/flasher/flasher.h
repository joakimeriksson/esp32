/* SPDX-License-Identifier: GPL-3.0-only */
/* The worker: one task owns the USB link and does everything slow (the FM-1's identity when it is
 * plugged in, the catalog, a package's download and checks, the install, WiFi), so the screen stays
 * live. The UI asks with flasher_*; the answers come back through ui.h. */
#pragma once
#include <stdbool.h>
#include "fm1_ota.h"
#include "fm1_dock.h"
#include "catalog.h"

typedef enum {
    DEV_NONE,      /* nothing on the USB port */
    DEV_PROBING,
    DEV_FM1,       /* an FM-1 (or SMK-37) answering the updater */
    DEV_LOADER,    /* one in update mode: an install was cut off */
    DEV_OTHER,     /* a MIDI device that does not answer as one */
} dev_state_t;

typedef struct {
    dev_state_t state;
    char usb_name[32];
    fm1_identity_t id;
    bool dock;                 /* it runs fm1-dock (answers its deploy protocol) */
    fm1_dock_hello_t hello;
} dev_info_t;

void flasher_start(void);
void flasher_refresh(void);                                 /* the catalog and the SD card */
void flasher_prepare(const fw_entry_t *entry);              /* fetch and check one */
void flasher_install(void);                                 /* the one prepared, on the FM-1 */
void flasher_save(const fw_entry_t *entry);                 /* onto the card's mirror */
void flasher_remove(const fw_entry_t *entry);               /* off it */
void flasher_save_all(void);                                /* every package onto the card; again: stop */
bool flasher_saving(void);
void flasher_wifi_scan(void);
void flasher_wifi_connect(const char *ssid, const char *pass);
/* fm1-dock's modules (the safe mode) */
void flasher_mod_deploy(const module_entry_t *m);           /* onto the FM-1 */
void flasher_mod_save(const module_entry_t *m);             /* onto the card */
void flasher_mod_remove(const module_entry_t *m);
void flasher_dock_select(const char *name);                 /* play the FM-1's module */
void flasher_dock_delete(const char *name);
void flasher_dock_list(void);
/* the FM-1's modules as last listed (copied): their number */
int flasher_dock_modules(fm1_dock_module_t *out, int max);
/* the device as last seen (copied) */
void flasher_device(dev_info_t *out);

/* The library: the FM-1 browses the P4 on its own screen. The browser, a probe (fm1-dock runs it
 * from RAM, nothing written), is sent to an fm1-dock once when it is plugged in, the list patched
 * in (library.h); its pick is deployed or installed. Again on the next plug-in, or flasher_browse.
 * The probe: flasher_library_probe's, else /sdcard/fm1/browser.dock. */
void flasher_library(bool on);
void flasher_library_probe(const uint8_t *blob, size_t n);
void flasher_browse(void);
