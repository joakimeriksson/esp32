/* SPDX-License-Identifier: BSD-3-Clause */
/* The BLE MIDI packet parser from atech-firmware (src/sid-theremin/midi_in.h, commit 0a92ea1),
 * Copyright (c) 2026, Joakim Eriksson, BSD 3-Clause License (see that repository's LICENSE), unchanged
 * apart from this header and the includes, so it compiles as C here. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/// A channel message: the status byte and up to two data bytes (the missing ones 0).
typedef void (*MidiEmit)(void* context, uint8_t status, uint8_t d1, uint8_t d2);
/// Data bytes that follow a status byte; system exclusive is handled apart.
static inline int midiDataBytes(uint8_t status) {
    switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
    default: return status == 0xF2 ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0;
    }
}

/// One BLE MIDI packet (the MIDI over Bluetooth LE specification): a header byte, then messages,
/// each behind a timestamp byte. Header and timestamps have bit 7 set, as status bytes do, so which
/// is which is a matter of position: the first high byte after data is a timestamp, a high byte
/// right after a timestamp is a status. Running status is allowed, with or without a timestamp in
/// front of the data. The timestamps are dropped — a note is played when it arrives. Channel
/// messages go to `emit`; real-time, system common and system exclusive are skipped.
static inline void bleMidiParse(const uint8_t* p, size_t n, MidiEmit emit, void* context) {
    if (n < 3 || !(p[0] & 0x80)) { return; }
    uint8_t status = 0;                          // running status does not carry from packet to packet
    size_t i = 1;
    while (i < n) {
        if (p[i] & 0x80) {                       // a timestamp
            if (++i >= n) { break; }
            if (p[i] & 0x80) {                   // and a status after it; otherwise running status goes on
                uint8_t s = p[i++];
                if (s >= 0xF8) { continue; }     // real-time: no data, and running status is left alone
                status = s;
            }
        }
        if (status == 0xF0) {                    // system exclusive: its data, up to the timestamp of its F7
            while (i < n && !(p[i] & 0x80)) { i++; }
            status = 0;
            continue;
        }
        if (status == 0) { if (i < n && !(p[i] & 0x80)) { i++; } continue; }   // data with nothing to belong to
        int need = midiDataBytes(status);
        if (i + need > n) { break; }
        bool whole = true;
        for (int k = 0; k < need; k++) { if (p[i + k] & 0x80) { whole = false; } }
        if (!whole) { status = 0; continue; }    // a high byte where data should be: take it as a timestamp and go on
        if (status < 0xF0) { emit(context, status, need > 0 ? p[i] : 0, need > 1 ? p[i + 1] : 0); }
        i += need;
        if (status >= 0xF0) { status = 0; }      // system common ends running status
    }
}
