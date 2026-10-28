/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1_install.py's Updater (fm1-dock's, after Felucca's), for one device on the link. */
#include "fm1_ota.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

fm1_delays_t fm1_delay = {
    .start = 2000, .reply = 10, .loader = 3000, .reboot = 3000, .retry = 1000, .idle_check = 8000,
    .idle_write = 180000, .wait_loader = 30000, .wait_reboot = 40000, .hs = 1000,
};

static fm1_err_t fail(fm1_err_t e, char *msg, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, len, fmt, ap);
    va_end(ap);
    return e;
}

bool fm1_handshake(const fm1_io_t *io, fm1_identity_t *id, int tries)
{
    uint8_t pkt[FM1_SYSEX_MAX];
    if (!io->up()) return false;
    io->drain();
    for (int t = 0; t < tries; t++) {
        if (!io->send(FM1_HS_QUERY, sizeof FM1_HS_QUERY)) return false;
        uint32_t end = io->now_ms() + fm1_delay.hs;
        for (int32_t left; (left = (int32_t)(end - io->now_ms())) > 0;) {
            int n = io->read(pkt, sizeof pkt, left);
            if (n < 0) return false;
            if (n == 0) break;
            if (fm1_parse_identity(pkt, n, id)) return true;
        }
    }
    return false;
}

typedef bool (*want_t)(const fm1_identity_t *id, const char *model);

static bool want_running(const fm1_identity_t *id, const char *model) { return !fm1_id_is_loader(id) && fm1_same_keyboard(id->model, model); }
static bool want_loader(const fm1_identity_t *id, const char *model) { return fm1_id_is_loader(id) && fm1_same_keyboard(id->model, model); }

/* the device answering the handshake as want() wants, within ms */
static bool wait_for(const fm1_io_t *io, want_t want, const char *model, uint32_t ms, fm1_identity_t *id)
{
    uint32_t end = io->now_ms() + ms;
    while ((int32_t)(end - io->now_ms()) > 0) {
        if (fm1_handshake(io, id, 2) && want(id, model)) return true;
        io->sleep_ms(fm1_delay.retry);
    }
    return false;
}

typedef struct {
    int served;
    bool finished, lost;
} served_t;

/* answer read requests until the device asks for finish */
static fm1_err_t serve(const fm1_io_t *io, const uint8_t *image, size_t n, uint32_t finish, uint32_t idle,
                       fm1_step_t step, fm1_progress_t progress, void *arg, served_t *s, char *msg, size_t msglen)
{
    static uint8_t pkt[FM1_SYSEX_MAX], out[FM1_SYSEX_MAX];
    static const uint8_t success[] = "success";   /* with its 0 */
    uint32_t last = io->now_ms();
    *s = (served_t){ 0 };
    for (;;) {
        int k = io->read(pkt, sizeof pkt, 1000);
        if (k < 0) {
            s->lost = true;
            return FM1_OK;
        }
        if (k == 0) {
            if (!io->up()) {
                s->lost = true;
                return FM1_OK;
            }
            if (io->now_ms() - last > idle) return FM1_OK;
            continue;
        }
        fm1_request_t r;
        if (!fm1_parse_request(pkt, k, &r)) continue;
        last = io->now_ms();
        if (r.addr == FM1_FINISH_CHECK || r.addr == FM1_FINISH_WRITE) {
            io->send(out, fm1_response(r.addr, success, sizeof success, 0, out));
            if (r.addr == finish) {
                s->finished = true;
                return FM1_OK;
            }
            continue;
        }
        if (r.len > FM1_MAXDATA || (uint64_t)r.addr + r.len > n)
            return fail(FM1_ERR_BADREQ, msg, msglen, "the device asked for 0x%lx+%lu, outside the package",
                        (unsigned long)r.addr, (unsigned long)r.len);
        io->sleep_ms(fm1_delay.reply);
        if (!io->send(out, fm1_response(r.addr, image + r.addr, r.len, r.flags, out))) {
            s->lost = true;
            return FM1_OK;
        }
        s->served++;
        if (progress) {
            int pct = (int)((uint64_t)(r.addr + r.len) * 100 / n);
            progress(step, step == FM1_STEP_WRITE ? (pct > 99 ? 99 : pct) : s->served, NULL, arg);
        }
    }
}

