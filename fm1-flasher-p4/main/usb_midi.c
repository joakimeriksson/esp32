/* SPDX-License-Identifier: GPL-3.0-only */
/* USB-MIDI host: the USB Host Library through the BSP (bsp_usb_host_start, the OTG high-speed port),
 * one client task that opens a device with a MIDI streaming interface (class 1, subclass 3), keeps
 * a bulk IN transfer going and reassembles SysEx into a queue; sends are one bulk OUT transfer. */
#include "usb_midi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "usb_midi";

#define OUT_BUF (USB_MIDI_SYSEX_MAX / 3 * 4 + 8)
#define IN_BUF 512
#define QUEUE_LEN 8

typedef struct {
    uint16_t len;
    uint8_t data[USB_MIDI_SYSEX_MAX];
} msg_t;

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    uint8_t new_addr;          /* a device to look at, from the event callback */
    bool gone;                 /* the open device was unplugged */
    uint8_t intf, ep_in, ep_out;
    uint16_t mps_in;
    usb_transfer_t *in, *out;
    volatile bool in_busy, out_busy;
    volatile bool up;
    volatile uint32_t gen;
    char name[32];
    QueueHandle_t q;
    SemaphoreHandle_t out_done, send_lock;
    /* the SysEx being reassembled (client task only) */
    uint8_t rx[USB_MIDI_SYSEX_MAX];
    size_t rx_n;
    bool rx_on;
    usb_transfer_status_t out_status;
} s;

static void rx_byte(uint8_t b)
{
    if (b == 0xF0) {
        s.rx_on = true;
        s.rx_n = 0;
    }
    if (!s.rx_on) return;
    if (s.rx_n < sizeof s.rx) s.rx[s.rx_n++] = b;
    else s.rx_on = false;   /* too long for us: dropped */
    if (b == 0xF7 && s.rx_on) {
        static msg_t m;
        m.len = s.rx_n;
        memcpy(m.data, s.rx, s.rx_n);
        if (xQueueSend(s.q, &m, 0) != pdTRUE) ESP_LOGW(TAG, "SysEx queue full, one dropped");
        s.rx_on = false;
    }
}

/* USB-MIDI 1.0 event packets: CIN 4 SysEx start / continue (3 bytes), 5 / 6 / 7 the end with 1 / 2 / 3 */
static void rx_packets(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint8_t cin = p[i] & 0x0F;
        int k = cin == 0x4 || cin == 0x7 ? 3 : cin == 0x6 ? 2 : cin == 0x5 ? 1 : 0;
        for (int j = 0; j < k; j++) rx_byte(p[i + 1 + j]);
    }
}

static void in_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) rx_packets(t->data_buffer, t->actual_num_bytes);
    if (!s.gone && s.dev && (t->status == USB_TRANSFER_STATUS_COMPLETED || t->status == USB_TRANSFER_STATUS_TIMED_OUT) &&
        usb_host_transfer_submit(t) == ESP_OK)
        return;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED && t->status != USB_TRANSFER_STATUS_NO_DEVICE)
        ESP_LOGW(TAG, "IN stopped: status %d", t->status);
    s.in_busy = false;
}

static void out_cb(usb_transfer_t *t)
{
    s.out_status = t->status;
    s.out_busy = false;
    xSemaphoreGive(s.out_done);
}

static void client_cb(const usb_host_client_event_msg_t *ev, void *arg)
{
    if (ev->event == USB_HOST_CLIENT_EVENT_NEW_DEV) s.new_addr = ev->new_dev.address;
    else if (ev->event == USB_HOST_CLIENT_EVENT_DEV_GONE && ev->dev_gone.dev_hdl == s.dev) s.gone = true;
}

/* the MIDI streaming interface and its bulk endpoints; false when the device has none */
static bool find_midi(const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    bool in_midi = false, found = false;
    s.ep_in = s.ep_out = 0;
    for (; p + 2 <= end && p[0] >= 2; p += p[0]) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)p;
            if (found) break;   /* the first MIDI interface's endpoints only */
            in_midi = i->bInterfaceClass == USB_CLASS_AUDIO && i->bInterfaceSubClass == 3 && i->bAlternateSetting == 0;
            if (in_midi) {
                s.intf = i->bInterfaceNumber;
                found = true;
            }
        } else if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_midi) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)p;
            if ((e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK) continue;
            if (e->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                if (!s.ep_in) {
                    s.ep_in = e->bEndpointAddress;
                    s.mps_in = USB_EP_DESC_GET_MPS(e);
                }
            } else if (!s.ep_out) {
                s.ep_out = e->bEndpointAddress;
            }
        }
    }
    return found && s.ep_in && s.ep_out;
}

static void product_name(void)
{
    usb_device_info_t info;
    s.name[0] = 0;
    if (usb_host_device_info(s.dev, &info) != ESP_OK || !info.str_desc_product) return;
    const usb_str_desc_t *d = info.str_desc_product;
    int n = (d->bLength - 2) / 2, o = 0;
    for (int i = 0; i < n && o < (int)sizeof s.name - 1; i++) {
        uint16_t c = d->wData[i];
        s.name[o++] = c >= 32 && c < 127 ? (char)c : '?';
    }
    s.name[o] = 0;
}

