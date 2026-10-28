/* SPDX-License-Identifier: GPL-3.0-only */
/* hbdeploy.py's frame, parse, hello, modules, deploy and dock.py's delete / select, for one device. */
#include "fm1_dock.h"
#include <stdio.h>
#include <string.h>

static const uint8_t MAN[3] = { 0x7D, 0x48, 0x42 };   /* 7D: non-commercial; "HB" */

static const char *const STATUS[] = { "done", "bad CRC", "bad header", "too big", "no space", "flash error", "verify failed",
                                      "data missing", "not started", "not found", "bad message", "refused" };
static const char *const STATE[] = { "ok", "bad CRC", "bad header", "too big", "crashed", "init failed", "too slow" };

const char *fm1_dock_status(int s) { return s >= 0 && s < (int)(sizeof STATUS / sizeof *STATUS) ? STATUS[s] : "unknown status"; }
const char *fm1_dock_state(int s) { return s >= 0 && s < (int)(sizeof STATE / sizeof *STATE) ? STATE[s] : "?"; }

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

uint32_t fm1_crc32_upd(uint32_t crc, const uint8_t *p, size_t n)   /* zlib's crc32(crc, p, n) */
{
    uint32_t c = ~crc;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

uint32_t fm1_crc32(const uint8_t *p, size_t n) { return fm1_crc32_upd(0, p, n); }

size_t fm1_dock_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t n, uint8_t *out)
{
    uint8_t body[FM1_DOCK_PAYLOAD_MAX + 4];
    if (n > FM1_DOCK_PAYLOAD_MAX) return 0;
    memcpy(body, payload, n);
    wr32(body + n, fm1_crc32(payload, n));
    out[0] = 0xF0;
    memcpy(out + 1, MAN, 3);
    out[4] = cmd;
    out[5] = seq & 0x7F;
    size_t o = 6 + fm1_pack7(body, n + 4, out + 6);
    out[o++] = 0xF7;
    return o;
}

bool fm1_dock_parse(const uint8_t *msg, size_t n, uint8_t *cmd, uint8_t *seq, uint8_t *payload, size_t *plen)
{
    uint8_t d[FM1_SYSEX_MAX];
    if (n < 7 || msg[0] != 0xF0 || msg[n - 1] != 0xF7 || memcmp(msg + 1, MAN, 3) != 0 || n - 7 > FM1_SYSEX_MAX) return false;
    size_t k = fm1_unpack7(msg + 6, n - 7, d);
    if (k < 4 || k - 4 > FM1_DOCK_PAYLOAD_MAX || fm1_crc32(d, k - 4) != rd32(d + k - 4)) return false;
    *cmd = msg[4] & ~0x40;
    *seq = msg[5];
    memcpy(payload, d, k - 4);
    *plen = k - 4;
    return true;
}

/* hb_file_t (fm1-dock's hb_module.h, 80 bytes): magic, abi, flags, name[24], version, code_size,
 * data_size, bss_size, rt_size, worst_cycles, desc, base, n_relocs, reserved, crc; then the
 * relocations (n_relocs words) and, from the next 256-byte boundary, the image (code, .data's
 * initial values, .ram_text). crc covers the header up to crc, the relocations and the image. */
bool fm1_dock_module_info(const uint8_t *blob, size_t n, fm1_dock_info_t *info)
{
    if (n < 80 || rd32(blob) != FM1_DOCK_MAGIC) return false;
    memset(info, 0, sizeof *info);
    info->abi = rd32(blob + 4);
    info->flags = rd32(blob + 8);
    int l = 0;
    while (l < 23 && blob[12 + l]) {
        uint8_t c = blob[12 + l];
        info->name[l++] = c >= 32 && c < 127 ? c : '?';
    }
    info->name[l] = 0;
    info->version = rd32(blob + 36);
    info->code_size = rd32(blob + 40);
    info->data_size = rd32(blob + 44);
    info->bss_size = rd32(blob + 48);
    info->rt_size = rd32(blob + 52);
    uint32_t relocs = rd32(blob + 68);
    if (relocs > 65536) return false;
    info->image = (80 + 4 * relocs + 255) & ~255u;
    return true;
}

