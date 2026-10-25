/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* What plays the engine, from any task: the touch keyboard, BLE MIDI. The audio task applies them
 * between blocks with the engine's immediate calls, so the engine's own one-writer queue is not used. */
#include <stdbool.h>
#include <stdint.h>
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_ALL_OFF, EV_BEND, EV_PRESET };
typedef struct { uint8_t type, note; int32_t val; } ev_t;   /* val: velocity or bend Q16, or a preset index */
void events_init(void);
bool events_post(uint8_t type, uint8_t note, int32_t val);
bool events_take(ev_t *e);                                  /* non-blocking, for the audio task */
