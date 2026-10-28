/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define W 480
#define H 320
#define HEADER 34

#define C_BG 0x14161a
#define C_PANEL 0x1f232a
#define C_TEXT 0xe6e8eb
#define C_DIM 0x8a9099
#define C_OK 0x4caf6a
#define C_WARN 0xe0a030
#define C_BAD 0xe0524a
#define C_ACCENT 0x3d8bfd

static lv_obj_t *dev_label, *net_label;
static lv_obj_t *fw_list, *sd_list, *fw_status, *sd_status, *save_btn;
static lv_obj_t *wifi_status, *wifi_list, *wifi_panel, *wifi_title, *wifi_pass;
static char wifi_ssid[33];
static char wifi_found[24][33];   /* the scan's networks, by button */

/* the module page (fm1-dock's modules): built further down */
static lv_obj_t *mpage, *mpage_status, *mpage_bar;
static void build_modules(lv_obj_t *tab);
static void build_mpage(void);

/* the firmware page */
typedef enum { P_INFO, P_FETCHING, P_READY, P_REFUSED, P_INSTALLING, P_DONE } page_t;
static page_t page_state;
static lv_obj_t *page, *page_title, *page_info, *page_status, *page_bar, *page_accept;
static lv_obj_t *btn_back, *btn_get, *btn_install, *btn_card;
static bool card_busy;   /* a save or remove of the page's firmware is under way */
static fw_entry_t page_entry;
static bool page_resume;   /* the FM-1 is in update mode: the install finishes an earlier write */

