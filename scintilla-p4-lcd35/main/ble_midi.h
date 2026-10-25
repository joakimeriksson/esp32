/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
/* MIDI over Bluetooth LE as a peripheral, input only: NimBLE here, the C6's controller over
 * ESP-Hosted. Notes, pitch bend, program change (the preset) and all-notes-off become events. */
void ble_midi_start(const char *name);
bool ble_midi_connected(void);
