/* A WiFi scanner for the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5: the P4 scans through its ESP32-C6
 * (ESP-Hosted, esp_wifi_remote) and shows it on the ST7796 in two screens, swiped between:
 *   Networks  the strongest RSSI on each 2.4 GHz channel as bars, and every network heard
 *   Spectrum  the band 2400-2484 MHz, 1 MHz per point, rebuilt from the scan: each network drawn
 *             over the band it occupies (its primary channel, 20 or 40 MHz, the OFDM transmit
 *             mask's roll-off), summed in mW; below it a waterfall of that spectrum over time.
 *   Bluetooth the BLE devices advertising around: name (when the advertisement has one), address,
 *             RSSI, seconds since last heard; NimBLE on the P4, the C6's controller over ESP-Hosted's
 *             virtual HCI, a passive scan that never stops.
 * The spectrum is not measured energy: it shows the networks the C6 decoded, at their beacon RSSI.
 * Only the page on screen scans: the WiFi pages run WiFi scans back to back, the Bluetooth page a
 * BLE scan that listens all the time, so the C6 never splits its radio between the two. Rescan on
 * the first page starts a WiFi scan at once. Each scan is printed on the console. Public domain (Unlicense) or CC0, as the ESP-IDF examples it follows. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_hosted.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#define MAX_APS    32
#define CHANNELS   13
#define RSSI_MIN   (-100)
#define RSSI_MAX   (-20)
#define F_LO       2400                 /* MHz */
#define BINS       85                   /* 2400 ... 2484 */
#define WF_W       300
#define WF_H       180
#define WF_ROW     3                    /* pixels per scan in the waterfall */

static const char *TAG = "wifi_scan";
static SemaphoreHandle_t rescan;
static lv_obj_t *title, *chart, *table, *spec, *wf;
static lv_chart_series_t *bars, *sum_series;
static uint8_t wf_buf[WF_W * WF_H * 2];

/* BLE devices heard: updated from NimBLE's host task, read by the LVGL timer. */
#define MAX_BLE   48
#define BLE_AGE_S 30
typedef struct { ble_addr_t addr; char name[24]; uint16_t company; bool has_company; int8_t rssi; TickType_t seen; } ble_dev_t;

/* A few Bluetooth SIG company identifiers (Assigned Numbers), for devices that send no name. */
static const char *company_name(uint16_t id)
{
    switch (id) {
    case 0x004c: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00e0: return "Google";
    case 0x0087: return "Garmin";
    case 0x0157: return "Huawei";
    case 0x038f: return "Xiaomi";
    case 0x0499: return "Ruuvi";
    case 0x02e5: return "Espressif";
    case 0x0059: return "Nordic";
    case 0x000d: return "TI";
    case 0x0171: return "Amazon";
    case 0x0310: return "SGL Italia";
    case 0x0131: return "Cypress";
    default: return NULL;
    }
}
static ble_dev_t ble_devs[MAX_BLE];
static int ble_n;
static SemaphoreHandle_t ble_lock;
static volatile int active_tab;                 /* 0 Networks, 1 Spectrum, 2 Bluetooth */
static volatile bool ble_synced, ble_on;
#define TAB_BLE 2
static lv_obj_t *ble_title, *ble_table;

static int ble_gap_event(struct ble_gap_event *ev, void *arg)
{
    if (ev->type != BLE_GAP_EVENT_DISC) return 0;
    xSemaphoreTake(ble_lock, portMAX_DELAY);
    int i = 0;
    while (i < ble_n && ble_addr_cmp(&ble_devs[i].addr, &ev->disc.addr) != 0) i++;
    if (i == ble_n && ble_n < MAX_BLE) { memset(&ble_devs[i], 0, sizeof ble_devs[i]); ble_devs[i].addr = ev->disc.addr; ble_n++; }
    if (i < ble_n) {
        ble_devs[i].rssi = ev->disc.rssi;
        ble_devs[i].seen = xTaskGetTickCount();
        /* the advertisement and the scan response each carry part of it: keep what either gave */
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) == 0) {
            if (f.name_len > 0) {
                int n = f.name_len < sizeof ble_devs[i].name - 1 ? f.name_len : sizeof ble_devs[i].name - 1;
                memcpy(ble_devs[i].name, f.name, n);
                ble_devs[i].name[n] = 0;
            }
            if (f.mfg_data_len >= 2) { ble_devs[i].company = f.mfg_data[0] | f.mfg_data[1] << 8; ble_devs[i].has_company = true; }
        }
    }
    xSemaphoreGive(ble_lock);
    return 0;
}

