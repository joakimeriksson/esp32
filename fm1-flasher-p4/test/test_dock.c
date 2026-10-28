/* SPDX-License-Identifier: GPL-3.0-only */
/* components/fm1/fm1_dock.c on the computer: the frames of a deploy as dock_ref.py prints them
 * (hbdeploy.py's), and deploys, lists, selects and deletes against a simulated fm1-dock.
 *
 *   test_dock MODULE.dock...      (run.sh diffs the lines against dock_ref.py's) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fm1_dock.h"

static int failures;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) exit(2);
    fclose(f);
    return b;
}

static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* --------------------------------------------------------------------- a simulated fm1-dock --- */

#define SIM_ABI 2
static struct {
    uint32_t now;
    bool running;                 /* fm1-dock (false: another firmware: nothing answers) */
    uint8_t q[16][FM1_SYSEX_MAX];
    size_t qn[16];
    int qh, qt;
    /* the deploy */
    bool rx;
    uint32_t size, crc;
    char name[24];
    uint8_t buf[1 << 20];
    /* the modules */
    int n;
    char names[8][24];
    uint32_t versions[8];
    int cur;
    int drop_every;               /* every nth frame is lost (a reply never comes) */
    uint32_t largest;             /* the largest gap in its flash */
    bool lose_commit_answer;      /* COMMIT is done, its answer lost */
    int frames;
    /* probes: RUN checks the file as harbor's pr_run, "runs" it (rc, out), answers when run_ms have passed */
    bool run_pending;
    uint8_t run_seq;
    uint32_t run_due, run_ms;
    uint32_t run_id;
    uint8_t run_arg[512];
    size_t run_argn;
    int32_t rc;
    uint8_t out[8192];
    uint32_t out_n;
    uint8_t pstate;
    int runs;
} dk;

static void reply(uint8_t cmd, uint8_t seq, const uint8_t *r, size_t n)
{
    dk.qn[dk.qt % 16] = fm1_dock_frame(cmd | 0x40, seq, r, n, dk.q[dk.qt % 16]);
    dk.qt++;
}

