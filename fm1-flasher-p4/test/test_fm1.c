/* SPDX-License-Identifier: GPL-3.0-only */
/* components/fm1 on the computer: the package lines pkg_ref.py prints (fm1_install.py's), the SysEx
 * round trips, and whole installs against a simulated FM-1 (step 1, the loader, the write, the
 * restart; and the ways it stops).
 *
 *   test_fm1 PACKAGE.fwsc...      (run.sh builds it and diffs the lines against pkg_ref.py's) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fm1_ota.h"

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

/* ------------------------------------------------------------------------- simulated FM-1 --- */

typedef enum { RUNNING, CHECKING, LOADER, WRITING, GONE } phase_t;

static struct {
    uint32_t now, down_until;
    phase_t phase, after;            /* after: what comes up when down_until is reached */
    char id[FM1_ID_MAX], next_id[FM1_ID_MAX];
    const uint8_t *image;            /* what the package is (the device checks every answer against it) */
    size_t n;
    uint8_t flash[1 << 20];
    uint32_t want_addr, want_len, next;   /* the outstanding request */
    int check_reads;
    uint8_t q[8][FM1_SYSEX_MAX];
    size_t qn[8];
    int qh, qt;
    int unplug_at;                   /* write requests before the cable comes out, -1 never */
    int written_reqs;
    bool silent_after_upgrade;
    int bad_answers;
} dev;

static void push(const uint8_t *p, size_t n)
{
    memcpy(dev.q[dev.qt % 8], p, n);
    dev.qn[dev.qt % 8] = n;
    dev.qt++;
}

static void identity(const char *text)
{
    uint8_t d[34] = { 0x00, 0x59, 0x11 }, out[48];
    memcpy(d + 6, text, strlen(text));
    out[0] = 0xF0;
    size_t o = 1 + fm1_pack7(d, sizeof d, out + 1);
    out[o++] = 0xF7;
    push(out, o);
}

static void request(uint32_t addr, uint32_t len, uint8_t flags)
{
    uint8_t u[15] = { 0x00, 0x59, 0x30, 0, 0, 0, flags }, out[24];
    for (int i = 0; i < 4; i++) u[7 + i] = addr >> (8 * i);
    u[11] = len; u[12] = len >> 8; u[13] = len >> 16;
    uint8_t s = 0;
    for (int i = 6; i < 14; i++) s += u[i];
    u[14] = ~s;
    out[0] = 0xF0;
    size_t o = 1 + fm1_pack7(u, sizeof u, out + 1);
    out[o++] = 0xF7;
    push(out, o);
    dev.want_addr = addr;
    dev.want_len = len;
}

static void sim_reset(const uint8_t *image, size_t n, const char *id, const char *next_id, phase_t phase)
{
    memset(&dev, 0, sizeof dev);
    dev.image = image;
    dev.n = n;
    dev.phase = phase;
    dev.unplug_at = -1;
    snprintf(dev.id, sizeof dev.id, "%s", id);
    snprintf(dev.next_id, sizeof dev.next_id, "%s", next_id);
    memset(dev.flash, 0xFF, sizeof dev.flash);
}

static void go_down(uint32_t ms, phase_t after, const char *id)
{
    dev.phase = GONE;
    dev.after = after;
    dev.down_until = dev.now + ms;
    dev.qh = dev.qt = 0;
    if (id) snprintf(dev.id, sizeof dev.id, "%s", id);
}

static bool sim_up(void)
{
    if (dev.phase == GONE && dev.down_until && dev.now >= dev.down_until) dev.phase = dev.after;
    return dev.phase != GONE;
}

/* the device reads the answer to its outstanding request and asks for the next */
static void answer(const uint8_t *p, size_t n)
{
    uint8_t b[FM1_SYSEX_MAX];
    size_t k = fm1_unpack7(p + 1, n - 2, b);
    uint32_t len = b[11] | (b[12] << 8), addr = b[7] | (b[8] << 8) | (b[9] << 16) | ((uint32_t)b[10] << 24);
    uint8_t s = 0;
    for (size_t i = 6; i < 14 + len; i++) s += b[i];
    if (k < 15 + len || b[2] != 0x30 || (uint8_t)~s != b[14 + len] || addr != dev.want_addr) {
        dev.bad_answers++;
        return;
    }
    if (addr == FM1_FINISH_CHECK) {          /* step 1 done: restart as the loader */
        go_down(1500, LOADER, "ota-FM-1_906");
        return;
    }
    if (addr == FM1_FINISH_WRITE) {          /* written: restart as the new firmware */
        go_down(2500, RUNNING, dev.next_id);
        return;
    }
    if (len != dev.want_len || memcmp(b + 14, dev.image + addr, len) != 0) {
        dev.bad_answers++;
        return;
    }
    if (dev.phase == CHECKING) {
        /* the running firmware reads the head and a few parts, as the loader it stages needs */
        static const uint32_t parts[][2] = { { 0, 0x40 }, { 0x40, 0x50 * 2 }, { 0x1000, 512 }, { 0x2000, 300 } };
        if (++dev.check_reads < 4) request(parts[dev.check_reads][0], parts[dev.check_reads][1], 1);
        else request(FM1_FINISH_CHECK, 8, 0);
        return;
    }
    /* WRITING */
    memcpy(dev.flash + addr, b + 14, len);
    if (++dev.written_reqs == dev.unplug_at) {
        go_down(0, GONE, NULL);
        dev.down_until = 0;   /* stays gone */
        return;
    }
    dev.next = addr + len;
    if (dev.next >= dev.n) request(FM1_FINISH_WRITE, 8, 0);
    else request(dev.next, dev.n - dev.next < FM1_MAXDATA ? dev.n - dev.next : FM1_MAXDATA, 0);
}

