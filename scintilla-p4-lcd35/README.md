# Scintilla on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5

[Scintilla](https://github.com/joakimeriksson/scintilla-fm1), the 1981 Elka Synthex in integer C, on
Waveshare's ESP32-P4-WIFI6-Touch-LCD-3.5 (ESP32-P4 v3.2, 32 MB PSRAM, 16 MB flash, ST7796 320x480 on SPI,
FT5x06-family touch, ES8311 codec with a speaker amplifier). The engine in `engine/` is a git subtree of
scintilla-fm1's `engine/` and is not edited here.

**What it does**: a landscape screen in synthex-web's panel colours, with
- an 18-key touch keyboard (C to F), octave -2..+2, sliding across the keys retriggers;
- the 80 factory presets, by ◀ ▶ or a picker in the Synthex numbering (tap the program display: bank,
  group, then the patch by name);
- volume (the codec's output level), and SOUND to silence everything;
- INSTRUMENT: six tabs (OSC, MIX, FILTER, ENV, LFO, FX) that edit the playing patch live, upper or
  lower layer, with synthex-web's ranges (edits are not saved yet);
- MIDI over Bluetooth LE: it advertises as "Scintilla" (NimBLE on the P4, the on-board C6's controller
  through ESP-Hosted; works with the C6's factory firmware). Notes, pitch bend, program change and
  all-notes-off. From a Mac: Audio MIDI Setup > MIDI Studio > Bluetooth, or atech-firmware's
  `tools/midi-keys.py`. The BT MIDI light shows a connection.

The engine runs alone on core 1 and renders a 128-frame block in about 80 us, under 3 % of the
2902 us budget; touch and BLE MIDI reach it through one FreeRTOS queue that it drains between blocks.
Single touch for now (the BSP's LVGL input forwards one point).

Built with ESP-IDF v5.5.4 and Waveshare's BSP `waveshare/esp32_p4_wifi6_touch_lcd_3_5` 2.0.2, which brings
up the display, touch, codec and SD card:

    idf.py set-target esp32p4 build
    idf.py -b 230400 flash monitor

Flash at 230400 baud: on this board 460800 and 921600 stop part way ("The chip stopped responding").

The console is UART0, through the board's USB-C (a CH343 USB-UART bridge). The ES8311 has one DAC, so the
engine's stereo output is mixed to mono as the last step. GPL-3.0-only, as the engine ([LICENSE](LICENSE)).
