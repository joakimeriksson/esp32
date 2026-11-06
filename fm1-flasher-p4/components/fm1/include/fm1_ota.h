/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 firmware install over USB-MIDI (the Hub's, fm1-dock's and Felucca's updater): step 1,
 * the running firmware reads parts of the package and starts the update loader; step 2, the loader
 * reads the whole image and writes it; then the FM-1 restarts and its identity is checked. In both
 * steps the device asks and we answer. One device on the link (the P4's USB host port). */
#pragma once
#include "fm1_pkg.h"

/* the link to the device; the P4's is usb_midi.c, test/ has a simulated FM-1 */
typedef struct {
    bool (*up)(void);                                /* a MIDI device is there */
    bool (*send)(const uint8_t *sysex, size_t n);    /* false: the device is gone */
    int (*read)(uint8_t *buf, size_t cap, uint32_t ms);   /* a SysEx's length; 0 timeout; -1 gone */
    void (*drain)(void);
    void (*sleep_ms)(uint32_t ms);
    uint32_t (*now_ms)(void);
} fm1_io_t;

typedef enum {
    FM1_OK = 0,
    FM1_ERR_NOTFOUND,   /* no FM-1 answered */
    FM1_ERR_MODEL,      /* the package is for another keyboard */
    FM1_ERR_BOARD,      /* the package is laid out for another board */
    FM1_ERR_FOREIGN,    /* the device is in another firmware's update mode */
    FM1_ERR_STOPPED,    /* the device stopped asking */
    FM1_ERR_LOST,       /* the device was disconnected */
    FM1_ERR_BADREQ,     /* the device asked for something outside the package */
    FM1_ERR_NOLOADER,   /* the update loader did not appear */
    FM1_ERR_NORETURN,   /* the device did not come back after the write */
    FM1_ERR_MISMATCH,   /* written, but the device reports another identity */
} fm1_err_t;

typedef enum {
    FM1_STEP_START,     /* text: the device */
    FM1_STEP_CHECK,     /* n: requests served */
    FM1_STEP_LOADER,
    FM1_STEP_WRITE,     /* n: percent */
    FM1_STEP_REBOOT,
    FM1_STEP_DONE,      /* text: the new identity */
} fm1_step_t;

typedef void (*fm1_progress_t)(fm1_step_t step, int n, const char *text, void *arg);

typedef struct {
    uint32_t start, reply, loader, reboot, retry, idle_check, idle_write, wait_loader, wait_reboot, hs;
} fm1_delays_t;
/* fm1_install.py's (ms); test/ shortens them */
extern fm1_delays_t fm1_delay;

/* the device's identity, or false when nothing answers */
bool fm1_handshake(const fm1_io_t *io, fm1_identity_t *id, int tries);

/* the whole install of image (fm1_logical_image's) with identity product ("FM-1_975"); a device
 * already in Felucca's update mode only gets the write. msg: why it stopped, for the screen */
fm1_err_t fm1_install(const fm1_io_t *io, const uint8_t *image, size_t n, const char *product,
                      fm1_progress_t progress, void *arg, char *msg, size_t msglen);