static void dock_frame(const uint8_t *m, size_t n)
{
    uint8_t d[FM1_SYSEX_MAX], r[48] = { 0 };
    uint8_t cmd = m[4], seq = m[5];
    size_t k = fm1_unpack7(m + 6, n - 7, d), rn = 1;
    if (dk.drop_every && ++dk.frames % dk.drop_every == 0) return;
    if (dk.run_pending) return;           /* the main loop holds while the probe runs: nothing answers */
    if (k < 4 || fm1_crc32(d, k - 4) != rd32(d + k - 4)) {
        r[0] = 10;
        reply(0x3F, seq, r, 1);
        return;
    }
    k -= 4;
    switch (cmd) {
    case FM1_DOCK_HELLO:
        r[1] = 1; r[2] = SIM_ABI; r[3] = dk.n; r[4] = dk.cur;
        wr32(r + 8, dk.largest); wr32(r + 12, dk.largest + 4096); wr32(r + 16, 393216);
        rn = 44;
        break;
    case FM1_DOCK_LIST:
        for (int i = 0; i < dk.n; i++) {
            memset(r, 0, sizeof r);
            r[1] = i; r[3] = i ? 2 : 0;
            wr32(r + 4, dk.versions[i]);
            memcpy(r + 16, dk.names[i], 24);
            reply(cmd, seq, r, 40);
        }
        memset(r, 0, sizeof r);
        r[1] = 0xFF;
        rn = 2;
        break;
    case FM1_DOCK_BEGIN:
        dk.rx = true;
        dk.size = rd32(d);
        dk.crc = rd32(d + 4);
        memcpy(dk.name, d + 8, 24);
        break;
    case FM1_DOCK_DATA: {
        uint32_t off = rd32(d);
        wr32(r + 4, off);
        rn = 8;
        if (!dk.rx) { r[0] = 8; break; }
        uint32_t want = dk.size - off < 256 ? dk.size - off : 256;
        if ((off & 255) || k - 4 != want) { r[0] = 10; break; }
        memcpy(dk.buf + off, d + 4, want);
        break;
    }
    case FM1_DOCK_COMMIT:
        if (!dk.rx) { r[0] = 8; break; }
        if (rd32(dk.buf + 8) & 0x100) { r[0] = 11; dk.rx = false; break; }   /* a probe: refused */
        dk.rx = false;
        if (fm1_crc32(dk.buf, dk.size) != dk.crc) { r[0] = 1; break; }
        memcpy(dk.names[dk.n], dk.name, 24);
        dk.versions[dk.n] = rd32(dk.buf + 36);
        dk.cur = dk.n++;
        r[1] = dk.cur;
        rn = 8;
        if (dk.lose_commit_answer) return;
        break;
    case FM1_DOCK_ABORT:
        dk.rx = false;
        break;
    case FM1_DOCK_SELECT:
    case FM1_DOCK_DELETE: {
        int f = -1;
        for (int i = 0; i < dk.n; i++)
            if (strncmp(dk.names[i], (const char *)d, 23) == 0) f = i;
        if (f < 0 || (cmd == FM1_DOCK_DELETE && f == 0)) { r[0] = 9; break; }
        if (cmd == FM1_DOCK_SELECT) dk.cur = f;
        else {
            memmove(dk.names[f], dk.names[f + 1], (dk.n - f - 1) * 24);
            dk.n--;
        }
        rn = 2;
        break;
    }
    case FM1_DOCK_RUN: {                  /* harbor's pr_run checks, then the "run" */
        if (!dk.rx) { r[0] = 8; break; }
        dk.rx = false;
        const uint8_t *f = dk.buf;
        if (fm1_crc32(dk.buf, dk.size) != dk.crc) { r[0] = 1; break; }
        uint32_t flags = rd32(f + 8), code = rd32(f + 40), data = rd32(f + 44), rt = rd32(f + 52), relocs = rd32(f + 68);
        if (rd32(f) != FM1_DOCK_MAGIC || rd32(f + 4) != SIM_ABI || !(flags & 0x100) || relocs > 65536) { r[0] = 2; break; }
        uint32_t io = (80 + 4 * relocs + 255) & ~255u, isz = code + data + rt;
        if (io + isz > dk.size) { r[0] = 2; break; }
        if (io + isz > 0x30000) { r[0] = 3; break; }
        uint32_t c = fm1_crc32_upd(0, f, 76);
        c = fm1_crc32_upd(c, f + 80, 4 * relocs);
        c = fm1_crc32_upd(c, f + io, isz);
        if (c != rd32(f + 76)) { r[0] = 1; break; }
        dk.run_id = k >= 4 ? rd32(d) : 0;
        dk.run_argn = k >= 4 ? k - 4 : 0;
        memcpy(dk.run_arg, d + 4, dk.run_argn);
        dk.runs++;
        dk.pstate = 1;
        dk.run_pending = true;
        dk.run_seq = seq;
        dk.run_due = dk.now + dk.run_ms;
        return;                           /* answered when the run is done (sim_read) */
    }
    case FM1_DOCK_RESULT: {
        static uint8_t b[208];
        uint32_t from = k >= 4 ? rd32(d) : 0, m = 0;
        memset(b, 0, 28);
        b[1] = dk.pstate;
        wr32(b + 4, dk.run_id); wr32(b + 8, (uint32_t)dk.rc); wr32(b + 12, 4242); wr32(b + 16, dk.out_n); wr32(b + 20, 0);
        wr32(b + 24, from);
        while (from + m < dk.out_n && m < 180) { b[28 + m] = dk.out[from + m]; m++; }
        reply(cmd, seq, b, 28 + m);
        return;
    }
    default:
        r[0] = 10;
    }
    reply(cmd, seq, r, rn);
}

