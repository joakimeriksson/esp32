# Scintilla on the ESP32-P4 LCD-3.5: the port plan, as checked against the board

The plan of 2026-10-25 (Scintilla's engine on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5: speaker,
touch keyboard, a few controls, settings in NVS; no Synthex panel, patch editing, MIDI or radio yet),
with what the board and the sources changed in it.

## Changed against the board (verified 2026-10-25)

- **Layout**: a directory of joakimeriksson/esp32 like its other projects, `scintilla-p4-lcd35`,
  with the engine as `scintilla-p4-lcd35/engine`, a git subtree of scintilla-fm1's `engine/`. The
  engine stays untouched; changes go upstream, then come back with
  `git subtree pull --prefix=scintilla-p4-lcd35/engine`.
- **No PMIC.** Waveshare's BSP `waveshare/esp32_p4_wifi6_touch_lcd_3_5` 2.0.2 (Apache-2.0) has no
  AXP2101 or other power chip: the backlight is LEDC on GPIO 28 and the speaker amplifier's enable is
  GPIO 53. The BSP brings up the LCD, touch, codec and SD card; the pins are its
  (`include/bsp/esp32_p4_wifi6_touch_lcd_3_5.h`), not copied by hand.
- **The ES8311 is mono**: one DAC into the amplifier, and the BSP sets I2S to mono. The engine's
  stereo output is mixed to mono as the last step ((L + R) / 2, saturated to 16 bits).
- **The console stays on UART0**: the board's USB-C port is a WCH CH343 USB-UART bridge on UART0, not
  the P4's USB-Serial/JTAG, so `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` would hide the console.
- **The engine already has the event queue**: `sc_post` is a 64-entry one-writer, one-reader queue
  meant for exactly this (sc_synth.h: "sc_render() runs in the audio interrupt; everything else may
  run in the main loop"). Notes go through it; no second ring buffer. A preset change is taken in the
  audio task between blocks: `sc_all_off`, the patch copied, `sc_reprep`.
- **Blocks of 128 frames**, as the host renderer cuts runs (upstream CLAUDE.md: a voice's coefficient
  clock depends on the run length), so the P4 renders the bits the host does before the mono mix.
- **LVGL 9.6 through the BSP's esp_lvgl_adapter** (not esp_lvgl_port), a 50-line buffer in internal
  RAM; the BSP's touch device is single-point, so the two-finger keyboard reads the FT5x06 itself.
- **PSRAM**: 32 MB, AP Memory x16 at 200 MHz, memory test OK at boot (the plan was right).
- **Touch**: an FT5x06-family controller (`esp_lcd_touch_ft5x06`, which also drives the FT6336).

## Milestones

1. Board bring-up: LCD colour bars, touch coordinates on the console, a 440 Hz sine. **Done
   2026-10-25** (the codec at 44.1 kHz mono).
2. The engine plays preset 0 from a timer, the render cost printed. **Done 2026-10-25**: Chunky
   Synth, 84 KB of state, 166 us per 128-frame block (5.7 % of the 2902 us budget), worst 203 us after
   the first block. The 16-voice case (both layers) is still to measure.
3. Touch keyboard, single touch.
4. Settings in NVS and console commands; preset, octave and volume on screen.
5. Two-finger keyboard, legato, the patch name.
6. Polish: boot to the last preset, screen dimming, README with build steps and the numbers.

Later (seams left): USB-MIDI on the OTG port, a delay with its line in PSRAM, patch saving to the
`storage` partition, SID-1 as a second engine, Synthex panel pages.

## Acceptance

`idf.py build flash monitor` from a clean checkout with the pinned IDF; boots to the last preset in
under 2 s; the keyboard plays without clicks; two-finger chords; the host renderer gives the same
bits for the engine revision in use; the README lists the board, IDF version, pins and render cost.
