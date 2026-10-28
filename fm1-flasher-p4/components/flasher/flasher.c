/* SPDX-License-Identifier: GPL-3.0-only */
#include "flasher.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb_midi.h"
#include "net.h"
#include "library.h"
#include "ui.h"

static const char *TAG = "flasher";

typedef enum {
    CMD_REFRESH, CMD_PREPARE, CMD_INSTALL, CMD_SAVE, CMD_REMOVE, CMD_SAVE_ALL, CMD_WIFI_SCAN, CMD_WIFI_CONNECT,
    CMD_MOD_DEPLOY, CMD_MOD_SAVE, CMD_MOD_REMOVE, CMD_DOCK_SELECT, CMD_DOCK_DELETE, CMD_DOCK_LIST, CMD_BROWSE,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    fw_entry_t entry;
    module_entry_t module;
    char name[24];
    char ssid[33], pass[65];
} cmd_t;

static QueueHandle_t cmds;
static dev_info_t dev;
static portMUX_TYPE dev_lock = portMUX_INITIALIZER_UNLOCKED;
static fw_package_t pkg;
static fm1_dock_module_t dock_mods[FM1_DOCK_MODS_MAX];   /* what fm1-dock on the FM-1 has */
static int dock_n;

static void io_sleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
static uint32_t io_now(void) { return esp_timer_get_time() / 1000; }

static const fm1_io_t io = { usb_midi_up, usb_midi_send, usb_midi_read, usb_midi_drain, io_sleep, io_now };

static void set_device(dev_state_t st, const fm1_identity_t *id, const fm1_dock_hello_t *dock)
{
    dev_info_t d = { .state = st };
    usb_midi_name(d.usb_name, sizeof d.usb_name);
    if (id) d.id = *id;
    if (dock) {
        d.dock = true;
        d.hello = *dock;
    }
    portENTER_CRITICAL(&dev_lock);
    dev = d;
    portEXIT_CRITICAL(&dev_lock);
    ui_device(&d);
}

void flasher_device(dev_info_t *out)
{
    portENTER_CRITICAL(&dev_lock);
    *out = dev;
    portEXIT_CRITICAL(&dev_lock);
}

int flasher_dock_modules(fm1_dock_module_t *out, int max)
{
    portENTER_CRITICAL(&dev_lock);
    int n = dock_n < max ? dock_n : max;
    memcpy(out, dock_mods, n * sizeof *out);
    portEXIT_CRITICAL(&dev_lock);
    return n;
}

/* fm1-dock's module list, again (after a deploy, a select, a delete) */
static void dock_list(void)
{
    static fm1_dock_module_t m[FM1_DOCK_MODS_MAX];
    int n = fm1_dock_list(&io, m, FM1_DOCK_MODS_MAX);
    portENTER_CRITICAL(&dev_lock);
    dock_n = n < 0 ? 0 : n;
    memcpy(dock_mods, m, dock_n * sizeof *m);
    portEXIT_CRITICAL(&dev_lock);
    if (n >= 0) {
        fm1_dock_hello_t h;
        dev_info_t d;
        flasher_device(&d);
        if (fm1_dock_hello(&io, &h, 2)) set_device(d.state, &d.id, &h);   /* the free space, the module playing */
    }
    ui_dock();
}

static void probe(void)
{
    fm1_identity_t id;
    fm1_dock_hello_t h;
    portENTER_CRITICAL(&dev_lock);
    dock_n = 0;
    portEXIT_CRITICAL(&dev_lock);
    if (!usb_midi_up()) {
        set_device(DEV_NONE, NULL, NULL);
        ui_dock();
        return;
    }
    set_device(DEV_PROBING, NULL, NULL);
    vTaskDelay(pdMS_TO_TICKS(300));   /* a device just opened may not hear the first message */
    if (fm1_handshake(&io, &id, 3)) {
        ESP_LOGI(TAG, "device: %s", id.text);
        bool loader = fm1_id_is_loader(&id);
        bool dock = !loader && fm1_dock_hello(&io, &h, 2);   /* fm1-dock answers both the updater and its own */
        if (dock) ESP_LOGI(TAG, "fm1-dock: ABI %u, %u modules, %lu KB free", h.abi, h.n_mods, (unsigned long)(h.free / 1024));
        set_device(loader ? DEV_LOADER : DEV_FM1, &id, dock ? &h : NULL);
        if (dock) dock_list();
        else ui_dock();
    } else {
        set_device(DEV_OTHER, NULL, NULL);
        ui_dock();
    }
}