/* the image is inside the file (a whole one) */
static bool probe_layout(const uint8_t *blob, size_t n, fm1_dock_info_t *info)
{
    if (!fm1_dock_module_info(blob, n, info) || !(info->flags & FM1_DOCK_F_PROBE)) return false;
    uint64_t end = (uint64_t)info->image + info->code_size + info->data_size + info->rt_size;
    return end <= n;
}

/* ------------------------------------------------------------------------------- requests --- */

static uint8_t seq_next;

static uint8_t send(const fm1_io_t *io, uint8_t cmd, const uint8_t *payload, size_t n)
{
    static uint8_t f[FM1_SYSEX_MAX];
    seq_next = (seq_next + 1) & 0x7F;
    io->send(f, fm1_dock_frame(cmd, seq_next, payload, n, f));
    return seq_next;
}

/* the next reply to seq within ms (the wait restarts with each one): its cmd, -1 none */
static int reply(const fm1_io_t *io, uint8_t seq, uint32_t ms, uint8_t *payload, size_t *plen)
{
    static uint8_t m[FM1_SYSEX_MAX];
    uint32_t end = io->now_ms() + ms;
    for (int32_t left; (left = (int32_t)(end - io->now_ms())) > 0;) {
        int k = io->read(m, sizeof m, left);
        if (k < 0) return -1;
        if (k == 0) break;
        uint8_t c, s;
        if (fm1_dock_parse(m, k, &c, &s, payload, plen) && s == seq) return c;
    }
    return -1;
}

/* one request and its reply's payload: its length, -1 when nothing answers */
static int ask(const fm1_io_t *io, uint8_t cmd, const uint8_t *payload, size_t n, uint32_t ms, int tries, uint8_t *out)
{
    size_t plen;
    for (int t = 0; t < tries; t++) {
        uint8_t s = send(io, cmd, payload, n);
        int c;
        while ((c = reply(io, s, ms, out, &plen)) >= 0)
            if (c == cmd || c == 0x3F) return plen;
    }
    return -1;
}

static void name24(uint8_t out[24], const char *name)
{
    memset(out, 0, 24);
    for (int i = 0; i < 23 && name[i]; i++) out[i] = name[i];
}

bool fm1_dock_hello(const fm1_io_t *io, fm1_dock_hello_t *h, int tries)
{
    uint8_t d[FM1_DOCK_PAYLOAD_MAX];
    io->drain();
    int n = ask(io, FM1_DOCK_HELLO, NULL, 0, 600, tries, d);
    if (n < 20 || d[0] != 0) return false;
    *h = (fm1_dock_hello_t){ d[1], d[2], d[3], d[4], rd32(d + 8), rd32(d + 12), rd32(d + 16) };
    return true;
}

int fm1_dock_list(const fm1_io_t *io, fm1_dock_module_t *mods, int max)
{
    uint8_t d[FM1_DOCK_PAYLOAD_MAX];
    size_t plen;
    for (int t = 0; t < 2; t++) {
        int k = 0, c;
        uint8_t s = send(io, FM1_DOCK_LIST, NULL, 0);
        while ((c = reply(io, s, 1000, d, &plen)) >= 0) {
            if (c != FM1_DOCK_LIST || plen < 2) continue;
            if (d[1] == 0xFF) return k;   /* the end */
            if (plen < 40 || k >= max) continue;
            fm1_dock_module_t *m = &mods[k++];
            m->index = d[1];
            m->state = d[2];
            m->kind = d[3];
            m->version = rd32(d + 4);
            m->size = rd32(d + 8);
            int l = 0;
            while (l < 23 && d[16 + l]) {
                m->name[l] = d[16 + l] >= 32 && d[16 + l] < 127 ? d[16 + l] : '?';
                l++;
            }
            m->name[l] = 0;
        }
    }
    return -1;
}

static int by_name(const fm1_io_t *io, uint8_t cmd, const char *name)
{
    uint8_t p[24], d[FM1_DOCK_PAYLOAD_MAX];
    name24(p, name);
    int n = ask(io, cmd, p, 24, 10000, 1, d);
    return n < 1 ? -1 : d[0];
}

int fm1_dock_select(const fm1_io_t *io, const char *name) { return by_name(io, FM1_DOCK_SELECT, name); }
int fm1_dock_delete(const fm1_io_t *io, const char *name) { return by_name(io, FM1_DOCK_DELETE, name); }