static void run_done(void)                /* the probe returns: rc and the output as it left them */
{
    uint8_t r[8] = { 0 };
    dk.run_pending = false;
    dk.pstate = 2;
    if (!dk.lose_commit_answer) reply(FM1_DOCK_RUN, dk.run_seq, r, 1);
}

static bool sim_up(void) { return true; }
static bool sim_send(const uint8_t *p, size_t n)
{
    if (dk.running && n > 6 && p[1] == 0x7D && p[2] == 0x48 && p[3] == 0x42) dock_frame(p, n);
    return true;
}
static int sim_read(uint8_t *buf, size_t cap, uint32_t ms)
{
    if (dk.qh == dk.qt && dk.run_pending && (int32_t)(dk.run_due - dk.now) <= (int32_t)ms) {
        dk.now = dk.run_due;
        run_done();
    }
    if (dk.qh == dk.qt) {
        dk.now += ms;
        return 0;
    }
    size_t n = dk.qn[dk.qh % 16];
    if (n > cap) return 0;
    memcpy(buf, dk.q[dk.qh % 16], n);
    dk.qh++;
    dk.now++;
    return n;
}
static void sim_drain(void) { dk.qh = dk.qt; }
static void sim_sleep(uint32_t ms) { dk.now += ms; }
static uint32_t sim_now(void) { return dk.now; }
static const fm1_io_t sim = { sim_up, sim_send, sim_read, sim_drain, sim_sleep, sim_now };

static void sim_reset(void)
{
    memset(&dk, 0, sizeof dk);
    dk.running = true;
    dk.n = 1;
    strcpy(dk.names[0], "SINE");
    dk.largest = 400000;
}

/* ----------------------------------------------------------------------------------- tests --- */

static void frames_line(const char *path, const uint8_t *blob, size_t n)
{
    static uint8_t all[1 << 21];
    uint8_t p[4 + 256];
    size_t o = 0;
    int i = 0;
    fm1_dock_info_t info;
    CHECK(fm1_dock_module_info(blob, n, &info));
    wr32(p, n);
    wr32(p + 4, fm1_crc32(blob, n));
    memset(p + 8, 0, 24);
    memcpy(p + 8, info.name, strlen(info.name));
    o += fm1_dock_frame(FM1_DOCK_BEGIN, i++ & 0x7F, p, 32, all + o);
    for (size_t off = 0; off < n; off += 256) {
        size_t c = n - off < 256 ? n - off : 256;
        wr32(p, off);
        memcpy(p + 4, blob + off, c);
        o += fm1_dock_frame(FM1_DOCK_DATA, i++ & 0x7F, p, 4 + c, all + o);
    }
    if (info.flags & FM1_DOCK_F_PROBE) {
        uint8_t run[4] = { 1, 0, 0, 0 };
        o += fm1_dock_frame(FM1_DOCK_RUN, i++ & 0x7F, run, 4, all + o);
    } else {
        o += fm1_dock_frame(FM1_DOCK_COMMIT, i++ & 0x7F, NULL, 0, all + o);
    }
    const char *base = strrchr(path, '/');
    printf("%s: name=%s version=%u abi=%u flags=0x%x n=%zu frames=%zu:%08x\n", base ? base + 1 : path, info.name,
           info.version, info.abi, info.flags, n, o, fm1_crc32(all, o));
}