static void on_download(size_t got, size_t total) { ui_download(got, total); }

static void on_mod_progress(int pct, void *arg) { ui_mod_progress(pct); }

static void mod_deploy(const module_entry_t *m)
{
    char msg[200];
    uint8_t *blob;
    int n = module_fetch(m, &blob, on_download, msg, sizeof msg);
    if (n < 0) {
        ui_mod_done(false, msg);
        return;
    }
    ESP_LOGI(TAG, "deploying %s (%d bytes)", m->title, n);
    bool ok = fm1_dock_deploy(&io, blob, n, on_mod_progress, NULL, msg, sizeof msg);
    free(blob);
    ESP_LOGI(TAG, "deploy: %s", msg);
    ui_mod_done(ok, msg);
    dock_list();
}

static void on_step(fm1_step_t step, int n, const char *text, void *arg) { ui_install_step(step, n, text); }

static void install(void)
{
    char msg[200];
    if (!pkg.img) {
        ui_install_done(FM1_ERR_NOTFOUND, "no package prepared");
        return;
    }
    ESP_LOGI(TAG, "installing %s %s (%s, %u bytes)", pkg.entry.name, pkg.entry.version, pkg.product, (unsigned)pkg.n);
    fm1_err_t e = fm1_install(&io, pkg.img, pkg.n, pkg.product, on_step, NULL, msg, sizeof msg);
    if (e == FM1_OK) ESP_LOGI(TAG, "installed: %s", pkg.product);
    else ESP_LOGW(TAG, "install stopped (%d): %s", e, msg);
    ui_install_done(e, msg);
}

/* ------------------------------------------------------------------------------ the library --- */

static bool lib_on, lib_running, lib_want;
static uint32_t lib_gen;              /* the connection the browser was sent on (once per plug-in) */
static const uint8_t *lib_probe;      /* the app's browser, else the card's */
static size_t lib_probe_n;
static struct { uint8_t kind; int index; } lib_map[CATALOG_MAX + MODULES_MAX];   /* the table's lines */
static int lib_map_n;

void flasher_library(bool on) { lib_on = on; }
void flasher_library_probe(const uint8_t *blob, size_t n) { lib_probe = blob; lib_probe_n = n; }

static void lib_say(const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "library: %s", text);
    ui_library(text);
}

static void wr16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }

/* the table: the modules (fm1-dock's page's and the card's), then the firmwares */
static size_t lib_table(uint8_t *t, size_t cap, const dev_info_t *d)
{
    size_t n = 0;
    lib_map_n = 0;
    for (int i = 0; i < modules_n && n < cap; i++) {
        const module_entry_t *m = &modules[i];
        uint8_t *e = t + LIB_HEAD + n * LIB_ENTRY;
        memset(e, 0, LIB_ENTRY);
        e[0] = LIB_MODULE;
        e[1] = (m->size <= d->hello.largest ? LIB_F_FITS : 0) | (m->on_card ? LIB_F_CARD : 0);
        wr16(e + 2, (m->size + 1023) / 1024);
        strncpy((char *)e + 4, m->title, 20);
        strncpy((char *)e + 24, m->version, 8);
        lib_map[lib_map_n].kind = LIB_MODULE;
        lib_map[lib_map_n++].index = i;
        n++;
    }
    const fw_entry_t *now = catalog_installed(d->id.text);
    for (int i = 0; i < catalog_n && n < cap; i++) {
        const fw_entry_t *f = &catalog[i];
        uint8_t *e = t + LIB_HEAD + n * LIB_ENTRY;
        memset(e, 0, LIB_ENTRY);
        e[0] = LIB_FIRMWARE;
        e[1] = (f->on_card || f->src == SRC_SD ? LIB_F_CARD : 0) | (f == now ? LIB_F_INSTALLED : 0) | (f->beta ? LIB_F_BETA : 0) |
               (f->src == SRC_STOCK ? LIB_F_OFFICIAL : 0);
        wr16(e + 2, (f->size + 1023) / 1024);
        strncpy((char *)e + 4, f->name, 20);
        strncpy((char *)e + 24, f->version, 8);
        lib_map[lib_map_n].kind = LIB_FIRMWARE;
        lib_map[lib_map_n++].index = i;
        n++;
    }
    return n;
}