bool fm1_dock_deploy(const fm1_io_t *io, const uint8_t *blob, size_t n, void (*progress)(int, void *), void *arg,
                     char *msg, size_t msglen)
{
    fm1_dock_info_t info;
    fm1_dock_hello_t h;
    uint8_t p[4 + FM1_DOCK_CHUNK], d[FM1_DOCK_PAYLOAD_MAX];
    if (!fm1_dock_module_info(blob, n, &info)) {
        snprintf(msg, msglen, "not an fm1-dock module (.dock)");
        return false;
    }
    if (info.flags & FM1_DOCK_F_PROBE) {
        snprintf(msg, msglen, "%s is a probe (fm1-dock's tools/probe.py runs it), not a module", info.name);
        return false;
    }
    if (info.flags & FM1_DOCK_F_BOOT) {
        snprintf(msg, msglen, "%s is a boot firmware (it takes over the FM-1 at power-on): not deployed here, only "
                 "with fm1-dock's dock.py --boot", info.name);
        return false;
    }
    if (!fm1_dock_hello(io, &h, 3)) {
        snprintf(msg, msglen, "fm1-dock does not answer: is it running on the FM-1?");
        return false;
    }
    if (h.abi != info.abi) {
        snprintf(msg, msglen, "the FM-1's fm1-dock speaks ABI %u, %s was built for %lu: update one of them", h.abi, info.name,
                 (unsigned long)info.abi);
        return false;
    }
    if (n > h.max_file) {
        snprintf(msg, msglen, "%s: %u bytes, fm1-dock takes up to %lu", info.name, (unsigned)n, (unsigned long)h.max_file);
        return false;
    }
    /* what is there before: whether it replaces one of its name (whose space it may take), and whether
     * this very version was there already (then a lost COMMIT answer cannot be told from the list) */
    static fm1_dock_module_t mods[FM1_DOCK_MODS_MAX];
    int nm = fm1_dock_list(io, mods, FM1_DOCK_MODS_MAX);
    bool replaces = false, had = false;
    for (int i = 0; i < nm; i++) {
        replaces |= strcmp(mods[i].name, info.name) == 0;
        had |= strcmp(mods[i].name, info.name) == 0 && mods[i].version == info.version;
    }
    /* no room for it, and it does not replace one: said before anything is sent, not at COMMIT */
    if (n > h.largest) {
        if (!replaces) {
            snprintf(msg, msglen, "%s needs %u KB; the FM-1 has room for %lu KB (%lu KB free in all): delete a module first",
                     info.name, (unsigned)((n + 1023) / 1024), (unsigned long)(h.largest / 1024), (unsigned long)(h.free / 1024));
            return false;
        }
    }
    /* BEGIN [size][CRC-32][name24], DATA [offset][256 bytes] each, COMMIT */
    wr32(p, n);
    wr32(p + 4, fm1_crc32(blob, n));
    name24(p + 8, info.name);
    int k = ask(io, FM1_DOCK_BEGIN, p, 32, 2000, 4, d);
    if (k < 1 || d[0] != 0) {
        snprintf(msg, msglen, "%s: %s (at BEGIN)", info.name, k < 1 ? "no answer" : fm1_dock_status(d[0]));
        return false;
    }
    for (size_t off = 0; off < n; off += FM1_DOCK_CHUNK) {
        size_t c = n - off < FM1_DOCK_CHUNK ? n - off : FM1_DOCK_CHUNK;
        wr32(p, off);
        memcpy(p + 4, blob + off, c);
        k = ask(io, FM1_DOCK_DATA, p, 4 + c, 2000, 4, d);
        if (k < 1 || d[0] != 0) {
            send(io, FM1_DOCK_ABORT, NULL, 0);
            snprintf(msg, msglen, "%s: %s (at DATA %u)", info.name, k < 1 ? "no answer" : fm1_dock_status(d[0]), (unsigned)off);
            return false;
        }
        if (progress) progress((int)((off + c) * 99 / n), arg);
    }
    k = ask(io, FM1_DOCK_COMMIT, NULL, 0, 30000, 1, d);
    if (k < 1 && !had) {
        /* the answer was lost (a USB glitch as the flash is written): the module list tells */
        io->sleep_ms(1000);
        int m = fm1_dock_list(io, mods, FM1_DOCK_MODS_MAX);
        for (int i = 0; i < m; i++)
            if (strcmp(mods[i].name, info.name) == 0 && mods[i].version == info.version) {
                if (progress) progress(100, arg);
                snprintf(msg, msglen, "%s v%lu is in the FM-1's flash (its answer was lost; the FM-1's list has it)", info.name,
                         (unsigned long)info.version);
                return true;
            }
    }
    if (k < 1 || d[0] != 0) {
        snprintf(msg, msglen, "%s: %s (at COMMIT)%s", info.name, k < 1 ? "no answer" : fm1_dock_status(d[0]),
                 k >= 1 && d[0] == 4 ? ": delete a module on the FM-1 first" : "");
        return false;
    }
    if (progress) progress(100, arg);
    snprintf(msg, msglen, "%s v%lu is in the FM-1's flash and playing", info.name, (unsigned long)info.version);
    return true;
}