static void test_deploys(uint8_t *blob, size_t n)
{
    char msg[160];
    fm1_dock_module_t mods[FM1_DOCK_MODS_MAX];
    fm1_dock_info_t info;
    fm1_dock_module_info(blob, n, &info);

    sim_reset();
    bool ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  deploy: %d %s\n", ok, msg);
    CHECK(ok && memcmp(dk.buf, blob, n) == 0 && dk.n == 2);
    int k = fm1_dock_list(&sim, mods, FM1_DOCK_MODS_MAX);
    CHECK(k == 2 && strcmp(mods[1].name, info.name) == 0 && mods[1].version == info.version);
    CHECK(fm1_dock_select(&sim, "SINE") == 0 && dk.cur == 0);
    CHECK(fm1_dock_select(&sim, "NOPE") == 9);
    CHECK(fm1_dock_delete(&sim, info.name) == 0 && dk.n == 1);

    /* no room: refused before BEGIN; unless it replaces one of its name */
    sim_reset();
    dk.largest = n / 2;
    ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  no room: %d %s\n", ok, msg);
    CHECK(!ok && !dk.rx && dk.size == 0);
    memcpy(dk.names[dk.n++], info.name, 24);
    ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    CHECK(ok && memcmp(dk.buf, blob, n) == 0);

    /* the COMMIT done, its answer lost: the list has it */
    sim_reset();
    dk.lose_commit_answer = true;
    ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  lost COMMIT answer: %d %s\n", ok, msg);
    CHECK(ok && dk.n == 2);

    /* lost frames: each request is sent again */
    sim_reset();
    dk.drop_every = 7;
    ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  lossy deploy: %d %s\n", ok, msg);
    CHECK(ok && memcmp(dk.buf, blob, n) == 0);

    /* another ABI, a boot module, not a module, no fm1-dock: refused before anything is written */
    sim_reset();
    blob[4] ^= 1;
    ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  other ABI: %d %s\n", ok, msg);
    CHECK(!ok && dk.n == 1 && !dk.rx);
    blob[4] ^= 1;
    blob[9] |= 2;   /* HB_F_BOOT */
    ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  boot module: %d %s\n", ok, msg);
    CHECK(!ok && !dk.rx);
    blob[9] &= ~2;
    blob[0] ^= 1;
    CHECK(!fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg));
    blob[0] ^= 1;
    dk.running = false;
    ok = fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  no fm1-dock: %d %s\n", ok, msg);
    CHECK(!ok);
    CHECK(fm1_dock_list(&sim, mods, FM1_DOCK_MODS_MAX) == -1);
}