/* the browser sent and started on the FM-1, with the library in it */
static void lib_start(void)
{
    char msg[160];
    dev_info_t d;
    uint8_t *blob = NULL;
    size_t n;
    flasher_device(&d);
    lib_want = false;
    lib_gen = usb_midi_generation();
    if (lib_probe) {
        n = lib_probe_n;
        blob = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (blob) memcpy(blob, lib_probe, n);
    } else {
        int k = catalog_card_file("browser.dock", &blob, FM1_DOCK_PROBE_FILE);
        if (k < 0) {
            lib_say("no browser: none built in, none on the card (/fm1/browser.dock)");
            return;
        }
        n = k;
    }
    if (!blob) return;
    long at = fm1_dock_probe_find(blob, n, LIB_MAGIC);
    if (at < 0) {
        lib_say("the browser probe has no library table in it");
        free(blob);
        return;
    }
    fm1_dock_info_t info;
    fm1_dock_module_info(blob, n, &info);
    const uint8_t *head = blob + info.image + info.code_size + at;
    size_t cap = head[4] | (head[5] << 8);
    if (cap > (size_t)(CATALOG_MAX + MODULES_MAX)) cap = CATALOG_MAX + MODULES_MAX;
    size_t tn = LIB_HEAD + cap * LIB_ENTRY;
    uint8_t *t = calloc(1, tn);
    if (!t) {
        free(blob);
        return;
    }
    memcpy(t, head, LIB_HEAD);
    size_t filled = lib_table(t, cap, &d);
    wr16(t + 6, filled);
    bool ok = fm1_dock_probe_patch(blob, n, at, t, tn);
    free(t);
    if (ok) ok = fm1_dock_run(&io, blob, n, LIB_RUN_ID, NULL, 0, NULL, NULL, msg, sizeof msg);
    free(blob);
    if (!ok) {
        lib_say("%s", msg);
        return;
    }
    lib_running = true;
    int nm = 0;
    for (int i = 0; i < lib_map_n; i++) nm += lib_map[i].kind == LIB_MODULE;
    lib_say("the browser is on the FM-1: %d modules, %d firmwares", nm, lib_map_n - nm);
    (void)filled;
}


/* what the FM-1 picked */
static void lib_pick(int32_t rc)
{
    if (rc <= 0) {
        lib_say("the browser was left (nothing picked); again at the next plug-in, or browse");
        return;
    }
    int i = rc - 1;
    if (i >= lib_map_n) {
        lib_say("the pick (%ld) is outside the list", (long)rc);
        return;
    }
    if (lib_map[i].kind == LIB_MODULE) {
        module_entry_t m = modules[lib_map[i].index];
        lib_say("picked the module %s", m.title);
        mod_deploy(&m);
    } else {
        fw_entry_t e = catalog[lib_map[i].index];
        char why[200] = "";
        lib_say("picked the firmware %s %s (the risk accepted on the FM-1)", e.name, e.version);
        package_free(&pkg);
        bool ok = package_prepare(&e, &pkg, on_download, why, sizeof why);
        ui_prepared(&pkg, ok, why);
        if (!ok) {
            lib_say("%s %s: %s", e.name, e.version, why);
            return;
        }
        install();
        probe();
    }
}