/* --------------------------------------------------------------------------------- probes --- */

static const char *const PSTATE[] = { "none", "running", "done", "crashed", "hung" };
const char *fm1_dock_probe_state(int s) { return s >= 0 && s < (int)(sizeof PSTATE / sizeof *PSTATE) ? PSTATE[s] : "?"; }

long fm1_dock_probe_find(const uint8_t *blob, size_t n, uint32_t magic)
{
    fm1_dock_info_t info;
    if (!probe_layout(blob, n, &info)) return -1;
    const uint8_t *data = blob + info.image + info.code_size;
    for (size_t o = 0; o + 4 <= info.data_size; o += 4)
        if (rd32(data + o) == magic) return (long)o;
    return -1;
}

bool fm1_dock_probe_patch(uint8_t *blob, size_t n, size_t data_off, const uint8_t *p, size_t len)
{
    fm1_dock_info_t info;
    if (!probe_layout(blob, n, &info) || data_off > info.data_size || len > info.data_size - data_off) return false;
    memcpy(blob + info.image + info.code_size + data_off, p, len);
    uint32_t relocs = rd32(blob + 68);
    uint32_t c = fm1_crc32_upd(0, blob, 76);
    c = fm1_crc32_upd(c, blob + 80, 4 * relocs);
    c = fm1_crc32_upd(c, blob + info.image, info.code_size + info.data_size + info.rt_size);
    wr32(blob + 76, c);
    return true;
}

static uint8_t run_seq;
static bool run_on;
static uint32_t run_id, run_check;   /* the run's id; when to ask HELLO next */
#define RUN_CHECK_MS 2000

bool fm1_dock_run(const fm1_io_t *io, const uint8_t *blob, size_t n, uint32_t id, const uint8_t *arg, size_t argn,
                  void (*progress)(int, void *), void *parg, char *msg, size_t msglen)
{
    fm1_dock_info_t info;
    fm1_dock_hello_t h;
    uint8_t p[4 + FM1_DOCK_CHUNK], d[FM1_DOCK_PAYLOAD_MAX];
    run_on = false;
    if (!fm1_dock_module_info(blob, n, &info)) {
        snprintf(msg, msglen, "not an fm1-dock module");
        return false;
    }
    if (!(info.flags & FM1_DOCK_F_PROBE) || !probe_layout(blob, n, &info)) {
        snprintf(msg, msglen, "%s is not a probe (a module is deployed, not run)", info.name);
        return false;
    }
    if (n > FM1_DOCK_PROBE_FILE || argn > FM1_DOCK_PROBE_ARG) {
        snprintf(msg, msglen, "%s: %u bytes, a probe is at most %u", info.name, (unsigned)n, (unsigned)FM1_DOCK_PROBE_FILE);
        return false;
    }
    if (!fm1_dock_hello(io, &h, 3)) {
        snprintf(msg, msglen, "fm1-dock does not answer: is it running on the FM-1?");
        return false;
    }
    if (h.abi != info.abi) {
        snprintf(msg, msglen, "the FM-1's fm1-dock speaks ABI %u, %s was built for %lu: update one of them", h.abi, info.name,
                 (unsigned long)info.abi);
        return false;
    }
    wr32(p, n);
    wr32(p + 4, fm1_crc32(blob, n));
    name24(p + 8, info.name);
    int k = ask(io, FM1_DOCK_BEGIN, p, 32, 2000, 4, d);
    if (k < 1 || d[0] != 0) {
        snprintf(msg, msglen, "%s: %s (at BEGIN)", info.name, k < 1 ? "no answer" : fm1_dock_status(d[0]));
        return false;
    }
    for (size_t off = 0; off < n; off += FM1_DOCK_CHUNK) {
        size_t c = n - off < FM1_DOCK_CHUNK ? n - off : FM1_DOCK_CHUNK;
        wr32(p, off);
        memcpy(p + 4, blob + off, c);
        k = ask(io, FM1_DOCK_DATA, p, 4 + c, 2000, 4, d);
        if (k < 1 || d[0] != 0) {
            send(io, FM1_DOCK_ABORT, NULL, 0);
            snprintf(msg, msglen, "%s: %s (at DATA %u)", info.name, k < 1 ? "no answer" : fm1_dock_status(d[0]), (unsigned)off);
            return false;
        }
        if (progress) progress((int)((off + c) * 99 / n), parg);
    }
    /* RUN [id][argument]: answered when run() returns; sent once (a second RUN after the first was
     * taken would only be answered "not started" when the probe is done) */
    uint8_t a[4 + FM1_DOCK_PROBE_ARG];
    wr32(a, id);
    memcpy(a + 4, arg, argn);
    run_seq = send(io, FM1_DOCK_RUN, a, 4 + argn);
    run_on = true;
    run_id = id;
    run_check = io->now_ms() + RUN_CHECK_MS;
    if (progress) progress(100, parg);
    snprintf(msg, msglen, "%s runs on the FM-1", info.name);
    return true;
}

