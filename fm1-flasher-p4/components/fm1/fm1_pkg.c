/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 package and updater SysEx, as fm1-dock's tools/fm1_install.py has them (the functions
 * keep its names: product_of, logical_image, package_damage, package_board, board_refusal). */
#include "fm1_pkg.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

const uint8_t FM1_HS_QUERY[10] = { 0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7 };
const uint8_t FM1_UPGRADE[6] = { 0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7 };

#define BLOCKS 20
#define BLK 0x30
#define KEEP 0x2F

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

size_t fm1_pack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    uint32_t acc = 0;
    int nb = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= (uint32_t)in[i] << nb;
        nb += 8;
        while (nb >= 7) {
            out[o++] = acc & 0x7F;
            acc >>= 7;
            nb -= 7;
        }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}

size_t fm1_unpack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    uint32_t acc = 0;
    int nb = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= (uint32_t)(in[i] & 0x7F) << nb;
        nb += 7;
        while (nb >= 8) {
            out[o++] = acc & 0xFF;
            acc >>= 8;
            nb -= 8;
        }
    }
    return o;
}

size_t fm1_response(uint32_t addr, const uint8_t *data, size_t n, uint8_t flags, uint8_t *out)
{
    uint8_t body[7 + 4 + 3 + FM1_MAXDATA + 1];
    if (n > FM1_MAXDATA) return 0;
    size_t b = 0;
    body[b++] = 0x00; body[b++] = 0x59; body[b++] = 0x30;
    body[b++] = (n + 8) & 0xFF; body[b++] = ((n + 8) >> 8) & 0xFF;
    body[b++] = 0; body[b++] = flags;
    for (int i = 0; i < 4; i++) body[b++] = (addr >> (8 * i)) & 0xFF;
    body[b++] = n & 0xFF; body[b++] = (n >> 8) & 0xFF; body[b++] = 0;
    memcpy(body + b, data, n);
    b += n;
    uint8_t sum = 0;
    for (size_t i = 6; i < b; i++) sum += body[i];
    body[b++] = ~sum;
    out[0] = 0xF0;
    size_t o = 1 + fm1_pack7(body, b, out + 1);
    out[o++] = 0xF7;
    return o;
}

bool fm1_parse_request(const uint8_t *pkt, size_t n, fm1_request_t *r)
{
    uint8_t u[32];
    if (n < 2 || pkt[0] != 0xF0 || pkt[n - 1] != 0xF7 || n - 2 > 34) return false;
    if (fm1_unpack7(pkt + 1, n - 2, u) != 15 || u[0] != 0x00 || u[1] != 0x59 || u[2] != 0x30) return false;
    uint8_t sum = 0;
    for (int i = 6; i < 14; i++) sum += u[i];
    if ((uint8_t)~sum != u[14]) return false;
    r->flags = u[6];
    r->addr = rd32(u + 7);
    r->len = u[11] | (u[12] << 8) | (u[13] << 16);
    return true;
}