/* the run's end, when it comes; between times the worker goes on */
static void lib_poll(void)
{
    if (!lib_running) return;
    int r = fm1_dock_run_wait(&io, 100);
    if (r == FM1_DOCK_WAITING) return;
    lib_running = false;
    if (r == -1) {
        lib_say("the FM-1 went away while browsing");
    } else if (r == FM1_DOCK_LOST) {
        lib_say("the browser never started (the start was lost): sent again");
        lib_want = true;
    } else if (r == FM1_DOCK_CRASHED) {
        fm1_dock_result_t res;
        uint8_t none[1];
        fm1_dock_result(&io, &res, none, 0);
        lib_say("the browser %s on the FM-1 (pc %08lx)", fm1_dock_probe_state(res.state), (unsigned long)res.pc);
    } else if (r > 0) {
        lib_say("fm1-dock refused the browser: %s", fm1_dock_status(r));
    } else {
        fm1_dock_result_t res;
        uint8_t none[1];
        if (fm1_dock_result(&io, &res, none, 0) < 0 || res.id != LIB_RUN_ID) lib_say("the browser's result did not come");
        else lib_pick(res.rc);
    }
}

static bool hub_loaded;   /* from the net, not the card's copy */
static volatile bool saving, save_stop;

static void refresh(void)
{
    char err[64] = "";
    bool offline = false, mod_offline = false;
    catalog_scan_sd();
    bool ok = catalog_fetch(&offline, err, sizeof err);
    hub_loaded |= ok && !offline;
    ui_catalog(ok, offline, err);
    char merr[64] = "";
    bool mok = modules_fetch(&mod_offline, merr, sizeof merr);
    ui_modules(mok, mod_offline, merr);
}

/* the device (probed when it comes or goes) and the network; between requests, and between the
 * packages of a long save */
static void poll(void)
{
    static uint32_t gen;
    static bool was_up;
    static net_state_t net = -1;
    bool up = usb_midi_up();
    if (up != was_up || (up && usb_midi_generation() != gen)) {
        was_up = up;
        gen = usb_midi_generation();
        lib_running = false;
        probe();
    }
    if (lib_on && up && !lib_running && !saving) {
        dev_info_t d;
        flasher_device(&d);
        if (d.dock && (lib_want || gen != lib_gen)) lib_start();
    }
    net_state_t ns = net_state();
    if (ns != net) {
        char ssid[33];
        net = ns;
        net_ssid(ssid, sizeof ssid);
        ui_net(ns, ssid);
        if (ns == NET_UP && !hub_loaded && !saving) refresh();
    }
}

static void on_save(int done, int total, const char *name)
{
    ui_save_progress(done, total, name);
    poll();
}

static void task(void *arg)
{
    cmd_t c;
    refresh();   /* the card's catalog and packages, before (or without) WiFi */
    for (;;) {
        lib_poll();
        if (xQueueReceive(cmds, &c, pdMS_TO_TICKS(lib_running ? 50 : 300)) == pdTRUE) {
            switch (c.kind) {
            case CMD_REFRESH:
                refresh();
                break;
            case CMD_PREPARE: {
                char why[200] = "";
                package_free(&pkg);
                bool ok = package_prepare(&c.entry, &pkg, on_download, why, sizeof why);
                ui_prepared(&pkg, ok, why);
                break;
            }
            case CMD_INSTALL:
                install();
                probe();
                break;
            case CMD_SAVE: {
                char why[160] = "";
                bool ok = catalog_save(&c.entry, on_download, why, sizeof why);
                ui_saved(&c.entry, ok, why);
                break;
            }
            case CMD_REMOVE:
                ui_saved(&c.entry, !catalog_remove(&c.entry), "");
                break;
            case CMD_SAVE_ALL: {
                int failed = 0;
                saving = true;
                save_stop = false;
                int saved = catalog_save_all(on_save, &save_stop, &failed);
                saving = false;
                ui_save_done(saved, failed, save_stop);
                break;
            }
            case CMD_MOD_DEPLOY:
                mod_deploy(&c.module);
                break;
            case CMD_MOD_SAVE: {
                char why[160] = "";
                bool ok = module_save(&c.module, on_download, why, sizeof why);
                ui_mod_saved(&c.module, ok, why);
                break;
            }
            case CMD_MOD_REMOVE:
                ui_mod_saved(&c.module, !module_remove(&c.module), "");
                break;
            case CMD_DOCK_SELECT:
            case CMD_DOCK_DELETE: {
                char msg[96];
                int r = c.kind == CMD_DOCK_SELECT ? fm1_dock_select(&io, c.name) : fm1_dock_delete(&io, c.name);
                snprintf(msg, sizeof msg, "%s %s: %s", c.kind == CMD_DOCK_SELECT ? "Play" : "Delete", c.name,
                         r < 0 ? "no answer" : fm1_dock_status(r));
                ui_mod_done(r == 0, msg);
                dock_list();
                break;
            }
            case CMD_DOCK_LIST:
                dock_list();
                break;
            case CMD_BROWSE:
                lib_want = true;
                break;
            case CMD_WIFI_SCAN: {
                static wifi_ap_record_t aps[24];
                int n = net_scan(aps, 24);
                ui_wifi_list(aps, n);
                break;
            }
            case CMD_WIFI_CONNECT:
                net_connect(c.ssid, c.pass, 15000);
                break;
            }
        }
        poll();
    }
}

