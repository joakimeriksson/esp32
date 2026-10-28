/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-dock's deploy protocol: a module (.dock) into a running fm1-dock, and the module list, select
 * and delete, after fm1-dock's tools/hbdeploy.py and firmware/src/dock.c. The safe mode: fm1-dock
 * checks a module's header, ABI and CRC before it runs it, and the firmware is not touched; boot
 * modules (they take over the FM-1 at power-on) are refused here.
 *
 *   frame   F0 7D 48 42 cmd seq pack7(payload + CRC-32 of the payload, little endian) F7
 *   reply   the same, cmd | 0x40 (0x3F: a frame it could not read), the same seq; a status byte first */
#pragma once
#include "fm1_ota.h"

enum {
    FM1_DOCK_HELLO = 0x01, FM1_DOCK_LIST = 0x02, FM1_DOCK_BEGIN = 0x10, FM1_DOCK_DATA = 0x11, FM1_DOCK_COMMIT = 0x12,
    FM1_DOCK_ABORT = 0x13, FM1_DOCK_DELETE = 0x14, FM1_DOCK_SELECT = 0x22,
};
#define FM1_DOCK_CHUNK 256
#define FM1_DOCK_PAYLOAD_MAX 300
#define FM1_DOCK_MAGIC 0x52425248u   /* "HRBR" */
#define FM1_DOCK_F_PROBE 0x100u   /* run once from RAM (tools/probe.py), not kept */
#define FM1_DOCK_F_BOOT 0x200u
#define FM1_DOCK_MODS_MAX 24

/* the frame for cmd into out (FM1_SYSEX_MAX), its length */
size_t fm1_dock_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t n, uint8_t *out);
/* a reply: its cmd (without 0x40), seq and payload (FM1_DOCK_PAYLOAD_MAX); false when it is not
 * fm1-dock's or is damaged */
bool fm1_dock_parse(const uint8_t *msg, size_t n, uint8_t *cmd, uint8_t *seq, uint8_t *payload, size_t *plen);
uint32_t fm1_crc32(const uint8_t *p, size_t n);
/* the reply status as text ("done", "bad CRC", ...) */
const char *fm1_dock_status(int status);

typedef struct {
    char name[24];
    uint32_t abi, flags, version;
} fm1_dock_info_t;
/* a .dock's header; false when it is not one */
bool fm1_dock_module_info(const uint8_t *blob, size_t n, fm1_dock_info_t *info);

typedef struct {
    uint8_t proto, abi, n_mods, cur;
    uint32_t largest, free, max_file;
} fm1_dock_hello_t;
/* fm1-dock answers: it runs; false when the FM-1 runs something else */
bool fm1_dock_hello(const fm1_io_t *io, fm1_dock_hello_t *h, int tries);

typedef struct {
    uint8_t index, state, kind;   /* kind: 0 built in, 1 image, 2 deployed, 3 boot */
    uint32_t version, size;
    char name[24];
} fm1_dock_module_t;
/* the modules on the FM-1: their number, -1 when it does not answer */
int fm1_dock_list(const fm1_io_t *io, fm1_dock_module_t *mods, int max);
const char *fm1_dock_state(int state);   /* "ok", "crashed", ... */

/* deploys the module: false with msg (for the screen) when it is refused or stops. progress: percent */
bool fm1_dock_deploy(const fm1_io_t *io, const uint8_t *blob, size_t n, void (*progress)(int pct, void *arg), void *arg,
                     char *msg, size_t msglen);
/* plays the module (SELECT), deletes it (DELETE): 0 done, else the status (fm1_dock_status), -1 no answer */
int fm1_dock_select(const fm1_io_t *io, const char *name);
int fm1_dock_delete(const fm1_io_t *io, const char *name);
