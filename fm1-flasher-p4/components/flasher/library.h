/* SPDX-License-Identifier: GPL-3.0-only */
/* The library the FM-1 browses on its own screen: a table the P4 patches into the browser probe's
 * .data before the probe is sent (fm1_dock_probe_find / fm1_dock_probe_patch), the probe's run()
 * returning the pick. The probe (fm1-dock's probes/browser.c) keeps this layout; both sides are
 * little endian, the struct packed by hand. */
#pragma once
#include <stdint.h>

#define LIB_MAGIC 0x424C3450u   /* "P4LB" */
#define LIB_HEAD 8              /* magic u32, cap u16 (entries the probe has room for), n u16 (filled) */
#define LIB_ENTRY 32            /* kind u8, flags u8, size_kb u16, name[20], version[8] (both NUL-padded) */

enum { LIB_MODULE = 0, LIB_FIRMWARE = 1 };
enum {
    LIB_F_FITS = 1,         /* a module: fm1-dock has room for it now */
    LIB_F_CARD = 2,         /* on the P4's card (installs without WiFi) */
    LIB_F_INSTALLED = 4,    /* a firmware: what the FM-1 runs now */
    LIB_F_BETA = 8,
    LIB_F_OFFICIAL = 16,    /* M-VAVE's own */
};

/* run() returns 0 (nothing picked, the browser left), or 1 + the index of the pick. For a firmware
 * the probe asks for the risk to be accepted on the FM-1 before it returns the pick. */
#define LIB_RUN_ID 0x50344C42u  /* the RUN id the P4 uses */