fm1_err_t fm1_install(const fm1_io_t *io, const uint8_t *image, size_t n, const char *product,
                      fm1_progress_t progress, void *arg, char *msg, size_t msglen)
{
    char model[FM1_ID_MAX];
    fm1_identity_t dev, ota, back;
    served_t s;
    fm1_err_t e;
    msg[0] = 0;
    snprintf(model, sizeof model, "%s", product);
    char *us = strchr(model, '_');
    if (us) *us = 0;

    if (!fm1_handshake(io, &dev, 3)) return fail(FM1_ERR_NOTFOUND, msg, msglen, "no FM-1 answered on USB");
    const char *refusal = fm1_board_refusal(fm1_bare_model(dev.model), image, n);
    if (refusal) return fail(FM1_ERR_BOARD, msg, msglen, "%s (%s): nothing was sent", refusal, dev.text);
    if (!fm1_same_keyboard(dev.model, model))
        return fail(FM1_ERR_MODEL, msg, msglen, "the device is %s, the package is for %s", dev.text, product);

    if (fm1_id_is_loader(&dev)) {
        /* an install was cut off: finish the write, but only with Felucca's own loader (another
         * firmware's may lay the image out differently) */
        if (!fm1_own_loader(dev.text))
            return fail(FM1_ERR_FOREIGN, msg, msglen, "the FM-1 is in the update mode of another firmware (%s): finish that "
                        "update with its own updater, then install again", dev.text);
        ota = dev;
    } else {
        if (progress) progress(FM1_STEP_START, 0, dev.text, arg);
        io->send(FM1_UPGRADE, sizeof FM1_UPGRADE);
        io->sleep_ms(fm1_delay.start);
        if ((e = serve(io, image, n, FM1_FINISH_CHECK, fm1_delay.idle_check, FM1_STEP_CHECK, progress, arg, &s, msg, msglen)))
            return e;
        if (!s.finished)
            return fail(s.lost ? FM1_ERR_LOST : FM1_ERR_STOPPED, msg, msglen, "the FM-1 %s after %d requests; nothing was written",
                        s.lost ? "was disconnected" : "stopped answering", s.served);
        if (progress) progress(FM1_STEP_LOADER, 0, NULL, arg);
        io->sleep_ms(fm1_delay.loader);
        if (!wait_for(io, want_loader, model, fm1_delay.wait_loader, &ota))
            return fail(FM1_ERR_NOLOADER, msg, msglen, "the update loader did not appear. Replug the FM-1 and install again: "
                        "it stays in update mode until it is done");
    }

    if (progress) progress(FM1_STEP_WRITE, 0, NULL, arg);
    io->send(FM1_UPGRADE, sizeof FM1_UPGRADE);
    io->sleep_ms(fm1_delay.start);
    if ((e = serve(io, image, n, FM1_FINISH_WRITE, fm1_delay.idle_write, FM1_STEP_WRITE, progress, arg, &s, msg, msglen)))
        return e;
    if (!s.finished)
        return fail(s.lost ? FM1_ERR_LOST : FM1_ERR_STOPPED, msg, msglen, "the loader %s after %d requests. Replug the FM-1 and "
                    "install again to finish", s.lost ? "was disconnected" : "stopped answering", s.served);
    if (progress) progress(FM1_STEP_WRITE, 100, NULL, arg);

    if (progress) progress(FM1_STEP_REBOOT, 0, NULL, arg);
    io->sleep_ms(fm1_delay.reboot);
    if (!wait_for(io, want_running, model, fm1_delay.wait_reboot, &back))
        return fail(FM1_ERR_NORETURN, msg, msglen, "the FM-1 did not come back: switch it off and on");
    if (strcmp(back.text, product) != 0)
        return fail(FM1_ERR_MISMATCH, msg, msglen, "written, but the FM-1 reports %s (expected %s)", back.text, product);
    if (progress) progress(FM1_STEP_DONE, 0, back.text, arg);
    return FM1_OK;
}
