/* SPDX-License-Identifier: GPL-3.0-only */
/* USB-MIDI host on the P4's OTG port: one class-compliant MIDI device (the FM-1, its update loader)
 * and its SysEx. ESP-IDF has no MIDI host class driver; this claims the MIDI streaming interface
 * and moves 4-byte USB-MIDI event packets over its two bulk endpoints. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_MIDI_SYSEX_MAX 1024

void usb_midi_start(void);
/* a MIDI device is open */
bool usb_midi_up(void);
/* counts the devices opened: a new number is another connection */
uint32_t usb_midi_generation(void);
/* the device's product string ("FM-1"), "" when none */
void usb_midi_name(char *out, size_t len);
/* one whole SysEx (F0 ... F7); false when the device is gone or did not take it */
bool usb_midi_send(const uint8_t *sysex, size_t n);
/* the next SysEx from the device: its length; 0 on timeout; -1 when no device is open */
int usb_midi_read(uint8_t *buf, size_t cap, uint32_t ms);
void usb_midi_drain(void);