/* -1: wait as long as it takes (0 is "do not wait" to the adapter, and an update would go unlocked) */
static void lock(void) { esp_lv_adapter_lock(-1); }
static void unlock(void) { bsp_display_unlock(); }

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_height(b, 40);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void enable(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

static void show(lv_obj_t *o, bool on)
{
    lv_obj_set_hidden(o, !on);
}

/* a list: a scrolling flex column of section lines and full-width buttons (lv_list is deprecated) */
static lv_obj_t *list_create(lv_obj_t *parent)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(l, 4, 0);
    lv_obj_set_style_pad_row(l, 4, 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(l, 0, 0);
    return l;
}

static void list_add_text(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = label(list, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(l, text);
    lv_obj_set_style_pad_top(l, 6, 0);
}

static lv_obj_t *list_add_button(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(list);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_t *l = label(b, &lv_font_montserrat_14, C_TEXT);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

static bool device_ready(dev_info_t *d)
{
    flasher_device(d);
    return d->state == DEV_FM1 || d->state == DEV_LOADER;
}

/* ------------------------------------------------------------------------- the firmware page --- */

static void page_buttons(void)
{
    dev_info_t d;
    bool ready = device_ready(&d);
    show(btn_back, page_state != P_INSTALLING);
    enable(btn_back, page_state != P_FETCHING);
    lv_label_set_text(lv_obj_get_child(btn_back, 0), page_state == P_DONE ? "Done" : "Back");
    show(btn_get, page_state == P_INFO || page_state == P_FETCHING);
    /* the card: save this one (or remove it), not while it is being fetched or installed */
    show(btn_card, page_entry.src != SRC_SD && page_entry.local[0] &&
                       (page_state == P_INFO || page_state == P_READY || page_state == P_REFUSED || page_state == P_DONE));
    enable(btn_card, !card_busy && !flasher_saving());
    lv_label_set_text(lv_obj_get_child(btn_card, 0), page_entry.on_card ? LV_SYMBOL_TRASH " card" : LV_SYMBOL_SD_CARD " Save");
    enable(btn_get, page_state == P_INFO);
    show(btn_install, page_state == P_READY);
    show(page_accept, page_state == P_READY);
    enable(btn_install, page_state == P_READY && ready && lv_obj_has_state(page_accept, LV_STATE_CHECKED));
    show(page_bar, page_state == P_FETCHING || page_state == P_INSTALLING);
}

static void page_status_set(uint32_t color, const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lv_obj_set_style_text_color(page_status, lv_color_hex(color), 0);
    lv_label_set_text(page_status, buf);
}

static void open_page(const fw_entry_t *e)
{
    page_entry = *e;
    page_state = P_INFO;
    lv_label_set_text_fmt(page_title, "%s %s", e->name, e->version);
    char info[600];
    int o = snprintf(info, sizeof info, "%s%s%s\n\n", e->author[0] ? "by " : "", e->author, e->beta ? "  (beta)" : "");
    o += snprintf(info + o, sizeof info - o, "%s\n\n", e->what);
    if (e->product[0]) o += snprintf(info + o, sizeof info - o, "Identity %s", e->product);
    if (e->size) o += snprintf(info + o, sizeof info - o, "   %lu KB", (unsigned long)(e->size / 1024));
    if (e->lic[0]) o += snprintf(info + o, sizeof info - o, "   %s", e->lic);
    lv_label_set_text(page_info, info);
    lv_obj_remove_state(page_accept, LV_STATE_CHECKED);
    page_status_set(C_DIM, "%s", e->src == SRC_SD ? "Read it from the card and check it first." : "Download it and check it first.");
    page_buttons();
    show(page, true);
    lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
}

static void on_back(lv_event_t *ev)
{
    show(page, false);
    if (page_state == P_DONE) flasher_refresh();   /* what the FM-1 runs now */
}

static void on_get(lv_event_t *ev)
{
    page_state = P_FETCHING;
    lv_bar_set_value(page_bar, 0, LV_ANIM_OFF);
    page_status_set(C_DIM, "%s", page_entry.src == SRC_SD ? "Reading..." : "Downloading...");
    page_buttons();
    flasher_prepare(&page_entry);
}

static void on_accept(lv_event_t *ev) { page_buttons(); }

static void on_card(lv_event_t *ev)
{
    if (!catalog_card()) {
        page_status_set(C_BAD, "No SD card: put one in to keep firmwares for offline.");
        return;
    }
    card_busy = true;
    if (page_entry.on_card) {
        flasher_remove(&page_entry);
    } else {
        lv_bar_set_value(page_bar, 0, LV_ANIM_OFF);
        page_status_set(C_DIM, "Saving to the card...");
        flasher_save(&page_entry);
    }
    page_buttons();
}

static void on_install(lv_event_t *ev)
{
    dev_info_t d;
    if (!device_ready(&d)) return;
    page_resume = d.state == DEV_LOADER;
    page_state = P_INSTALLING;
    lv_bar_set_value(page_bar, 0, LV_ANIM_OFF);
    page_status_set(C_WARN, "Starting. Do not unplug the FM-1 or switch it off.");
    page_buttons();
    flasher_install();
}

static void build_page(void)
{
    page = lv_obj_create(lv_layer_top());
    lv_obj_set_size(page, W, H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 8, 0);

    page_title = label(page, &lv_font_montserrat_20, C_TEXT);
    page_info = label(page, &lv_font_montserrat_14, C_DIM);
    lv_obj_set_width(page_info, LV_PCT(100));
    lv_label_set_long_mode(page_info, LV_LABEL_LONG_MODE_WRAP);
    page_status = label(page, &lv_font_montserrat_14, C_DIM);
    lv_obj_set_width(page_status, LV_PCT(100));
    lv_label_set_long_mode(page_status, LV_LABEL_LONG_MODE_WRAP);
    page_bar = lv_bar_create(page);
    lv_obj_set_size(page_bar, LV_PCT(100), 14);
    lv_bar_set_range(page_bar, 0, 100);
    page_accept = lv_checkbox_create(page);
    lv_checkbox_set_text(page_accept, "I accept: this replaces the FM-1's firmware, at my own risk");
    lv_obj_set_style_text_color(page_accept, lv_color_hex(C_TEXT), 0);
    lv_obj_add_event_cb(page_accept, on_accept, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *row = lv_obj_create(page);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    btn_back = button(row, "Back", C_PANEL, on_back);
    lv_obj_set_width(btn_back, 90);
    btn_card = button(row, LV_SYMBOL_SD_CARD " Save", C_PANEL, on_card);
    lv_obj_set_width(btn_card, 100);
    btn_get = button(row, "Get and check", C_ACCENT, on_get);
    lv_obj_set_flex_grow(btn_get, 1);
    btn_install = button(row, "Install on the FM-1", C_BAD, on_install);
    lv_obj_set_flex_grow(btn_install, 1);
    show(page, false);
}

/* ---------------------------------------------------------------------------------- the lists --- */

static void on_entry(lv_event_t *ev)
{
    int i = (int)(intptr_t)lv_event_get_user_data(ev);
    if (i >= 0 && i < catalog_n) open_page(&catalog[i]);
}

static const char *const SRC_HEAD[] = { "Firmware Hub", "Official", "fm1-dock", "Other packages on the card" };

/* the Firmware tab (sd false): fm1-dock, the official one, the Hub, the card's copies marked; the SD
 * card tab: what is on the card, the catalog's copies by source, then the card's other packages */
static void fill(lv_obj_t *list, bool sd)
{
    static const fw_source_t order[] = { SRC_DOCK, SRC_STOCK, SRC_HUB, SRC_SD };
    lv_obj_clean(list);
    for (int k = 0; k < (sd ? 4 : 3); k++) {
        fw_source_t src = order[k];
        bool head = false;
        for (int i = 0; i < catalog_n; i++) {
            const fw_entry_t *e = &catalog[i];
            if (e->src != src || (sd && !e->on_card)) continue;
            if (!head) {
                list_add_text(list, SRC_HEAD[src]);
                head = true;
            }
            char line[96];
            snprintf(line, sizeof line, "%s%s  %s%s", e->on_card && !sd ? LV_SYMBOL_SD_CARD "  " : "", e->name, e->version,
                     e->beta ? "  beta" : "");
            list_add_button(list, line, on_entry, (void *)(intptr_t)i);
        }
    }
}

/* the SD card tab's line */
static void sd_status_line(void)
{
    int saved = 0, other = 0;
    for (int i = 0; i < catalog_n; i++) {
        saved += catalog[i].src != SRC_SD && catalog[i].on_card;
        other += catalog[i].src == SRC_SD;
    }
    if (!catalog_card()) lv_label_set_text(sd_status, "No SD card.");
    else if (!saved && !other)
        lv_label_set_text(sd_status, "Nothing on the card yet: save firmwares from their pages, or all at once on the Firmware tab.");
    else lv_label_set_text_fmt(sd_status, "%d saved from the catalog, %d other packages", saved, other);
}

/* both lists and their lines, after anything that changes what is on the card */
static void refill(void)
{
    fill(fw_list, false);
    fill(sd_list, true);
    sd_status_line();
}

/* a tab: a status line with a button (or two: extra) over a list */
static lv_obj_t *list_tab(lv_obj_t *tab, lv_obj_t **status, const char *refresh, lv_event_cb_t cb, const char *extra,
                          lv_event_cb_t extra_cb, lv_obj_t **extra_btn)
{
    lv_obj_set_style_pad_all(tab, 4, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *top = lv_obj_create(tab);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    *status = label(top, &lv_font_montserrat_14, C_DIM);
    lv_obj_set_flex_grow(*status, 1);
    lv_label_set_long_mode(*status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *b = button(top, refresh, C_PANEL, cb);
    lv_obj_set_height(b, 34);
    if (extra) {
        *extra_btn = button(top, extra, C_PANEL, extra_cb);
        lv_obj_set_height(*extra_btn, 34);
    }
    lv_obj_t *list = list_create(tab);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    return list;
}

static void on_refresh(lv_event_t *ev)
{
    if (flasher_saving()) return;
    lv_label_set_text(fw_status, "Fetching the catalog...");
    lv_label_set_text(sd_status, "Reading the card...");
    flasher_refresh();
}

static void on_save_all(lv_event_t *ev)
{
    if (!catalog_card()) {
        lv_label_set_text(fw_status, "No SD card: put one in to keep the firmwares for offline.");
        return;
    }
    lv_label_set_text(fw_status, flasher_saving() ? "Stopping after this one..." : "Saving to the card...");
    flasher_save_all();
}

/* --------------------------------------------------------------------------------------- WiFi --- */

static void on_wifi_scan(lv_event_t *ev)
{
    lv_label_set_text(wifi_status, "Scanning...");
    flasher_wifi_scan();
}

static void on_network(lv_event_t *ev)
{
    int i = (int)(intptr_t)lv_event_get_user_data(ev);
    snprintf(wifi_ssid, sizeof wifi_ssid, "%s", wifi_found[i]);
    lv_label_set_text_fmt(wifi_title, "Password for %s", wifi_ssid);
    lv_textarea_set_text(wifi_pass, "");
    show(wifi_panel, true);
}

static void on_keyboard(lv_event_t *ev)
{
    lv_event_code_t code = lv_event_get_code(ev);
    if (code == LV_EVENT_READY) {
        flasher_wifi_connect(wifi_ssid, lv_textarea_get_text(wifi_pass));
        lv_label_set_text_fmt(wifi_status, "Connecting to %s...", wifi_ssid);
    }
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) show(wifi_panel, false);
}

static void build_wifi(lv_obj_t *tab)
{
    wifi_list = list_tab(tab, &wifi_status, "Scan", on_wifi_scan, NULL, NULL, NULL);
    lv_label_set_text(wifi_status, "Not connected. Scan, then tap your network.");

    wifi_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wifi_panel, W, H);
    lv_obj_set_pos(wifi_panel, 0, 0);
    lv_obj_set_style_radius(wifi_panel, 0, 0);
    lv_obj_set_style_bg_color(wifi_panel, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(wifi_panel, 6, 0);
    lv_obj_set_scrollable(wifi_panel, false);
    wifi_title = label(wifi_panel, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(wifi_title, LV_ALIGN_TOP_LEFT, 0, 0);
    wifi_pass = lv_textarea_create(wifi_panel);
    lv_textarea_set_one_line(wifi_pass, true);
    lv_textarea_set_password_mode(wifi_pass, true);
    lv_obj_set_width(wifi_pass, LV_PCT(100));
    lv_obj_align(wifi_pass, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_t *kb = lv_keyboard_create(wifi_panel);
    lv_obj_set_size(kb, LV_PCT(100), 220);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, wifi_pass);
    lv_obj_add_event_cb(kb, on_keyboard, LV_EVENT_ALL, NULL);
    show(wifi_panel, false);
}

/* --------------------------------------------------------------------------------------- build --- */

void ui_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_scrollable(scr, false);

    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, W, HEADER);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_pad_hor(hdr, 8, 0);
    lv_obj_set_style_pad_ver(hdr, 0, 0);
    lv_obj_set_scrollable(hdr, false);
    lv_obj_t *title = label(hdr, &lv_font_montserrat_14, C_TEXT);
    lv_label_set_text(title, "FM-1 flasher");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    dev_label = label(hdr, &lv_font_montserrat_14, C_DIM);
    lv_obj_align(dev_label, LV_ALIGN_CENTER, 20, 0);
    lv_label_set_text(dev_label, "No FM-1");
    net_label = label(hdr, &lv_font_montserrat_14, C_DIM);
    lv_obj_align(net_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(net_label, LV_SYMBOL_WIFI);

    lv_obj_t *tv = lv_tabview_create(scr);
    lv_obj_set_size(tv, W, H - HEADER);
    lv_obj_set_pos(tv, 0, HEADER);
    lv_tabview_set_tab_bar_size(tv, 34);
    fw_list = list_tab(lv_tabview_add_tab(tv, "Firmware"), &fw_status, LV_SYMBOL_REFRESH, on_refresh,
                       LV_SYMBOL_DOWNLOAD " " LV_SYMBOL_SD_CARD, on_save_all, &save_btn);
    sd_list = list_tab(lv_tabview_add_tab(tv, "SD card"), &sd_status, LV_SYMBOL_REFRESH, on_refresh, NULL, NULL, NULL);
    build_modules(lv_tabview_add_tab(tv, "Modules"));
    build_wifi(lv_tabview_add_tab(tv, "WiFi"));
    lv_label_set_text(fw_status, "Waiting for WiFi...");
    lv_label_set_text(sd_status, "Reading the card...");
    build_page();
    build_mpage();
}

/* ------------------------------------------------------------------------- from the worker --- */

void ui_device(const dev_info_t *d)
{
    lock();
    switch (d->state) {
    case DEV_NONE:
        lv_label_set_text(dev_label, "No FM-1 on USB");
        lv_obj_set_style_text_color(dev_label, lv_color_hex(C_DIM), 0);
        break;
    case DEV_PROBING:
        lv_label_set_text_fmt(dev_label, "%s ...", d->usb_name[0] ? d->usb_name : "USB device");
        lv_obj_set_style_text_color(dev_label, lv_color_hex(C_DIM), 0);
        break;
    case DEV_FM1: {
        const fw_entry_t *e = catalog_installed(d->id.text);
        if (e) lv_label_set_text_fmt(dev_label, "%s: %s %s", d->id.text, e->name, e->version);
        else lv_label_set_text(dev_label, d->id.text);
        lv_obj_set_style_text_color(dev_label, lv_color_hex(C_OK), 0);
        break;
    }
    case DEV_LOADER:
        lv_label_set_text_fmt(dev_label, "%s: in update mode", d->id.text);
        lv_obj_set_style_text_color(dev_label, lv_color_hex(C_WARN), 0);
        break;
    case DEV_OTHER:
        lv_label_set_text_fmt(dev_label, "%s: no updater answers", d->usb_name[0] ? d->usb_name : "USB MIDI");
        lv_obj_set_style_text_color(dev_label, lv_color_hex(C_BAD), 0);
        break;
    }
    if (page_state == P_READY) page_buttons();
    unlock();
}

void ui_net(net_state_t st, const char *ssid)
{
    static const uint32_t col[] = { C_DIM, C_WARN, C_OK, C_BAD };
    lock();
    lv_obj_set_style_text_color(net_label, lv_color_hex(col[st]), 0);
    switch (st) {
    case NET_OFF: lv_label_set_text(wifi_status, "Not connected. Scan, then tap your network."); break;
    case NET_CONNECTING: lv_label_set_text_fmt(wifi_status, "Connecting to %s...", ssid); break;
    case NET_UP: lv_label_set_text_fmt(wifi_status, "Connected to %s", ssid); break;
    case NET_FAILED:
        lv_label_set_text_fmt(wifi_status, "Could not connect to %s: scan and try again", ssid);
        lv_label_set_text(fw_status, "No WiFi: the WiFi tab.");
        break;
    }
    unlock();
}

/* the Firmware tab's line: how many, how many on the card, and whether the catalog is the card's */
static void fw_status_line(bool ok, bool offline, const char *err)
{
    int hub = 0, on_card = 0;
    for (int i = 0; i < catalog_n; i++) {
        hub += catalog[i].src != SRC_SD;
        on_card += catalog[i].src != SRC_SD && catalog[i].on_card;
    }
    if (!ok) lv_label_set_text_fmt(fw_status, "Catalog: %s%s", err, catalog_card() ? "" : ", and no copy on a card");
    else if (offline) lv_label_set_text_fmt(fw_status, "Offline: the card's catalog (%.10s), %d of %d on the card", catalog_updated, on_card, hub);
    else lv_label_set_text_fmt(fw_status, "%d firmwares, %d on the card. Installing is at your own risk.", hub, on_card);
}

static bool last_ok, last_offline;

void ui_catalog(bool ok, bool offline, const char *err)
{
    lock();
    refill();
    last_ok = ok;
    last_offline = offline;
    fw_status_line(ok, offline, err);
    /* the device line names the firmware once the catalog knows it */
    dev_info_t d;
    flasher_device(&d);
    unlock();
    if (d.state == DEV_FM1) ui_device(&d);
}

void ui_saved(const fw_entry_t *e, bool on_card, const char *why)
{
    lock();
    card_busy = false;
    if (strcmp(e->local, page_entry.local) == 0) {
        bool was = page_entry.on_card;
        page_entry.on_card = on_card;
        if (on_card && !was) page_status_set(C_OK, "Saved on the card: there without WiFi too.");
        else if (!on_card && was) page_status_set(C_DIM, "Removed from the card.");
        else page_status_set(C_BAD, "%s", why[0] ? why : "The card did not take it.");
        page_buttons();
    }
    refill();
    fw_status_line(last_ok, last_offline, "");
    unlock();
}

void ui_save_progress(int done, int total, const char *name)
{
    lock();
    lv_label_set_text(lv_obj_get_child(save_btn, 0), LV_SYMBOL_STOP);
    lv_label_set_text_fmt(fw_status, "Saving to the card: %d of %d  %s", done + 1 > total ? total : done + 1, total, name);
    unlock();
}

void ui_save_done(int saved, int failed, bool stopped)
{
    lock();
    lv_label_set_text(lv_obj_get_child(save_btn, 0), LV_SYMBOL_DOWNLOAD " " LV_SYMBOL_SD_CARD);
    refill();
    if (saved < 0) lv_label_set_text(fw_status, "No SD card.");
    else if (failed || stopped)
        lv_label_set_text_fmt(fw_status, "%d saved to the card%s%s", saved, stopped ? ", stopped" : "",
                              failed ? ", some not (WiFi? the log has which)" : "");
    else fw_status_line(last_ok, last_offline, "");
    unlock();
}

void ui_download(size_t got, size_t total)
{
    static uint32_t last;
    uint32_t now = lv_tick_get();
    if (now - last < 100 && got != total) return;
    last = now;
    lock();
    bool mod = !lv_obj_is_hidden(mpage);   /* a module's download, else a firmware's */
    if (total) lv_bar_set_value(mod ? mpage_bar : page_bar, got * 100 / total, LV_ANIM_OFF);
    lv_label_set_text_fmt(mod ? mpage_status : page_status, "Downloading: %u KB", (unsigned)(got / 1024));
    unlock();
}

void ui_prepared(const fw_package_t *p, bool ok, const char *why)
{
    lock();
    if (!ok) {
        page_state = P_REFUSED;
        page_status_set(C_BAD, "Not installable: %s", why);
    } else {
        dev_info_t d;
        device_ready(&d);
        page_state = P_READY;
        const char *board = p->board == FM1_BOARD_FM1 ? "laid out for the FM-1" : "board layout not recognised";
        char extra[160] = "";
        if (d.state == DEV_LOADER)
            snprintf(extra, sizeof extra, "\nThe FM-1 is in update mode: installing finishes the write with this package.");
        else if (d.state != DEV_FM1)
            snprintf(extra, sizeof extra, "\nPlug the FM-1 into the USB host port to install.");
        page_status_set(C_OK, "Checked%s: %s, %u KB, %s%s, %s.%s", p->from_card ? " (from the card)" : "", p->product,
                        (unsigned)(p->n / 1024), p->official ? "M-VAVE's official V15" : "file CRCs good",
                        p->entry.sha256[0] ? ", SHA-256 as the catalog's" : "", board, extra);
    }
    page_buttons();
    unlock();
}

void ui_install_step(fm1_step_t step, int n, const char *text)
{
    static uint32_t last;
    uint32_t now = lv_tick_get();
    if ((step == FM1_STEP_CHECK || step == FM1_STEP_WRITE) && now - last < 100 && n != 100) return;
    last = now;
    lock();
    switch (step) {
    case FM1_STEP_START: page_status_set(C_WARN, "Starting the update on %s. Do not unplug.", text); break;
    case FM1_STEP_CHECK: page_status_set(C_WARN, "1/3: the FM-1 checks the package (%d reads)", n); break;
    case FM1_STEP_LOADER: page_status_set(C_WARN, "2/3: the FM-1 restarts into update mode..."); break;
    case FM1_STEP_WRITE:
        lv_bar_set_value(page_bar, n, LV_ANIM_OFF);
        page_status_set(C_WARN, "3/3: writing %d %%. Do not unplug.", n);
        break;
    case FM1_STEP_REBOOT: page_status_set(C_WARN, "Written. The FM-1 restarts..."); break;
    case FM1_STEP_DONE: break;
    }
    unlock();
}

void ui_install_done(fm1_err_t e, const char *msg)
{
    lock();
    page_state = P_DONE;
    if (e == FM1_OK) {
        lv_bar_set_value(page_bar, 100, LV_ANIM_OFF);
        page_status_set(C_OK, "Done: the FM-1 runs %s %s.", page_entry.name, page_entry.version);
    } else {
        page_status_set(C_BAD, "Stopped: %s", msg);
    }
    page_buttons();
    unlock();
}

void ui_wifi_list(const wifi_ap_record_t *aps, int n)
{
    lock();
    lv_obj_clean(wifi_list);
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++) {
        char line[64];
        snprintf(wifi_found[i], sizeof wifi_found[i], "%s", (const char *)aps[i].ssid);
        snprintf(line, sizeof line, "%s  %s  %d dBm", aps[i].authmode == WIFI_AUTH_OPEN ? LV_SYMBOL_WIFI : LV_SYMBOL_EYE_CLOSE,
                 wifi_found[i], aps[i].rssi);
        list_add_button(wifi_list, line, on_network, (void *)(intptr_t)i);
    }
    lv_label_set_text_fmt(wifi_status, "%d networks: tap yours", n);
    unlock();
}

/* -------------------------------------------------------------------- fm1-dock's modules --- */
/* The safe mode: a module goes into a running fm1-dock, which checks its header, ABI and CRC before it
 * runs it; the firmware is not touched. The tab: the FM-1's modules (play, delete), then fm1-dock's
 * page's and the card's (deploy, keep on the card). */

static lv_obj_t *mod_list, *mod_status;
static lv_obj_t *mpage_title, *mpage_info, *btn_mback, *btn_mcard, *btn_mgo, *btn_mdel;
static bool mpage_dev;            /* the page is one of the FM-1's modules (else one to deploy) */
static module_entry_t mpage_mod;
static fm1_dock_module_t mpage_on;
static bool mod_busy;
static fm1_dock_module_t on_fm1[FM1_DOCK_MODS_MAX];
static int on_fm1_n;
static bool mods_ok, mods_offline;

static const char *const KIND[] = { "built in", "in the firmware", "deployed", "boot" };

static void mpage_buttons(void)
{
    dev_info_t d;
    flasher_device(&d);
    show(btn_mback, !mod_busy);
    show(btn_mcard, !mpage_dev && mpage_mod.from_page);
    enable(btn_mcard, !mod_busy && catalog_card());
    lv_label_set_text(lv_obj_get_child(btn_mcard, 0), mpage_mod.on_card ? LV_SYMBOL_TRASH " card" : LV_SYMBOL_SD_CARD " Save");
    show(btn_mdel, mpage_dev && mpage_on.kind == 2);   /* only what was deployed */
    enable(btn_mdel, !mod_busy && d.dock);
    lv_label_set_text(lv_obj_get_child(btn_mgo, 0), mpage_dev ? LV_SYMBOL_PLAY " Play" : "Deploy to the FM-1");
    enable(btn_mgo, !mod_busy && d.dock);
}

static void mstatus(uint32_t color, const char *fmt, ...)
{
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lv_obj_set_style_text_color(mpage_status, lv_color_hex(color), 0);
    lv_label_set_text(mpage_status, buf);
}

static void open_mpage(void)
{
    dev_info_t d;
    flasher_device(&d);
    mod_busy = false;
    show(mpage_bar, false);
    if (mpage_dev) {
        lv_label_set_text_fmt(mpage_title, "%s  v%lu", mpage_on.name, (unsigned long)mpage_on.version);
        lv_label_set_text_fmt(mpage_info, "On the FM-1: %s%s%s.", mpage_on.kind < 4 ? KIND[mpage_on.kind] : "?",
                              mpage_on.state ? ", " : "", mpage_on.state ? fm1_dock_state(mpage_on.state) : "");
        mstatus(C_DIM, "%s", d.hello.cur == mpage_on.index ? "Playing now." : "Play it, or delete it (deployed ones only).");
    } else {
        lv_label_set_text_fmt(mpage_title, "%s  %s", mpage_mod.title, mpage_mod.version);
        lv_label_set_text_fmt(mpage_info, "by %s\n\n%s", mpage_mod.by, mpage_mod.source);
        mstatus(C_DIM, "%s", d.dock ? "Safe to deploy: fm1-dock checks the module before it runs it, and the firmware is not touched."
                                    : "The FM-1 must run fm1-dock (the Firmware tab) to take modules.");
    }
    mpage_buttons();
    show(mpage, true);
}

static void on_mback(lv_event_t *ev) { show(mpage, false); }

static void on_mgo(lv_event_t *ev)
{
    mod_busy = true;
    if (mpage_dev) {
        mstatus(C_DIM, "Asking fm1-dock to play %s...", mpage_on.name);
        flasher_dock_select(mpage_on.name);
    } else {
        lv_bar_set_value(mpage_bar, 0, LV_ANIM_OFF);
        show(mpage_bar, true);
        mstatus(C_DIM, "Deploying %s...", mpage_mod.title);
        flasher_mod_deploy(&mpage_mod);
    }
    mpage_buttons();
}

static void on_mdel(lv_event_t *ev)
{
    mod_busy = true;
    mstatus(C_DIM, "Deleting %s from the FM-1...", mpage_on.name);
    flasher_dock_delete(mpage_on.name);
    mpage_buttons();
}

static void on_mcard(lv_event_t *ev)
{
    mod_busy = true;
    if (mpage_mod.on_card) {
        flasher_mod_remove(&mpage_mod);
    } else {
        lv_bar_set_value(mpage_bar, 0, LV_ANIM_OFF);
        show(mpage_bar, true);
        mstatus(C_DIM, "Saving to the card...");
        flasher_mod_save(&mpage_mod);
    }
    mpage_buttons();
}

static void on_dev_module(lv_event_t *ev)
{
    int i = (int)(intptr_t)lv_event_get_user_data(ev);
    if (i < 0 || i >= on_fm1_n) return;
    mpage_dev = true;
    mpage_on = on_fm1[i];
    open_mpage();
}

static void on_module(lv_event_t *ev)
{
    int i = (int)(intptr_t)lv_event_get_user_data(ev);
    if (i < 0 || i >= modules_n) return;
    mpage_dev = false;
    mpage_mod = modules[i];
    open_mpage();
}

static void on_mod_refresh(lv_event_t *ev)
{
    lv_label_set_text(mod_status, "Asking the FM-1...");
    flasher_dock_list();
}

/* the list and its line, from what the worker last saw */
static void mod_fill(void)
{
    dev_info_t d;
    flasher_device(&d);
    on_fm1_n = d.dock ? flasher_dock_modules(on_fm1, FM1_DOCK_MODS_MAX) : 0;
    lv_obj_clean(mod_list);
    if (on_fm1_n) {
        list_add_text(mod_list, "On the FM-1");
        for (int i = 0; i < on_fm1_n; i++) {
            char line[80];
            const fm1_dock_module_t *m = &on_fm1[i];
            snprintf(line, sizeof line, "%s%s  v%lu%s%s", m->index == d.hello.cur ? LV_SYMBOL_PLAY "  " : "", m->name,
                     (unsigned long)m->version, m->state ? "  " : "", m->state ? fm1_dock_state(m->state) : "");
            list_add_button(mod_list, line, on_dev_module, (void *)(intptr_t)i);
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        bool head = false;
        for (int i = 0; i < modules_n; i++) {
            const module_entry_t *m = &modules[i];
            if (m->from_page != (pass == 0)) continue;
            if (!head) {
                list_add_text(mod_list, pass == 0 ? "From fm1-dock's page" : "Other modules on the card");
                head = true;
            }
            char line[80];
            snprintf(line, sizeof line, "%s%s  %s", m->on_card && m->from_page ? LV_SYMBOL_SD_CARD "  " : "", m->title, m->version);
            list_add_button(mod_list, line, on_module, (void *)(intptr_t)i);
        }
    }
    if (d.dock)
        lv_label_set_text_fmt(mod_status, "fm1-dock: %d modules, %lu KB free. Deploying is safe.", on_fm1_n,
                              (unsigned long)(d.hello.free / 1024));
    else if (d.state == DEV_FM1)
        lv_label_set_text(mod_status, "The FM-1 does not run fm1-dock: install it from the Firmware tab to deploy modules.");
    else if (!mods_ok)
        lv_label_set_text(mod_status, "No module list: WiFi, or a copy on the card.");
    else
        lv_label_set_text_fmt(mod_status, "%d modules%s. Plug in an FM-1 with fm1-dock to deploy.", modules_n,
                              mods_offline ? " (the card's list)" : "");
}

static void build_modules(lv_obj_t *tab)
{
    mod_list = list_tab(tab, &mod_status, LV_SYMBOL_REFRESH, on_mod_refresh, NULL, NULL, NULL);
    lv_label_set_text(mod_status, "Reading the module list...");
}

static void build_mpage(void)
{
    mpage = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mpage, W, H);
    lv_obj_set_pos(mpage, 0, 0);
    lv_obj_set_style_radius(mpage, 0, 0);
    lv_obj_set_style_border_width(mpage, 0, 0);
    lv_obj_set_style_bg_color(mpage, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(mpage, 12, 0);
    lv_obj_set_flex_flow(mpage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mpage, 8, 0);
    mpage_title = label(mpage, &lv_font_montserrat_20, C_TEXT);
    mpage_info = label(mpage, &lv_font_montserrat_14, C_DIM);
    lv_obj_set_width(mpage_info, LV_PCT(100));
    lv_label_set_long_mode(mpage_info, LV_LABEL_LONG_MODE_WRAP);
    mpage_status = label(mpage, &lv_font_montserrat_14, C_DIM);
    lv_obj_set_width(mpage_status, LV_PCT(100));
    lv_label_set_long_mode(mpage_status, LV_LABEL_LONG_MODE_WRAP);
    mpage_bar = lv_bar_create(mpage);
    lv_obj_set_size(mpage_bar, LV_PCT(100), 14);
    lv_bar_set_range(mpage_bar, 0, 100);

    lv_obj_t *row = lv_obj_create(mpage);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    btn_mback = button(row, "Back", C_PANEL, on_mback);
    lv_obj_set_width(btn_mback, 90);
    btn_mcard = button(row, LV_SYMBOL_SD_CARD " Save", C_PANEL, on_mcard);
    lv_obj_set_width(btn_mcard, 100);
    btn_mdel = button(row, LV_SYMBOL_TRASH " Delete", C_PANEL, on_mdel);
    lv_obj_set_width(btn_mdel, 110);
    btn_mgo = button(row, "Deploy to the FM-1", C_OK, on_mgo);
    lv_obj_set_flex_grow(btn_mgo, 1);
    show(mpage, false);
}

void ui_modules(bool ok, bool offline, const char *err)
{
    lock();
    mods_ok = ok;
    mods_offline = offline;
    mod_fill();
    unlock();
}

void ui_dock(void)
{
    lock();
    mod_fill();
    if (!lv_obj_is_hidden(mpage) && !mod_busy) mpage_buttons();
    unlock();
}

void ui_mod_progress(int pct)
{
    lock();
    lv_bar_set_value(mpage_bar, pct, LV_ANIM_OFF);
    mstatus(C_DIM, "Deploying: %d %%", pct);
    unlock();
}

void ui_mod_done(bool ok, const char *msg)
{
    lock();
    mod_busy = false;
    mstatus(ok ? C_OK : C_BAD, "%s", msg);
    if (ok && mpage_dev) mpage_dev = false, show(mpage, false);   /* a play or a delete: back to the list */
    mpage_buttons();
    unlock();
}

void ui_mod_saved(const module_entry_t *m, bool on_card, const char *why)
{
    lock();
    mod_busy = false;
    if (strcmp(m->local, mpage_mod.local) == 0) {
        bool was = mpage_mod.on_card;
        mpage_mod.on_card = on_card;
        show(mpage_bar, false);
        if (on_card && !was) mstatus(C_OK, "Saved on the card.");
        else if (!on_card && was) mstatus(C_DIM, "Removed from the card.");
        else mstatus(C_BAD, "%s", why[0] ? why : "The card did not take it.");
    }
    mod_fill();
    mpage_buttons();
    unlock();
}
