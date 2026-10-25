/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* What plays the engine, from any task: the touch keyboard, BLE MIDI, the instrument page. The audio
 * task applies them between blocks with the engine's immediate calls, so the engine's own one-writer
 * queue is not used. A patch edit names a field by its offset in sc_patch_t and its size. */
#include <stdbool.h>
#include <stdint.h>
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_ALL_OFF, EV_BEND, EV_PRESET, EV_PARAM8, EV_PARAM32 };
typedef struct { uint8_t type, note; uint16_t off; int32_t val; } ev_t;   /* val: velocity, bend Q16, preset or field value */
void events_init(void);
bool events_post(uint8_t type, uint8_t note, int32_t val);
bool events_param(bool wide, uint16_t off, int32_t val);
bool events_take(ev_t *e);                                  /* non-blocking, for the audio task */