static bool sim_send(const uint8_t *p, size_t n)
{
    if (!sim_up()) return false;
    if (n == sizeof FM1_HS_QUERY && memcmp(p, FM1_HS_QUERY, n) == 0) {
        identity(dev.id);
    } else if (n == sizeof FM1_UPGRADE && memcmp(p, FM1_UPGRADE, n) == 0) {
        if (dev.silent_after_upgrade) return true;
        if (dev.phase == RUNNING) {
            dev.phase = CHECKING;
            dev.check_reads = 0;
            request(0, 0x40, 1);
        } else if (dev.phase == LOADER) {
            dev.phase = WRITING;
            request(0, FM1_MAXDATA, 0);
        }
    } else if (dev.phase == CHECKING || dev.phase == WRITING) {
        answer(p, n);
    }
    return true;
}

static int sim_read(uint8_t *buf, size_t cap, uint32_t ms)
{
    if (!sim_up()) {
        dev.now += ms;
        return -1;
    }
    if (dev.qh == dev.qt) {
        dev.now += ms;
        return 0;
    }
    size_t n = dev.qn[dev.qh % 8];
    assert(n <= cap);
    memcpy(buf, dev.q[dev.qh % 8], n);
    dev.qh++;
    dev.now += 1;
    return n;
}

static void sim_drain(void) { dev.qh = dev.qt; }
static void sim_sleep(uint32_t ms) { dev.now += ms; }
static uint32_t sim_now(void) { return dev.now; }

static const fm1_io_t sim = { sim_up, sim_send, sim_read, sim_drain, sim_sleep, sim_now };

static void progress(fm1_step_t step, int n, const char *text, void *arg)
{
    (void)n;
    (void)arg;
    if (step == FM1_STEP_DONE) printf("  done: %s\n", text);
}

/* --------------------------------------------------------------------------------- the tests --- */

static void test_sysex(void)
{
    uint8_t data[FM1_MAXDATA], out[FM1_SYSEX_MAX], back[FM1_SYSEX_MAX];
    for (int i = 0; i < FM1_MAXDATA; i++) data[i] = i * 37 + 11;
    size_t n = fm1_response(0xDEADBEEF, data, FM1_MAXDATA, 5, out);
    CHECK(n == 605 && out[0] == 0xF0 && out[n - 1] == 0xF7);
    for (size_t i = 1; i < n - 1; i++) CHECK(out[i] < 0x80);
    size_t k = fm1_unpack7(out + 1, n - 2, back);
    CHECK(k == 7 + 4 + 3 + FM1_MAXDATA + 1 && memcmp(back + 14, data, FM1_MAXDATA) == 0);

    fm1_identity_t id;
    CHECK(!fm1_parse_identity(FM1_HS_QUERY, sizeof FM1_HS_QUERY, &id));
    CHECK(fm1_same_keyboard("ota-FM-1", "FM-1") && !fm1_same_keyboard("FM-1", "SMK-37 Pro"));
    CHECK(fm1_same_keyboard("SMK-37 Pro", "MKE-P37") && !fm1_same_keyboard("SMK-37", "MKE-P37"));
    CHECK(fm1_own_loader("ota-FM-1_906") && fm1_own_loader("OTA-smk-37 pro_999"));
    CHECK(!fm1_own_loader("ota-FM-1_015") && !fm1_own_loader("ota-FM-1_9061") && !fm1_own_loader("FM-1_906"));
}