/* Start or stop the BLE scan: on only while the Bluetooth page is shown. Active (each advertiser
 * is asked for its scan response, where many keep their name), and with the window as long as the
 * interval (units of 0.625 ms) the controller listens all the time. */
static void ble_scan(bool on)
{
    if (!ble_synced || on == ble_on) return;
    if (on) {
        uint8_t own;
        if (ble_hs_id_infer_auto(0, &own) != 0) return;
        struct ble_gap_disc_params p = { .itvl = 0x60, .window = 0x60, .passive = 0, .filter_duplicates = 0 };
        int rc = ble_gap_disc(own, BLE_HS_FOREVER, &p, ble_gap_event, NULL);
        printf("ble: scan on (%d)\n", rc);
        ble_on = rc == 0;
    } else {
        ble_gap_disc_cancel();
        printf("ble: scan off\n");
        ble_on = false;
    }
}

static void ble_on_sync(void) { ble_synced = true; }

static void ble_host_task(void *param) { nimble_port_run(); nimble_port_freertos_deinit(); }

static int by_rssi(const void *a, const void *b) { return ((const ble_dev_t *)b)->rssi - ((const ble_dev_t *)a)->rssi; }

/* Every second, in the LVGL task: drop devices not heard for BLE_AGE_S, show the rest by RSSI. */
static void ble_refresh(lv_timer_t *t)
{
    static ble_dev_t snap[MAX_BLE];
    static int ticks;
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(ble_lock, portMAX_DELAY);
    int k = 0;
    for (int i = 0; i < ble_n; i++) if ((now - ble_devs[i].seen) < pdMS_TO_TICKS(BLE_AGE_S * 1000)) ble_devs[k++] = ble_devs[i];
    ble_n = k;
    memcpy(snap, ble_devs, sizeof(ble_dev_t) * k);
    xSemaphoreGive(ble_lock);
    qsort(snap, k, sizeof snap[0], by_rssi);
    lv_label_set_text_fmt(ble_title, "Bluetooth LE: %d devices", k);
    lv_table_set_row_count(ble_table, k + 1);
    for (int i = 0; i < k; i++) {
        const uint8_t *a = snap[i].addr.val;
        const char *who = snap[i].has_company ? company_name(snap[i].company) : NULL;
        if (snap[i].name[0]) lv_table_set_cell_value(ble_table, i + 1, 0, snap[i].name);
        else if (who) lv_table_set_cell_value_fmt(ble_table, i + 1, 0, "(%s)", who);
        else if (snap[i].has_company) lv_table_set_cell_value_fmt(ble_table, i + 1, 0, "(0x%04x)", snap[i].company);
        else lv_table_set_cell_value(ble_table, i + 1, 0, "-");
        lv_table_set_cell_value_fmt(ble_table, i + 1, 1, "%02x:%02x:%02x", a[2], a[1], a[0]);
        lv_table_set_cell_value_fmt(ble_table, i + 1, 2, "%d", snap[i].rssi);
        lv_table_set_cell_value_fmt(ble_table, i + 1, 3, "%lu", (unsigned long)((now - snap[i].seen) * portTICK_PERIOD_MS / 1000));
    }
    if (++ticks % 5 == 0) {
        int named = 0, maker = 0;
        for (int i = 0; i < k; i++) { if (snap[i].name[0]) named++; else if (snap[i].has_company) maker++; }
        printf("ble: %d devices, %d named, %d with a manufacturer only\n", k, named, maker);
    }
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "?";
    }
}

/* The level, in dB below the network's own, at |offset| MHz from its centre: flat to half - 1,
 * -20 at half + 1, -28 at the full width, -40 at 1.5 widths (the OFDM transmit spectrum mask;
 * for 20 MHz: 9, 11, 20 and 30 MHz). */
