/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmwares to choose from: the FM-1 Firmware Hub's catalog (thefirmwarehub.com/catalog.json),
 * M-VAVE's official V15 as the Hub keeps it, fm1-dock's latest release on GitHub, and the .fwsc
 * files on the SD card. A chosen one is fetched into PSRAM and checked before it can be installed.
 * The card mirrors the Hub (/sdcard/fm1/hub): the catalog and every package fetched, for offline. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fm1_pkg.h"

typedef enum { SRC_HUB, SRC_STOCK, SRC_DOCK, SRC_SD } fw_source_t;

typedef struct {
    fw_source_t src;
    char id[32], name[40], version[32], author[40], kind[16], lic[16];
    char product[FM1_ID_MAX];   /* "FM-1_975", "" when not known before the package is read */
    char ident[40];             /* the Hub's identity pattern for the installed firmware ("FM-1_97[0-689]") */
    char what[320];
    char url[256];              /* or the SD card path */
    char local[256];            /* its copy on the card (the mirror), "" when it has no place there */
    bool on_card;               /* that copy is there */
    char sha256[65];            /* "" when not known */
    uint32_t size;
    bool beta;
} fw_entry_t;

#define CATALOG_MAX 160
extern fw_entry_t *catalog;     /* PSRAM */
extern int catalog_n;
extern char catalog_updated[32];   /* the Hub's "updated" of the catalog in use */

/* the Hub, the stock firmware and fm1-dock: from the net (and kept on the card), else the card's
 * copy (*offline); false with err when neither is there */
bool catalog_fetch(bool *offline, char *err, size_t errlen);
/* an SD card is there (mounted) */
bool catalog_card(void);
/* one package fetched, checked against the catalog (size, SHA-256) and saved to the card's mirror */
bool catalog_save(const fw_entry_t *e, void (*progress)(size_t, size_t), char *why, size_t whylen);
/* its copy on the card deleted */
bool catalog_remove(const fw_entry_t *e);
/* every catalog package not on the card yet, fetched and saved (checked against the catalog first);
 * the number saved, -1 without a card; stops when *stop */
int catalog_save_all(void (*progress)(int done, int total, const char *name), volatile bool *stop, int *failed);
/* the .fwsc on the SD card (its root and /fm1), replacing the earlier SD lines */
int catalog_scan_sd(void);
/* the entry whose firmware a device reporting identity runs (exact product, else the Hub's ident), or NULL */
const fw_entry_t *catalog_installed(const char *identity);

/* the package fetched and checked for installing */
typedef struct {
    fw_entry_t entry;           /* a copy: a catalog refresh moves the entries */
    uint8_t *img;               /* the logical image (PSRAM) */
    size_t n;
    char product[FM1_ID_MAX];
    bool official;              /* M-VAVE's unmodified V15, by its digest */
    bool felucca_loader;
    fm1_board_t board;
    char sha256[65];
    bool from_card;             /* read from the card, not downloaded */
} fw_package_t;

/* fetches entry (or reads it from the card), checks it: size and digest against the catalog, the
 * identity, the file CRCs, the update loader. false with why when it must not be installed */
bool package_prepare(const fw_entry_t *entry, fw_package_t *pkg, void (*progress)(size_t, size_t),
                     char *why, size_t whylen);
void package_free(fw_package_t *pkg);

/* ---------------------------------------------------------------------- fm1-dock's modules --- */

typedef struct {
    bool from_page;             /* fm1-dock's page lists it (else: a .dock found on the card) */
    char id[24], title[32], version[16], by[40];
    char source[192];
    char url[192];
    char local[160];            /* its place on the card */
    bool on_card;
    uint32_t size;
} module_entry_t;

#define MODULES_MAX 64
extern module_entry_t *modules;   /* PSRAM */
extern int modules_n;

/* fm1-dock's page's modules (from the net, kept on the card; else the card's list) and the card's .dock */
bool modules_fetch(bool *offline, char *err, size_t errlen);
/* the module's bytes (the card's copy first): their length, -1 with why. free() *blob */
int module_fetch(const module_entry_t *m, uint8_t **blob, void (*progress)(size_t, size_t), char *why, size_t whylen);
bool module_save(const module_entry_t *m, void (*progress)(size_t, size_t), char *why, size_t whylen);
bool module_remove(const module_entry_t *m);