static void post(const cmd_t *c)
{
    if (xQueueSend(cmds, c, 0) != pdTRUE) ESP_LOGW(TAG, "busy: request %d dropped", c->kind);
}

void flasher_start(void)
{
    cmds = xQueueCreate(4, sizeof(cmd_t));
    usb_midi_start();
    xTaskCreatePinnedToCore(task, "flasher", 12288, NULL, 5, NULL, 1);
}

/* the requests are built in statics: a cmd_t is 1 KB, too much for the LVGL task's stack (one UI task) */
static void post_kind(cmd_kind_t k)
{
    static cmd_t c;
    c.kind = k;
    post(&c);
}

void flasher_refresh(void) { post_kind(CMD_REFRESH); }
void flasher_install(void) { post_kind(CMD_INSTALL); }
void flasher_wifi_scan(void) { post_kind(CMD_WIFI_SCAN); }

void flasher_save_all(void)
{
    if (saving) save_stop = true;
    else post_kind(CMD_SAVE_ALL);
}

bool flasher_saving(void) { return saving; }

static void post_entry(cmd_kind_t k, const fw_entry_t *e)
{
    static cmd_t c;
    c.kind = k;
    c.entry = *e;
    post(&c);
}

static void post_module(cmd_kind_t k, const module_entry_t *m)
{
    static cmd_t c;
    c.kind = k;
    c.module = *m;
    post(&c);
}

static void post_name(cmd_kind_t k, const char *name)
{
    static cmd_t c;
    c.kind = k;
    snprintf(c.name, sizeof c.name, "%s", name);
    post(&c);
}

void flasher_mod_deploy(const module_entry_t *m) { post_module(CMD_MOD_DEPLOY, m); }
void flasher_mod_save(const module_entry_t *m) { post_module(CMD_MOD_SAVE, m); }
void flasher_mod_remove(const module_entry_t *m) { post_module(CMD_MOD_REMOVE, m); }
void flasher_dock_select(const char *name) { post_name(CMD_DOCK_SELECT, name); }
void flasher_dock_delete(const char *name) { post_name(CMD_DOCK_DELETE, name); }
void flasher_dock_list(void) { post_kind(CMD_DOCK_LIST); }
void flasher_browse(void) { post_kind(CMD_BROWSE); }

void flasher_prepare(const fw_entry_t *e) { post_entry(CMD_PREPARE, e); }
void flasher_save(const fw_entry_t *e) { post_entry(CMD_SAVE, e); }
void flasher_remove(const fw_entry_t *e) { post_entry(CMD_REMOVE, e); }

void flasher_wifi_connect(const char *ssid, const char *pass)
{
    static cmd_t c;
    c.kind = CMD_WIFI_CONNECT;
    snprintf(c.ssid, sizeof c.ssid, "%s", ssid);
    snprintf(c.pass, sizeof c.pass, "%s", pass);
    post(&c);
}
