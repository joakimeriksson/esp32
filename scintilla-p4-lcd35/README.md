# Scintilla on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5

[Scintilla](https://github.com/joakimeriksson/scintilla-fm1), the 1981 Elka Synthex in integer C, on
Waveshare's ESP32-P4-WIFI6-Touch-LCD-3.5 (ESP32-P4 v3.2, 32 MB PSRAM, 16 MB flash, ST7796 320x480 on SPI,
FT5x06-family touch, ES8311 codec with a speaker amplifier). The engine in `engine/` is a git subtree of
scintilla-fm1's `engine/` and is not edited here.

**Status**: milestones 1 and 2 of [PLAN.md](PLAN.md). At boot: colour bars, touch coordinates on the
console, a 2 s 440 Hz tone, then preset 0 (Chunky Synth) as an arpeggio. The engine renders a 128-frame
block in 166 us, 5.7 % of one core's 2902 us budget; its 84 KB of state is in internal RAM.

Built with ESP-IDF v5.5.4 and Waveshare's BSP `waveshare/esp32_p4_wifi6_touch_lcd_3_5` 2.0.2, which brings
up the display, touch, codec and SD card:

    idf.py set-target esp32p4 build flash monitor

The console is UART0, through the board's USB-C (a CH343 USB-UART bridge). The ES8311 has one DAC, so the
engine's stereo output is mixed to mono as the last step. GPL-3.0-only, as the engine ([LICENSE](LICENSE)).