static void open_device(uint8_t addr)
{
    const usb_config_desc_t *cfg;
    if (s.dev) return;   /* one device at a time */
    if (usb_host_device_open(s.client, addr, &s.dev) != ESP_OK) {
        ESP_LOGW(TAG, "device %d: cannot open", addr);
        s.dev = NULL;
        return;
    }
    if (usb_host_get_active_config_descriptor(s.dev, &cfg) != ESP_OK || !find_midi(cfg)) {
        ESP_LOGI(TAG, "device %d: no MIDI streaming interface, left alone", addr);
        usb_host_device_close(s.client, s.dev);
        s.dev = NULL;
        return;
    }
    if (usb_host_interface_claim(s.client, s.dev, s.intf, 0) != ESP_OK) {
        ESP_LOGW(TAG, "device %d: cannot claim interface %d", addr, s.intf);
        usb_host_device_close(s.client, s.dev);
        s.dev = NULL;
        return;
    }
    product_name();
    s.gone = false;
    s.rx_on = false;
    xQueueReset(s.q);
    s.in->device_handle = s.out->device_handle = s.dev;
    s.in->bEndpointAddress = s.ep_in;
    s.out->bEndpointAddress = s.ep_out;
    s.in->num_bytes = usb_round_up_to_mps(IN_BUF, s.mps_in ? s.mps_in : 64);
    if (s.in->num_bytes > IN_BUF) s.in->num_bytes = IN_BUF;
    s.in_busy = true;
    if (usb_host_transfer_submit(s.in) != ESP_OK) {
        s.in_busy = false;
        ESP_LOGW(TAG, "device %d: IN transfer not taken", addr);
    }
    s.gen++;
    s.up = true;
    ESP_LOGI(TAG, "\"%s\" open: interface %d, IN 0x%02x (%d), OUT 0x%02x", s.name, s.intf, s.ep_in, s.mps_in, s.ep_out);
}

static void close_device(void)
{
    s.up = false;
    if (xSemaphoreTake(s.send_lock, pdMS_TO_TICKS(2000)) != pdTRUE) ESP_LOGW(TAG, "a send still holds the link");
    /* the transfers end (NO_DEVICE) as the events are handled */
    for (int i = 0; i < 200 && (s.in_busy || s.out_busy); i++) usb_host_client_handle_events(s.client, pdMS_TO_TICKS(10));
    if (!s.gone) {
        usb_host_endpoint_halt(s.dev, s.ep_in);
        usb_host_endpoint_flush(s.dev, s.ep_in);
    }
    usb_host_interface_release(s.client, s.dev, s.intf);
    usb_host_device_close(s.client, s.dev);
    s.dev = NULL;
    s.gone = false;
    xSemaphoreGive(s.send_lock);
    ESP_LOGI(TAG, "\"%s\" closed", s.name);
}

static void client_task(void *arg)
{
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s.client));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(IN_BUF, 0, &s.in));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(OUT_BUF, 0, &s.out));
    s.in->callback = in_cb;
    s.out->callback = out_cb;
    for (;;) {
        usb_host_client_handle_events(s.client, pdMS_TO_TICKS(50));
        if (s.new_addr) {
            uint8_t a = s.new_addr;
            s.new_addr = 0;
            open_device(a);
        }
        if (s.gone && s.dev) close_device();
    }
}

void usb_midi_start(void)
{
    s.q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    s.out_done = xSemaphoreCreateBinary();
    s.send_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
    xTaskCreatePinnedToCore(client_task, "usb_midi", 4096, NULL, 9, NULL, 0);
}

bool usb_midi_up(void) { return s.up; }
uint32_t usb_midi_generation(void) { return s.gen; }

void usb_midi_name(char *out, size_t len)
{
    snprintf(out, len, "%s", s.up ? s.name : "");
}

bool usb_midi_send(const uint8_t *p, size_t n)
{
    if (!s.up || n > USB_MIDI_SYSEX_MAX) return false;
    if (xSemaphoreTake(s.send_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool ok = false;
    if (s.up) {
        size_t o = 0;
        uint8_t *b = s.out->data_buffer;
        for (size_t i = 0; i < n; i += 3) {
            size_t k = n - i > 3 ? 3 : n - i;
            bool last = i + k == n;
            uint8_t cin = !last ? 0x4 : k == 1 ? 0x5 : k == 2 ? 0x6 : 0x7;
            b[o] = cin;   /* cable 0 */
            b[o + 1] = p[i];
            b[o + 2] = k > 1 ? p[i + 1] : 0;
            b[o + 3] = k > 2 ? p[i + 2] : 0;
            o += 4;
        }
        s.out->num_bytes = o;
        s.out->timeout_ms = 1000;
        xSemaphoreTake(s.out_done, 0);
        s.out_busy = true;
        if (usb_host_transfer_submit(s.out) == ESP_OK) {
            if (xSemaphoreTake(s.out_done, pdMS_TO_TICKS(1500)) == pdTRUE) ok = s.out_status == USB_TRANSFER_STATUS_COMPLETED;
            else ESP_LOGW(TAG, "OUT transfer did not finish");
        } else {
            s.out_busy = false;
        }
    }
    xSemaphoreGive(s.send_lock);
    return ok;
}

int usb_midi_read(uint8_t *buf, size_t cap, uint32_t ms)
{
    static msg_t m;   /* one reader */
    if (xQueueReceive(s.q, &m, 0) != pdTRUE) {
        if (!s.up) return -1;
        if (xQueueReceive(s.q, &m, pdMS_TO_TICKS(ms)) != pdTRUE) return s.up ? 0 : -1;
    }
    if (m.len > cap) return 0;
    memcpy(buf, m.data, m.len);
    return m.len;
}

void usb_midi_drain(void)
{
    xQueueReset(s.q);
}