int fm1_dock_run_wait(const fm1_io_t *io, uint32_t ms)
{
    static uint8_t m[FM1_SYSEX_MAX], d[FM1_DOCK_PAYLOAD_MAX];
    size_t plen;
    if (!run_on) return -1;
    uint32_t end = io->now_ms() + ms;
    do {
        int32_t left = (int32_t)(end - io->now_ms());
        int k = io->read(m, sizeof m, left > 0 ? left : 0);
        if (k < 0) {
            run_on = false;
            return -1;
        }
        if (k > 0) {
            uint8_t c, s;
            if (!fm1_dock_parse(m, k, &c, &s, d, &plen) || s != run_seq) continue;
            run_on = false;
            if (c != FM1_DOCK_RUN || plen < 1) return 10;
            return d[0];
        }
        if ((int32_t)(io->now_ms() - run_check) >= 0) {   /* is fm1-dock's loop free? */
            run_check = io->now_ms() + RUN_CHECK_MS;
            fm1_dock_hello_t h;
            if (fm1_dock_hello(io, &h, 1)) {
                fm1_dock_result_t r;
                uint8_t none[1];
                run_on = false;
                if (fm1_dock_result(io, &r, none, 0) >= 0 && r.id == run_id) {
                    if (r.state == 2) return 0;
                    if (r.state == 3 || r.state == 4) return FM1_DOCK_CRASHED;
                }
                send(io, FM1_DOCK_ABORT, NULL, 0);
                return FM1_DOCK_LOST;
            }
        }
    } while ((int32_t)(end - io->now_ms()) > 0);
    return FM1_DOCK_WAITING;
}

void fm1_dock_run_abort(const fm1_io_t *io)
{
    run_on = false;
    send(io, FM1_DOCK_ABORT, NULL, 0);
}

int fm1_dock_result(const fm1_io_t *io, fm1_dock_result_t *r, uint8_t *out, size_t cap)
{
    uint8_t p[4], d[FM1_DOCK_PAYLOAD_MAX];
    uint32_t from = 0;
    memset(r, 0, sizeof *r);
    for (;;) {
        wr32(p, from);
        int k = ask(io, FM1_DOCK_RESULT, p, 4, 1000, 3, d);
        if (k < 28 || d[0] != 0) return -1;
        r->state = d[1];
        r->id = rd32(d + 4);
        r->rc = (int32_t)rd32(d + 8);
        r->ticks = rd32(d + 12);
        r->out_n = rd32(d + 16);
        r->pc = rd32(d + 20);
        uint32_t at = rd32(d + 24);
        size_t chunk = k - 28;
        if (at != from) return -1;
        if (chunk && from < cap) {
            size_t c = cap - from < chunk ? cap - from : chunk;
            memcpy(out + from, d + 28, c);
        }
        from += chunk;
        if (!chunk || from >= r->out_n || from >= cap) return (int)r->out_n;
    }
}
