/* SPDX-License-Identifier: GPL-3.0-only */
/* An FM-1 update package (.fwsc) and the SysEx of the FM-1's USB-MIDI updater, after fm1-dock's
 * tools/fm1_install.py and Felucca's web/fm1ota.js (GPL-3.0, Leo Kuroshita). Plain C, no ESP-IDF:
 * test/ builds it on the computer and checks it against the Python. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FM1_MAXDATA 512                  /* the largest read the device asks for */
#define FM1_FINISH_CHECK 0xE0000000u     /* the address that ends step 1 (the running firmware) */
#define FM1_FINISH_WRITE 0xF0000000u     /* ... and step 2 (the update loader) */
#define FM1_SYSEX_MAX 640                /* a response to a 512-byte read is 605 bytes */
#define FM1_ID_MAX 28

extern const uint8_t FM1_HS_QUERY[10];   /* "who are you": the device answers with its identity */
extern const uint8_t FM1_UPGRADE[6];     /* start the update: the device starts asking */

/* 8 bits to 7, LSB first; the length out */
size_t fm1_pack7(const uint8_t *in, size_t n, uint8_t *out);
size_t fm1_unpack7(const uint8_t *in, size_t n, uint8_t *out);

/* the answer to a read request: F0 pack7(...) F7 into out (FM1_SYSEX_MAX), its length */
size_t fm1_response(uint32_t addr, const uint8_t *data, size_t n, uint8_t flags, uint8_t *out);

typedef struct {
    uint8_t flags;
    uint32_t addr;
    uint32_t len;
} fm1_request_t;
bool fm1_parse_request(const uint8_t *pkt, size_t n, fm1_request_t *r);

/* "FM-1_975" -> model "FM-1", version 975; the update loader is "ota-FM-1_9XX" */
typedef struct {
    char text[FM1_ID_MAX];
    char model[FM1_ID_MAX];
    int version;
} fm1_identity_t;
bool fm1_parse_identity(const uint8_t *pkt, size_t n, fm1_identity_t *id);
bool fm1_id_is_loader(const fm1_identity_t *id);
/* the model without "ota-" */
const char *fm1_bare_model(const char *model);
/* the same hardware: equal models, or two names of the SMK-37 family (never an FM-1 and an SMK-37) */
bool fm1_same_keyboard(const char *model_a, const char *model_b);
/* Felucca's own update loader (the only one an interrupted install is resumed with) */
bool fm1_own_loader(const char *id_text);

/* ---------------------------------------------------------------------------------- package --- */

/* The package's identity ("FM-1_906"), one marker byte after each of the first 20 blocks; false
 * when it is not one ("[^_]+_\d+") */
bool fm1_product_of(const uint8_t *raw, size_t n, char out[FM1_ID_MAX]);
/* the image the device reads: the package without the 20 marker bytes (n - 20 bytes; in place
 * allowed, out == raw); 0 when too short */
size_t fm1_logical_image(const uint8_t *raw, size_t n, uint8_t *out);
/* NULL, or what is damaged in the image (header, file list, a file's CRC) */
const char *fm1_package_damage(const uint8_t *img, size_t n, char *why, size_t whylen);

typedef enum { FM1_BOARD_UNKNOWN, FM1_BOARD_FM1, FM1_BOARD_SMK } fm1_board_t;
/* the board the package is laid out for, from flash.bin's region table */
fm1_board_t fm1_package_board(const uint8_t *img, size_t n);
/* NULL, or why the image must not be written to a device reporting model */
const char *fm1_board_refusal(const char *model, const uint8_t *img, size_t n);
/* Felucca's update loader is in the package (raw or logical) */
bool fm1_has_felucca_loader(const uint8_t *buf, size_t n);

uint16_t fm1_crc16(const uint8_t *p, size_t n, uint16_t crc);