static void package_line(const char *path, const uint8_t *raw, size_t rn, uint8_t *img)
{
    char product[FM1_ID_MAX], why[64];
    bool ok = fm1_product_of(raw, rn, product);
    size_t n = fm1_logical_image(raw, rn, img);
    const char *dmg = fm1_package_damage(img, n, why, sizeof why);
    fm1_board_t b = fm1_package_board(img, n);
    uint8_t resp[FM1_SYSEX_MAX];
    size_t rl = fm1_response(0x1234, img + 0x100, 512, 3, resp);
    const char *base = strrchr(path, '/');
    printf("%s: product=%s n=%zu crc=%04x damage=%s board=%s felucca=%d fm1=%s smk=%s resp=%zu:%04x\n",
           base ? base + 1 : path, ok ? product : "?", n, fm1_crc16(img, n, 0), dmg ? dmg : "none",
           b == FM1_BOARD_FM1 ? "fm1" : b == FM1_BOARD_SMK ? "smk" : "unknown", fm1_has_felucca_loader(raw, rn),
           fm1_board_refusal("FM-1", img, n) ? "refuse" : "ok", fm1_board_refusal("SMK-37 Pro", img, n) ? "refuse" : "ok",
           rl, fm1_crc16(resp, rl, 0));
}

static fm1_err_t run(const uint8_t *img, size_t n, const char *product, char *msg)
{
    return fm1_install(&sim, img, n, product, progress, NULL, msg, 160);
}

static void test_installs(const uint8_t *img, size_t n, const char *product)
{
    char msg[160];
    fm1_err_t e;

    /* the whole way: running -> loader -> written -> the new identity */
    sim_reset(img, n, "FM-1_015", product, RUNNING);
    e = run(img, n, product, msg);
    printf("  full install: %d %s (%u ms simulated)\n", e, msg, dev.now);
    CHECK(e == FM1_OK && dev.bad_answers == 0 && memcmp(dev.flash, img, n) == 0);

    /* interrupted earlier: the device starts in Felucca's loader, only the write */
    sim_reset(img, n, "ota-FM-1_906", product, LOADER);
    e = run(img, n, product, msg);
    CHECK(e == FM1_OK && memcmp(dev.flash, img, n) == 0);

    /* another firmware's loader: nothing is sent */
    sim_reset(img, n, "ota-FM-1_015", product, LOADER);
    e = run(img, n, product, msg);
    printf("  foreign loader: %d %s\n", e, msg);
    CHECK(e == FM1_ERR_FOREIGN && dev.written_reqs == 0);

    /* an SMK-37 with an FM-1 package: refused before anything */
    sim_reset(img, n, "SMK-37 Pro_906", product, RUNNING);
    e = run(img, n, product, msg);
    printf("  smk-37: %d %s\n", e, msg);
    CHECK(e == FM1_ERR_BOARD && dev.phase == RUNNING);

    /* the cable out during the write */
    sim_reset(img, n, "FM-1_015", product, RUNNING);
    dev.unplug_at = 100;
    e = run(img, n, product, msg);
    printf("  unplugged: %d %s\n", e, msg);
    CHECK(e == FM1_ERR_LOST);

    /* a firmware that does not take the update */
    sim_reset(img, n, "FM-1_015", product, RUNNING);
    dev.silent_after_upgrade = true;
    e = run(img, n, product, msg);
    printf("  silent: %d %s\n", e, msg);
    CHECK(e == FM1_ERR_STOPPED);

    /* written, but something else comes back */
    sim_reset(img, n, "FM-1_015", "FM-1_123", RUNNING);
    e = run(img, n, product, msg);
    printf("  mismatch: %d %s\n", e, msg);
    CHECK(e == FM1_ERR_MISMATCH);

    /* nothing on the cable */
    sim_reset(img, n, "FM-1_015", product, GONE);
    e = run(img, n, product, msg);
    CHECK(e == FM1_ERR_NOTFOUND);
}

int main(int argc, char **argv)
{
    test_sysex();
    static uint8_t img[1 << 20];
    for (int i = 1; i < argc; i++) {
        size_t rn;
        uint8_t *raw = load(argv[i], &rn);
        package_line(argv[i], raw, rn, img);
        free(raw);
    }
    /* the installs, with the last package */
    if (argc > 1) {
        size_t rn;
        uint8_t *raw = load(argv[argc - 1], &rn);
        char product[FM1_ID_MAX];
        fm1_product_of(raw, rn, product);
        size_t n = fm1_logical_image(raw, rn, raw);
        fprintf(stderr, "installs with %s (%s):\n", argv[argc - 1], product);
        fflush(stdout);
        int saved = dup(1);
        dup2(2, 1);   /* the install lines to stderr: stdout is diffed against pkg_ref.py */
        test_installs(raw, n, product);
        fflush(stdout);
        dup2(saved, 1);
        free(raw);
    }
    if (failures) fprintf(stderr, "%d CHECKs failed\n", failures);
    else fprintf(stderr, "all CHECKs passed\n");
    return failures != 0;
}