static float mask_db(float offset, float width)
{
    float half = width / 2, d = fabsf(offset);
    if (d <= half - 1) return 0;
    if (d <= half + 1) return -20 * (d - (half - 1)) / 2;
    if (d <= width) return -20 - 8 * (d - (half + 1)) / (width - half - 1);
    if (d <= 1.5f * width) return -28 - 12 * (d - width) / (0.5f * width);
    return -200;
}

/* The band, 1 MHz per bin, from the scan: every network over the band it occupies, summed in mW. */
static void spectrum(const wifi_ap_record_t *aps, uint16_t n, int out[BINS])
{
    for (int b = 0; b < BINS; b++) {
        float mw = 0;
        for (int i = 0; i < n; i++) {
            int ch = aps[i].primary;
            if (ch < 1 || ch > 14) continue;
            float centre = ch == 14 ? 2484 : 2407 + 5 * ch, width = 20;
            if (aps[i].second == WIFI_SECOND_CHAN_ABOVE) { centre += 10; width = 40; }
            else if (aps[i].second == WIFI_SECOND_CHAN_BELOW) { centre -= 10; width = 40; }
            float db = aps[i].rssi + mask_db(F_LO + b - centre, width);
            if (db > -200) mw += powf(10, db / 10);
        }
        int dbm = mw > 0 ? (int)lroundf(10 * log10f(mw)) : RSSI_MIN;
        out[b] = dbm < RSSI_MIN ? RSSI_MIN : dbm;
    }
}

/* dBm to a colour: dark blue at the floor through green and yellow to red at -30 and above. */
static lv_color_t heat(int dbm)
{
    float t = (float)(dbm - RSSI_MIN) / (-30 - RSSI_MIN);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    uint8_t r = t < 0.5f ? 0 : (uint8_t)(255 * (t - 0.5f) * 2);
    uint8_t g = t < 0.5f ? (uint8_t)(255 * t * 2) : (uint8_t)(255 * (1 - t) * 2);
    uint8_t b = t < 0.5f ? (uint8_t)(160 * (1 - t * 2)) + 20 : 0;
    return lv_color_make(r, g, b);
}

static void rescan_clicked(lv_event_t *e) { xSemaphoreGive(rescan); }

static void tab_changed(lv_event_t *e)
{
    active_tab = lv_tabview_get_tab_active(lv_event_get_target(e));
    xSemaphoreGive(rescan);                     /* the scan loop follows the page at once */
}

static void label_at(lv_obj_t *parent, lv_obj_t *below, const char *text, int x)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xa0a8b0), 0);
    lv_obj_align_to(l, below, LV_ALIGN_OUT_BOTTOM_LEFT, x, 2);
}

