/* SPDX-License-Identifier: GPL-3.0-only */
/* WiFi through the board's ESP32-C6 (ESP-Hosted, esp_wifi_remote), the network saved in NVS, and
 * HTTPS downloads into PSRAM. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_wifi.h"

typedef enum { NET_OFF, NET_CONNECTING, NET_UP, NET_FAILED } net_state_t;

void net_start(void);                 /* connects to the saved network, if there is one */
net_state_t net_state(void);
void net_ssid(char *out, size_t len); /* the saved network, "" when none */
/* the networks around, strongest first; their number */
int net_scan(wifi_ap_record_t *aps, int max);
/* saves the network and connects; true once it has an address (within ms) */
bool net_connect(const char *ssid, const char *pass, uint32_t ms);
bool net_wait_up(uint32_t ms);

/* GET url into a new PSRAM buffer (*out, free() it): its length, or -1 (err says why). Redirects are
 * followed (GitHub's release assets); max: the most it takes. progress(got, total) may be NULL. */
int net_get(const char *url, uint8_t **out, size_t max, void (*progress)(size_t got, size_t total),
            char *err, size_t errlen);