static void test_probe(uint8_t *blob, size_t n)
{
    char msg[160];
    fm1_dock_info_t info;
    fm1_dock_result_t res;
    uint8_t out[8192];
    CHECK(fm1_dock_module_info(blob, n, &info) && (info.flags & FM1_DOCK_F_PROBE));

    /* the file's CRC as stored is what the patch computes (the layout is understood) */
    uint32_t stored = rd32(blob + 76);
    uint8_t one[1];
    if (info.data_size) {
        one[0] = blob[info.image + info.code_size];
        CHECK(fm1_dock_probe_patch(blob, n, 0, one, 1));   /* the same byte: the CRC must come out the same */
        CHECK(rd32(blob + 76) == stored);
    }
    CHECK(!fm1_dock_probe_patch(blob, n, info.data_size, one, 1));   /* outside .data */
    CHECK(fm1_dock_probe_find(blob, n, 0x12345678u) == -1);

    /* a run: sent, started, the answer when it returns; the result */
    sim_reset();
    dk.run_ms = 5000;
    dk.rc = 7;
    dk.out_n = 300;
    for (uint32_t i = 0; i < dk.out_n; i++) dk.out[i] = (uint8_t)i;
    uint8_t arg[3] = { 1, 2, 3 };
    bool ok = fm1_dock_run(&sim, blob, n, 42, arg, 3, NULL, NULL, msg, sizeof msg);
    fprintf(stderr, "  run: %d %s\n", ok, msg);
    CHECK(ok && dk.runs == 1 && dk.run_id == 42 && dk.run_argn == 3 && dk.run_arg[2] == 3 && !dk.rx);
    CHECK(fm1_dock_run_wait(&sim, 300) == FM1_DOCK_WAITING);
    CHECK(fm1_dock_run_wait(&sim, 300) == FM1_DOCK_WAITING);
    CHECK(fm1_dock_run_wait(&sim, 10000) == 0);
    CHECK(fm1_dock_run_wait(&sim, 10) == -1);   /* no run on */
    int k = fm1_dock_result(&sim, &res, out, sizeof out);
    CHECK(k == 300 && res.state == 2 && res.id == 42 && res.rc == 7 && res.out_n == 300 && out[299] == 299 % 256 && out[0] == 0);
    k = fm1_dock_result(&sim, &res, out, 100);   /* less of it */
    CHECK(k == 300 && out[99] == 99);

    /* a deploy of a probe is refused here; a COMMIT of one by fm1-dock */
    sim_reset();
    CHECK(!fm1_dock_deploy(&sim, blob, n, NULL, NULL, msg, sizeof msg) && !dk.rx);
    fprintf(stderr, "  deploy of a probe: %s\n", msg);

    /* a patched byte without the CRC redone: fm1-dock refuses the RUN */
    sim_reset();
    if (info.data_size) {
        blob[info.image + info.code_size] ^= 0xFF;
        CHECK(fm1_dock_run(&sim, blob, n, 1, NULL, 0, NULL, NULL, msg, sizeof msg));
        CHECK(fm1_dock_run_wait(&sim, 1000) == 1 && dk.runs == 0);
        blob[info.image + info.code_size] ^= 0xFF;
        one[0] = blob[info.image + info.code_size] ^ 0xFF;
        CHECK(fm1_dock_probe_patch(blob, n, 0, one, 1));   /* through the patch: accepted */
        CHECK(fm1_dock_run(&sim, blob, n, 1, NULL, 0, NULL, NULL, msg, sizeof msg));
        CHECK(fm1_dock_run_wait(&sim, 1000) == 0 && dk.runs == 1);
    }

    /* lost frames on the way: BEGIN and DATA are resent; a lost RUN is found out (fm1-dock answers
     * HELLO when it should be running) and run again; a lost RUN answer is found in RESULT */
    sim_reset();
    dk.drop_every = 5;
    dk.run_ms = 100;
    int r = -9, tries = 0;
    while (r != 0 && tries++ < 6) {
        if (!fm1_dock_run(&sim, blob, n, 1, NULL, 0, NULL, NULL, msg, sizeof msg)) continue;
        while ((r = fm1_dock_run_wait(&sim, 500)) == FM1_DOCK_WAITING) {}
        fprintf(stderr, "  lossy run %d: %d (runs %d)\n", tries, r, dk.runs);
        CHECK(r == 0 || r == FM1_DOCK_LOST);
    }
    CHECK(r == 0 && dk.runs >= 1);
    sim_reset();
    dk.run_ms = 100;
    dk.lose_commit_answer = true;         /* (the RUN answer, here) */
    CHECK(fm1_dock_run(&sim, blob, n, 5, NULL, 0, NULL, NULL, msg, sizeof msg));
    while ((r = fm1_dock_run_wait(&sim, 500)) == FM1_DOCK_WAITING) {}
    CHECK(r == 0 && dk.runs == 1);
    sim_reset();
    dk.running = false;
    CHECK(!fm1_dock_run(&sim, blob, n, 1, NULL, 0, NULL, NULL, msg, sizeof msg));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        size_t n;
        uint8_t *b = load(argv[i], &n);
        frames_line(argv[i], b, n);
        fm1_dock_info_t info;
        if (fm1_dock_module_info(b, n, &info) && (info.flags & FM1_DOCK_F_PROBE)) {
            fflush(stdout);
            fprintf(stderr, "probe %s:\n", argv[i]);
            test_probe(b, n);
        } else if (i == argc - 1) {
            fflush(stdout);
            fprintf(stderr, "deploys with %s:\n", argv[i]);
            test_deploys(b, n);
        }
        free(b);
    }
    if (failures) fprintf(stderr, "%d CHECKs failed\n", failures);
    else fprintf(stderr, "all CHECKs passed\n");
    return failures != 0;
}
