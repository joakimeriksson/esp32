/* SPDX-License-Identifier: GPL-3.0-only */
/* BLE MIDI (the MIDI over Bluetooth LE specification): one service, one characteristic that the host
 * writes packets to (and may subscribe to; nothing is sent back). Laid out as atech-firmware's
 * sid-theremin build does it: the 128-bit service UUID fills the advertisement, so the name goes in the
 * scan response; advertising starts again after a disconnect. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_hosted.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_midi.h"
#include "ble_midi_parse.h"
#include "events.h"

/* esp_hosted 2.12.13 (pinned in idf_component.yml) host/drivers/transport/transport_drv.c: whether
 * the co-processor has come up. Its header is private to the component. */
extern uint8_t is_transport_rx_ready(void);

/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 and 7772E5DB-3868-4112-A1A9-F2669D106BF3, little-endian */
static const ble_uuid128_t MIDI_SVC = BLE_UUID128_INIT(0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03);
static const ble_uuid128_t MIDI_CHR = BLE_UUID128_INIT(0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77);
static const char *s_name;
static volatile bool s_connected;
static uint8_t s_own;

static void emit(void *ctx, uint8_t status, uint8_t d1, uint8_t d2)
{
    switch (status & 0xf0) {
    case 0x90: if (d2) { events_post(EV_NOTE_ON, d1, (int32_t)d2 * 65536 / 127); break; }   /* velocity 0: off */
               /* fall through */
    case 0x80: events_post(EV_NOTE_OFF, d1, 0); break;
    case 0xe0: events_post(EV_BEND, 0, ((int32_t)(d1 | d2 << 7) - 8192) * 2 * 65536 / 8192); break;   /* +-2 semitones */
    case 0xc0: events_post(EV_PRESET, 0, d1); break;
    case 0xb0: if (d1 == 120 || d1 == 123) events_post(EV_ALL_OFF, 0, 0); break;
    }
}

static int midi_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;                 /* a read returns no payload */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[256];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    bleMidiParse(buf, n, emit, NULL);
    return 0;
}

static const struct ble_gatt_svc_def svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &MIDI_SVC.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &MIDI_CHR.u, .access_cb = midi_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY },
          { 0 } } },
    { 0 }
};

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields adv = { 0 }, rsp = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &MIDI_SVC; adv.num_uuids128 = 1; adv.uuids128_is_complete = 1;
    rsp.name = (const uint8_t *)s_name; rsp.name_len = strlen(s_name); rsp.name_is_complete = 1;
    static const uint8_t itvl[4] = { 0x06, 0x00, 0x0c, 0x00 };             /* 7.5 to 15 ms, what Apple's hosts give MIDI */
    rsp.slave_itvl_range = itvl;
    ble_gap_adv_set_fields(&adv);
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    int rc = ble_gap_adv_start(s_own, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    printf("ble-midi: advertising as \"%s\" (%d)\n", s_name, rc);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_connected = ev->connect.status == 0;
        printf("ble-midi: %s\n", s_connected ? "connected" : "connect failed");
        if (!s_connected) advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        events_post(EV_ALL_OFF, 0, 0);                                     /* nothing left hanging */
        printf("ble-midi: disconnected\n");
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own);
    advertise();
}

static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

void ble_midi_start(const char *name)
{
    s_name = name;
    /* No co-processor on the SDIO bus (a board whose C6 does not answer, or an emulator without
     * one): ESP-Hosted's NimBLE transport would abort the app, so BLE MIDI stays off instead.
     * esp_hosted_connect_to_slave returns ESP_OK either way; the transport state says whether the
     * C6 sent its INIT event, which can come just after it returns. */
    esp_hosted_connect_to_slave();
    for (int i = 0; i < 30 && !is_transport_rx_ready(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!is_transport_rx_ready()) { printf("ble-midi: no co-processor answers, BLE MIDI off\n"); return; }
    /* the board's factory C6 firmware does not answer this, but its controller runs from boot */
    if (esp_hosted_bt_controller_init() != ESP_OK || esp_hosted_bt_controller_enable() != ESP_OK)
        printf("ble-midi: the co-processor did not confirm its BT controller; trying HCI anyway\n");
    if (nimble_port_init() != ESP_OK) { printf("ble-midi: NimBLE did not start\n"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(name);
    ble_gatts_count_cfg(svcs);
    ble_gatts_add_svcs(svcs);
    nimble_port_freertos_init(host_task);
}

bool ble_midi_connected(void) { return s_connected; }
