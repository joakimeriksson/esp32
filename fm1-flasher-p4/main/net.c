/* SPDX-License-Identifier: GPL-3.0-only */
#include "net.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"

static const char *TAG = "net";
#define BIT_UP BIT0
#define BIT_FAIL BIT1
#define RETRIES 5

static EventGroupHandle_t ev;
static volatile net_state_t state;
static int retries;
static char ssid_saved[33];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(ev, BIT_UP);
        if (state == NET_OFF) return;
        if (++retries <= RETRIES) {
            state = NET_CONNECTING;
            esp_wifi_connect();
        } else {
            state = NET_FAILED;
            xEventGroupSetBits(ev, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        retries = 0;
        state = NET_UP;
        xEventGroupSetBits(ev, BIT_UP);
        ESP_LOGI(TAG, "up: " IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
    }
}

static void connect_with(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_disconnect();
    xEventGroupClearBits(ev, BIT_UP | BIT_FAIL);
    retries = 0;
    state = NET_CONNECTING;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

void net_start(void)
{
    ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* ours is in the "net" namespace */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    nvs_handle_t h;
    char pass[65] = "";
    size_t l1 = sizeof ssid_saved, l2 = sizeof pass;
    if (nvs_open("net", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ssid", ssid_saved, &l1) != ESP_OK) ssid_saved[0] = 0;
        if (nvs_get_str(h, "pass", pass, &l2) != ESP_OK) pass[0] = 0;
        nvs_close(h);
    }
    if (ssid_saved[0]) connect_with(ssid_saved, pass);
}

net_state_t net_state(void) { return state; }
void net_ssid(char *out, size_t len) { snprintf(out, len, "%s", ssid_saved); }

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

int net_scan(wifi_ap_record_t *aps, int max)
{
    wifi_scan_config_t sc = { .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = { .min = 30, .max = 80 } };
    uint16_t n = max;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) return 0;
    qsort(aps, n, sizeof *aps, by_rssi);
    /* one line per name */
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!aps[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = strcmp((char *)aps[j].ssid, (char *)aps[i].ssid) == 0;
        if (!dup) aps[k++] = aps[i];
    }
    return k;
}

bool net_wait_up(uint32_t ms)
{
    if (state == NET_UP) return true;
    if (state == NET_OFF || state == NET_FAILED) return false;
    EventBits_t b = xEventGroupWaitBits(ev, BIT_UP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(ms));
    return b & BIT_UP;
}

bool net_connect(const char *ssid, const char *pass, uint32_t ms)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    snprintf(ssid_saved, sizeof ssid_saved, "%s", ssid);
    connect_with(ssid, pass);
    return net_wait_up(ms);
}

/* ------------------------------------------------------------------------------------ HTTPS --- */

typedef struct {
    uint8_t *buf;
    size_t n, cap, max, total;
    bool too_big;
    void (*progress)(size_t, size_t);
} sink_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    sink_t *s = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Length") == 0) {
        s->total = strtoul(e->header_value, NULL, 10);
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        int status = esp_http_client_get_status_code(e->client);
        if (status >= 300 && status < 400) return ESP_OK;   /* a redirect's body */
        if (s->n + e->data_len > s->max) {
            s->too_big = true;
            return ESP_FAIL;
        }
        if (s->n + e->data_len > s->cap) {
            size_t cap = s->cap ? s->cap : 64 * 1024;
            while (cap < s->n + e->data_len) cap *= 2;
            if (s->total > cap && s->total <= s->max) cap = s->total;
            uint8_t *b = heap_caps_realloc(s->buf, cap, MALLOC_CAP_SPIRAM);
            if (!b) {
                s->too_big = true;
                return ESP_FAIL;
            }
            s->buf = b;
            s->cap = cap;
        }
        memcpy(s->buf + s->n, e->data, e->data_len);
        s->n += e->data_len;
        if (s->progress) s->progress(s->n, s->total);
    } else if (e->event_id == HTTP_EVENT_REDIRECT) {
        s->n = 0;
        s->total = 0;
    }
    return ESP_OK;
}

int net_get(const char *url, uint8_t **out, size_t max, void (*progress)(size_t, size_t), char *err, size_t errlen)
{
    sink_t s = { .max = max, .progress = progress };
    *out = NULL;
    if (!net_wait_up(15000)) {
        snprintf(err, errlen, "no WiFi");
        return -1;
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .user_data = &s,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,   /* GitHub's redirect to its asset store is a long URL */
        .max_redirection_count = 5,
        .user_agent = "fm1-flasher-p4",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t r = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (r != ESP_OK || status != 200) {
        if (s.too_big) snprintf(err, errlen, "too big");
        else if (r != ESP_OK) snprintf(err, errlen, "%s", esp_err_to_name(r));
        else snprintf(err, errlen, "HTTP %d", status);
        free(s.buf);
        return -1;
    }
    *out = s.buf;
    return s.n;
}