/* "[^_]+_\d+": the model and the number */
static bool split_identity(const char *txt, char *model, int *version)
{
    const char *us = strchr(txt, '_');
    if (!us || us == txt || strchr(us + 1, '_') || !us[1]) return false;
    for (const char *p = us + 1; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
    if (model) {
        memcpy(model, txt, us - txt);
        model[us - txt] = 0;
    }
    if (version) *version = 0;
    for (const char *p = us + 1; *p && version; p++) *version = *version * 10 + (*p - '0');
    return true;
}

bool fm1_parse_identity(const uint8_t *pkt, size_t n, fm1_identity_t *id)
{
    uint8_t d[48];
    if (n < 2 || pkt[0] != 0xF0 || pkt[n - 1] != 0xF7 || n - 2 > 48) return false;
    if (fm1_unpack7(pkt + 1, n - 2, d) != 34 || d[0] != 0x00 || d[1] != 0x59 || d[2] != 0x11) return false;
    int len = 27;
    while (len > 0 && d[6 + len - 1] == 0) len--;
    for (int i = 0; i < len; i++) {   /* printable ASCII only: it goes on the screen */
        uint8_t c = d[6 + i];
        id->text[i] = (c >= 32 && c < 127) ? c : '?';
    }
    id->text[len] = 0;
    return split_identity(id->text, id->model, &id->version);
}

bool fm1_id_is_loader(const fm1_identity_t *id)
{
    return strncasecmp(id->model, "ota-", 4) == 0;
}

const char *fm1_bare_model(const char *model)
{
    return strncasecmp(model, "ota-", 4) == 0 ? model + 4 : model;
}

/* SMK-37[ +].*|MKE-P37|STARRYKEY-37.*, whole, ignoring case (fm1_install.py SMK_FAMILY) */
static bool smk_family(const char *m)
{
    if (strncasecmp(m, "SMK-37", 6) == 0) return m[6] == ' ' || m[6] == '+';
    return strcasecmp(m, "MKE-P37") == 0 || strncasecmp(m, "STARRYKEY-37", 12) == 0;
}

bool fm1_same_keyboard(const char *a, const char *b)
{
    a = fm1_bare_model(a);
    b = fm1_bare_model(b);
    return strcmp(a, b) == 0 || (smk_family(a) && smk_family(b));
}

/* ota-(FM-1|SMK-37 Pro|SMK-37 Elite|MKE-P37)_9\d\d, whole, ignoring case (OWN_LOADER_RE) */
bool fm1_own_loader(const char *t)
{
    static const char *const models[] = { "FM-1", "SMK-37 Pro", "SMK-37 Elite", "MKE-P37" };
    if (strncasecmp(t, "ota-", 4) != 0) return false;
    t += 4;
    for (size_t i = 0; i < sizeof models / sizeof models[0]; i++) {
        size_t l = strlen(models[i]);
        if (strncasecmp(t, models[i], l) == 0 && t[l] == '_' && t[l + 1] == '9' && isdigit((unsigned char)t[l + 2]) &&
            isdigit((unsigned char)t[l + 3]) && !t[l + 4])
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------------- package --- */

bool fm1_product_of(const uint8_t *raw, size_t n, char out[FM1_ID_MAX])
{
    int o = 0;
    out[0] = 0;
    if (n < BLOCKS * BLK) return false;
    for (int i = 0; i < BLOCKS; i++) {
        uint8_t m = raw[i * BLK + KEEP];
        if (m != 0x7D) out[o++] = (char)((m - i - 1) & 0xFF);
    }
    out[o] = 0;
    for (int i = 0; i < o; i++)
        if ((uint8_t)out[i] < 32 || (uint8_t)out[i] >= 127) return false;
    return split_identity(out, NULL, NULL);
}

size_t fm1_logical_image(const uint8_t *raw, size_t n, uint8_t *out)
{
    if (n < BLOCKS * BLK) return 0;
    for (int i = 0; i < BLOCKS; i++) memmove(out + i * KEEP, raw + i * BLK, KEEP);
    memmove(out + BLOCKS * KEEP, raw + BLOCKS * BLK, n - BLOCKS * BLK);
    return n - BLOCKS;
}

uint16_t fm1_crc16(const uint8_t *p, size_t n, uint16_t crc)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i] << 8;
        for (int b = 0; b < 8; b++) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

/* JieLi's ENC stream cipher (its own inverse), from key */
static void enc(uint8_t *p, size_t n, uint16_t key)
{
    for (size_t i = 0; i < n; i++) {
        p[i] ^= key & 0xFF;
        key = (key << 1) ^ (key & 0x8000 ? 0x1021 : 0);
    }
}

const char *fm1_package_damage(const uint8_t *img, size_t n, char *why, size_t whylen)
{
    uint8_t hdr[0x40], e[0x50];
    if (n < 0x40) return "too short";
    memcpy(hdr, img, 0x40);
    enc(hdr, 0x40, 0xFFFF);
    if (fm1_crc16(hdr + 2, 0x3E, 0) != rd16(hdr)) return "header CRC";
    uint32_t count = rd16(hdr + 8);
    if (count < 1 || count > 8 || n < 0x40 + 0x50 * count) return "file list";
    if (fm1_crc16(img + 0x40, 0x50 * count, 0) != rd16(hdr + 2)) return "file list CRC";
    for (uint32_t i = 0; i < count; i++) {
        memcpy(e, img + 0x40 + i * 0x50, 0x50);
        enc(e, 0x50, 0xFFFF);
        uint32_t off = rd32(e + 8), size = rd32(e + 12);
        char name[17];
        memcpy(name, e + 0x40, 16);
        name[16] = 0;
        for (char *c = name; *c; c++)
            if ((uint8_t)*c < 32 || (uint8_t)*c >= 127) *c = '?';
        if ((uint64_t)off + size > n) {
            snprintf(why, whylen, "%s: cut short", name);
            return why;
        }
        if (fm1_crc16(img + off, size, 0) != rd16(e + 4)) {
            snprintf(why, whylen, "%s: CRC", name);
            return why;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ the package's board --- */
/* flash.bin's region table (app offset 0x60 = flash 0x4060-0x411F, SFC-enciphered with the chip key
 * 0x980F): BTIF and USR at 0xC0000 / 0xF4000 on the SMK-37, at 0xE9000 / 0xEA000 on the FM-1 */

#define SMK_BTIF 0xC0000u
#define SMK_USR 0xF4000u
#define SMK_USR_SIZE 0xA000u
#define FM1_BTIF 0xE9000u
#define FM1_USR 0xEA000u

typedef struct {
    uint32_t type, off, size, usr, usr_size;
} pkg_file_t;

static int package_files(const uint8_t *img, size_t n, pkg_file_t *out, int max)
{
    uint8_t hdr[0x40], e[0x50];
    if (n < 0x40) return 0;
    memcpy(hdr, img, 0x40);
    enc(hdr, 0x40, 0xFFFF);
    int count = rd16(hdr + 8), k = 0;
    if (count > 11) count = 11;
    for (int i = 0; i < count && k < max; i++) {
        if (0x90 + (size_t)i * 0x50 > n) break;
        memcpy(e, img + 0x40 + i * 0x50, 0x50);
        enc(e, 0x50, 0xFFFF);
        out[k++] = (pkg_file_t){ rd16(e), rd32(e + 8), rd32(e + 12), rd32(e + 0x1C), rd32(e + 0x20) };
    }
    return k;
}

typedef struct {
    bool found;
    uint32_t addr, size;
} region_t;

static void region_table(const uint8_t *flash, size_t n, region_t *btif, region_t *usr)
{
    uint8_t t[0xC0];
    btif->found = usr->found = false;
    if (n < 0x4120) return;
    memcpy(t, flash + 0x4060, 0xC0);
    for (int i = 0; i < 0xC0; i += 32) enc(t + i, 32, (0x980F ^ ((0x60 + i) >> 2)) & 0xFFFF);
    for (int r = 0; r < 0xC0; r += 32) {
        char name[17];
        int l = 0;
        while (l < 16 && t[r + 16 + l] > 0x20 && t[r + 16 + l] < 0x7F) {
            name[l] = t[r + 16 + l];
            l++;
        }
        name[l] = 0;
        region_t *dst = strcmp(name, "BTIF") == 0 ? btif : strcmp(name, "USR") == 0 ? usr : NULL;
        if (dst) *dst = (region_t){ true, rd32(t + r + 4), rd32(t + r + 8) };   /* the last one, as a dict */
    }
}

fm1_board_t fm1_package_board(const uint8_t *img, size_t n)
{
    pkg_file_t files[11];
    int nf = package_files(img, n, files, 11);
    const pkg_file_t *fl = NULL;
    for (int i = 0; i < nf && !fl; i++)
        if (files[i].type == 0) fl = &files[i];
    if (!fl || (uint64_t)fl->off + fl->size > n) return FM1_BOARD_UNKNOWN;
    region_t btif, usr;
    region_table(img + fl->off, fl->size, &btif, &usr);
    uint32_t btif_addr = btif.found ? btif.addr : 0, usr_addr = usr.found ? usr.addr : 0;
    bool all_smk = true, any_fm1 = false;
    for (int i = 0; i < nf; i++) {
        if (files[i].type != 50) continue;
        if (files[i].usr != SMK_USR || files[i].usr_size != SMK_USR_SIZE) all_smk = false;
        if (files[i].usr == FM1_USR) any_fm1 = true;
    }
    if (btif_addr == SMK_BTIF && usr.found && usr.addr == SMK_USR && usr.size == SMK_USR_SIZE && all_smk)
        return FM1_BOARD_SMK;
    if (btif_addr == FM1_BTIF || usr_addr == FM1_USR || any_fm1) return FM1_BOARD_FM1;
    return FM1_BOARD_UNKNOWN;
}

/* (ota-)?(SMK-37([ +].*)?|MKE-P37|STARRYKEY-37.*), whole, ignoring case (SMK_MODEL_RE) */
static bool smk_model(const char *m)
{
    m = fm1_bare_model(m);
    if (strncasecmp(m, "SMK-37", 6) == 0) return !m[6] || m[6] == ' ' || m[6] == '+';
    return strcasecmp(m, "MKE-P37") == 0 || strncasecmp(m, "STARRYKEY-37", 12) == 0;
}

const char *fm1_board_refusal(const char *model, const uint8_t *img, size_t n)
{
    fm1_board_t board = fm1_package_board(img, n);
    bool smk = smk_model(model);
    if (smk && board == FM1_BOARD_FM1) return "this package is for the FM-1; the device is an SMK-37";
    if (!smk && board == FM1_BOARD_SMK) return "this package is for the SMK-37; the device is not one";
    return NULL;
}

bool fm1_has_felucca_loader(const uint8_t *buf, size_t n)
{
    static const char mark[] = "FELUCCA-LOADER-1";
    const size_t l = sizeof mark - 1;
    for (size_t i = 0; i + l <= n; i++)
        if (buf[i] == 'F' && memcmp(buf + i, mark, l) == 0) return true;
    return false;
}
