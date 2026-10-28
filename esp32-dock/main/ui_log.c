/* SPDX-License-Identifier: GPL-3.0-only */
/* The screen's part, on the log: every ui_* the worker calls (fm1-flasher-p4/components/flasher/ui.h)
 * is a line here. The console (console.c) shows the lists on request. */
#include "ui.h"
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "dock";

void ui_build(void) {}

void ui_device(const dev_info_t *d)
{
    switch (d->state) {
    case DEV_NONE: ESP_LOGI(TAG, "USB: nothing on the port"); break;
    case DEV_PROBING: ESP_LOGI(TAG, "USB: %s, asking who it is", d->usb_name); break;
    case DEV_FM1:
        if (d->dock)
            ESP_LOGI(TAG, "USB: %s runs fm1-dock (ABI %u, %u modules, %lu KB free, room for %lu KB)", d->id.text, d->hello.abi,
                     d->hello.n_mods, (unsigned long)(d->hello.free / 1024), (unsigned long)(d->hello.largest / 1024));
        else
            ESP_LOGI(TAG, "USB: %s", d->id.text);
        break;
    case DEV_LOADER: ESP_LOGI(TAG, "USB: %s is in update mode (an install was cut off): install again to finish it", d->id.text); break;
    case DEV_OTHER: ESP_LOGI(TAG, "USB: %s, a MIDI device that does not answer as an FM-1", d->usb_name); break;
    }
}

void ui_net(net_state_t st, const char *ssid)
{
    static const char *const S[] = { "off", "connecting", "up", "failed" };
    ESP_LOGI(TAG, "WiFi: %s%s%s", S[st], ssid[0] ? " " : "", ssid);
}

void ui_catalog(bool ok, bool offline, const char *err)
{
    if (!ok) ESP_LOGW(TAG, "catalog: %s", err);
    else ESP_LOGI(TAG, "catalog: %d firmwares%s", catalog_n, offline ? " (the card's copy)" : "");
}

void ui_saved(const fw_entry_t *e, bool on_card, const char *why)
{
    if (why[0]) ESP_LOGW(TAG, "card: %s %s: %s", e->name, e->version, why);
    else ESP_LOGI(TAG, "card: %s %s %s", e->name, e->version, on_card ? "saved" : "removed");
}

void ui_save_progress(int done, int total, const char *name) { ESP_LOGI(TAG, "card: %d of %d, %s", done, total, name); }
void ui_save_done(int saved, int failed, bool stopped)
{
    ESP_LOGI(TAG, "card: %d saved, %d failed%s", saved, failed, stopped ? ", stopped" : "");
}

void ui_download(size_t got, size_t total)
{
    static size_t last;
    if (got == total || got - last >= 65536 || got < last) {
        last = got;
        ESP_LOGI(TAG, "fetching: %u of %u KB", (unsigned)(got / 1024), (unsigned)(total / 1024));
    }
}

void ui_prepared(const fw_package_t *p, bool ok, const char *why)
{
    if (!ok) ESP_LOGW(TAG, "not installable: %s", why);
    else ESP_LOGI(TAG, "ready to install: %s %s -> %s (%u bytes%s%s)", p->entry.name, p->entry.version, p->product, (unsigned)p->n,
                  p->official ? ", M-VAVE's own" : "", p->from_card ? ", from the card" : "");
}

void ui_install_step(fm1_step_t step, int n, const char *text)
{
    static const char *const S[] = { "start", "the firmware reads the package", "the update loader", "writing", "restarting", "done" };
    ESP_LOGI(TAG, "install: %s %d %s", S[step], n, text ? text : "");
}

void ui_install_done(fm1_err_t e, const char *msg)
{
    if (e == FM1_OK) ESP_LOGI(TAG, "installed: %s", msg);
    else ESP_LOGW(TAG, "install stopped (%d): %s", e, msg);
}

void ui_modules(bool ok, bool offline, const char *err)
{
    if (!ok) ESP_LOGW(TAG, "modules: %s", err);
    else ESP_LOGI(TAG, "modules: %d%s", modules_n, offline ? " (the card's copy)" : "");
}

void ui_dock(void)
{
    fm1_dock_module_t m[FM1_DOCK_MODS_MAX];
    int n = flasher_dock_modules(m, FM1_DOCK_MODS_MAX);
    for (int i = 0; i < n; i++)
        ESP_LOGI(TAG, "FM-1 module %d: %s v%lu %s (%lu KB)", m[i].index, m[i].name, (unsigned long)m[i].version,
                 fm1_dock_state(m[i].state), (unsigned long)(m[i].size / 1024));
}

void ui_mod_progress(int pct)
{
    static int last;
    if (pct == 100 || pct - last >= 25 || pct < last) {
        last = pct;
        ESP_LOGI(TAG, "deploy: %d%%", pct);
    }
}

void ui_mod_done(bool ok, const char *msg) { if (ok) ESP_LOGI(TAG, "%s", msg); else ESP_LOGW(TAG, "%s", msg); }

void ui_mod_saved(const module_entry_t *m, bool on_card, const char *why)
{
    if (why[0]) ESP_LOGW(TAG, "card: %s: %s", m->title, why);
    else ESP_LOGI(TAG, "card: %s %s", m->title, on_card ? "saved" : "removed");
}

void ui_wifi_list(const wifi_ap_record_t *aps, int n)
{
    for (int i = 0; i < n; i++) ESP_LOGI(TAG, "WiFi %2d: %-32s %d dBm", i, (const char *)aps[i].ssid, aps[i].rssi);
}

void ui_library(const char *text) { (void)text; }   /* the worker logs it already */
