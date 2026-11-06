/* SPDX-License-Identifier: GPL-3.0-only */
#include "flasher.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb_midi.h"
#include "net.h"
#include "ui.h"

static const char *TAG = "flasher";

typedef enum {
    CMD_REFRESH, CMD_PREPARE, CMD_INSTALL, CMD_SAVE, CMD_REMOVE, CMD_SAVE_ALL, CMD_WIFI_SCAN, CMD_WIFI_CONNECT,
    CMD_MOD_DEPLOY, CMD_MOD_SAVE, CMD_MOD_REMOVE, CMD_DOCK_SELECT, CMD_DOCK_DELETE, CMD_DOCK_LIST,
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
        probe();
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
        if (xQueueReceive(cmds, &c, pdMS_TO_TICKS(300)) == pdTRUE) {
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