static void build_networks(lv_obj_t *tab)
{
    title = lv_label_create(tab);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "WiFi scan: starting");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    chart = lv_chart_create(tab);
    lv_obj_set_size(chart, 290, 130);
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 30);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, CHANNELS);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, RSSI_MIN, RSSI_MAX);
    lv_chart_set_div_line_count(chart, 5, 0);
    bars = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < CHANNELS; i++) lv_chart_set_value_by_id(chart, bars, i, RSSI_MIN);
    for (int i = 0; i < CHANNELS; i++) { char s[4]; snprintf(s, sizeof s, "%d", i + 1); label_at(tab, chart, s, 8 + i * 270 / CHANNELS); }

    table = lv_table_create(tab);
    lv_obj_set_size(table, 296, 170);
    lv_obj_align(table, LV_ALIGN_TOP_MID, 0, 182);
    lv_table_set_column_count(table, 4);
    lv_table_set_column_width(table, 0, 130);
    lv_table_set_column_width(table, 1, 40);
    lv_table_set_column_width(table, 2, 56);
    lv_table_set_column_width(table, 3, 66);
    lv_table_set_cell_value(table, 0, 0, "SSID");
    lv_table_set_cell_value(table, 0, 1, "Ch");
    lv_table_set_cell_value(table, 0, 2, "dBm");
    lv_table_set_cell_value(table, 0, 3, "Auth");

    lv_obj_t *btn = lv_button_create(tab);
    lv_obj_set_size(btn, 140, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(btn, rescan_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Rescan");
    lv_obj_center(bl);
}

static void build_spectrum(lv_obj_t *tab)
{
    lv_obj_t *t = lv_label_create(tab);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_label_set_text(t, "2.4 GHz from the scan");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    spec = lv_chart_create(tab);
    lv_obj_set_size(spec, WF_W, 110);
    lv_obj_align(spec, LV_ALIGN_TOP_MID, 0, 28);
    lv_chart_set_type(spec, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(spec, BINS);
    lv_chart_set_axis_range(spec, LV_CHART_AXIS_PRIMARY_Y, RSSI_MIN, RSSI_MAX);
    lv_chart_set_div_line_count(spec, 4, 0);
    lv_obj_set_style_size(spec, 0, 0, LV_PART_INDICATOR);       /* a curve, no point markers */
    sum_series = lv_chart_add_series(spec, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    for (int b = 0; b < BINS; b++) lv_chart_set_value_by_id(spec, sum_series, b, RSSI_MIN);
    /* channels 1, 6 and 11 (2412, 2437, 2462 MHz) under the curve */
    label_at(tab, spec, "1", (2412 - F_LO) * WF_W / BINS - 3);
    label_at(tab, spec, "6", (2437 - F_LO) * WF_W / BINS - 3);
    label_at(tab, spec, "11", (2462 - F_LO) * WF_W / BINS - 6);
    label_at(tab, spec, "MHz 2400", 0);

    wf = lv_canvas_create(tab);
    lv_canvas_set_buffer(wf, wf_buf, WF_W, WF_H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(wf, heat(RSSI_MIN), LV_OPA_COVER);
    lv_obj_align(wf, LV_ALIGN_TOP_MID, 0, 160);
}

static void build_ble(lv_obj_t *tab)
{
    ble_title = lv_label_create(tab);
    lv_obj_set_style_text_font(ble_title, &lv_font_montserrat_20, 0);
    lv_label_set_text(ble_title, "Bluetooth LE: starting");
    lv_obj_align(ble_title, LV_ALIGN_TOP_MID, 0, 0);
    ble_table = lv_table_create(tab);
    lv_obj_set_size(ble_table, 296, 380);
    lv_obj_align(ble_table, LV_ALIGN_TOP_MID, 0, 30);
    lv_table_set_column_count(ble_table, 4);
    lv_table_set_column_width(ble_table, 0, 116);
    lv_table_set_column_width(ble_table, 1, 90);
    lv_table_set_column_width(ble_table, 2, 50);
    lv_table_set_column_width(ble_table, 3, 36);
    lv_table_set_cell_value(ble_table, 0, 0, "Name");
    lv_table_set_cell_value(ble_table, 0, 1, "Addr");
    lv_table_set_cell_value(ble_table, 0, 2, "dBm");
    lv_table_set_cell_value(ble_table, 0, 3, "s");
}

/* The newest scan as the top rows of the waterfall, the older ones moved down. */
static void waterfall_push(const int bins[BINS])
{
    memmove(wf_buf + WF_W * 2 * WF_ROW, wf_buf, WF_W * 2 * (WF_H - WF_ROW));
    for (int x = 0; x < WF_W; x++) {
        lv_color_t c = heat(bins[x * BINS / WF_W]);
        for (int y = 0; y < WF_ROW; y++) lv_canvas_set_px(wf, x, y, c, LV_OPA_COVER);
    }
    lv_obj_invalidate(wf);
}

static void show(int scan, const wifi_ap_record_t *aps, uint16_t n, const int bins[BINS])
{
    int best[CHANNELS];
    for (int i = 0; i < CHANNELS; i++) best[i] = RSSI_MIN;
    for (int i = 0; i < n; i++) {
        int ch = aps[i].primary;
        if (ch >= 1 && ch <= CHANNELS && aps[i].rssi > best[ch - 1]) best[ch - 1] = aps[i].rssi;
    }
    if (!bsp_display_lock(1000)) return;
    lv_label_set_text_fmt(title, "WiFi scan %d: %u networks", scan, n);
    for (int i = 0; i < CHANNELS; i++) lv_chart_set_value_by_id(chart, bars, i, best[i]);
    lv_chart_refresh(chart);
    lv_table_set_row_count(table, n + 1);
    for (int i = 0; i < n; i++) {
        lv_table_set_cell_value(table, i + 1, 0, aps[i].ssid[0] ? (const char *)aps[i].ssid : "(hidden)");
        lv_table_set_cell_value_fmt(table, i + 1, 1, "%d", aps[i].primary);
        lv_table_set_cell_value_fmt(table, i + 1, 2, "%d", aps[i].rssi);
        lv_table_set_cell_value(table, i + 1, 3, auth_name(aps[i].authmode));
    }
    for (int b = 0; b < BINS; b++) lv_chart_set_value_by_id(spec, sum_series, b, bins[b]);
    lv_chart_refresh(spec);
    waterfall_push(bins);
    bsp_display_unlock();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    rescan = xSemaphoreCreateBinary();

    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_lock(-1);
    lv_obj_t *tv = lv_tabview_create(lv_screen_active());
    lv_tabview_set_tab_bar_size(tv, 34);
    build_networks(lv_tabview_add_tab(tv, "Networks"));
    build_spectrum(lv_tabview_add_tab(tv, "Spectrum"));
    build_ble(lv_tabview_add_tab(tv, "Bluetooth"));
    lv_obj_add_event_cb(tv, tab_changed, LV_EVENT_VALUE_CHANGED, NULL);
    ble_lock = xSemaphoreCreateMutex();
    lv_timer_create(ble_refresh, 1000, NULL);
    bsp_display_unlock();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* BLE: the C6's controller over ESP-Hosted's virtual HCI, NimBLE here */
    /* Older slave firmware has its controller running from boot and does not answer the request
     * to start it; NimBLE is started either way. */
    if (esp_hosted_bt_controller_init() != ESP_OK || esp_hosted_bt_controller_enable() != ESP_OK)
        ESP_LOGW(TAG, "the co-processor did not confirm its BT controller; trying HCI anyway");
    if (nimble_port_init() == ESP_OK) {
        ble_hs_cfg.sync_cb = ble_on_sync;
        nimble_port_freertos_init(ble_host_task);
    }

    static wifi_ap_record_t aps[MAX_APS];
    static int bins[BINS];
    wifi_scan_config_t sc = { .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = { .min = 10, .max = 25 } };
    for (int scan = 1;; scan++) {
        if (active_tab == TAB_BLE) {             /* the Bluetooth page: BLE only, WiFi idle */
            ble_scan(true);
            scan--;
            xSemaphoreTake(rescan, pdMS_TO_TICKS(1000));
            continue;
        }
        ble_scan(false);
        uint16_t n = MAX_APS;
        int64_t t0 = esp_timer_get_time();
        err = esp_wifi_scan_start(&sc, true);
        int64_t t1 = esp_timer_get_time();
        if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&n, aps);
        int64_t t2 = esp_timer_get_time();
        if (err != ESP_OK) { ESP_LOGE(TAG, "scan %d: %s", scan, esp_err_to_name(err)); n = 0; }
        spectrum(aps, n, bins);
        printf("scan %d: %u networks (scan %lld ms, records %lld ms)\n", scan, n, (t1 - t0) / 1000, (t2 - t1) / 1000);
        for (int i = 0; i < n; i++)
            printf("  %-32s ch %2d%s %4d dBm %s\n", aps[i].ssid[0] ? (const char *)aps[i].ssid : "(hidden)", aps[i].primary,
                   aps[i].second == WIFI_SECOND_CHAN_ABOVE ? "+" : aps[i].second == WIFI_SECOND_CHAN_BELOW ? "-" : " ", aps[i].rssi, auth_name(aps[i].authmode));
        printf("spectrum");
        for (int b = 0; b < BINS; b++) printf(",%d", bins[b]);
        printf("\n");
        show(scan, aps, n, bins);
        xSemaphoreTake(rescan, pdMS_TO_TICKS(100));
    }
}
