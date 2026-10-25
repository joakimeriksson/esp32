/* SPDX-License-Identifier: GPL-3.0-only */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "events.h"

static QueueHandle_t q;
void events_init(void) { q = xQueueCreate(64, sizeof(ev_t)); }
bool events_post(uint8_t type, uint8_t note, int32_t val)
{
    ev_t e = { .type = type, .note = note, .val = val };
    return xQueueSend(q, &e, 0) == pdTRUE;
}
bool events_param(bool wide, uint16_t off, int32_t val)
{
    ev_t e = { .type = wide ? EV_PARAM32 : EV_PARAM8, .off = off, .val = val };
    return xQueueSend(q, &e, 0) == pdTRUE;
}
bool events_take(ev_t *e) { return xQueueReceive(q, e, 0) == pdTRUE; }
