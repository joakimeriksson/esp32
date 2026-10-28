# fm1-flasher-p4

Installs firmware on an M-VAVE FM-1 from the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5, no computer: the FM-1 on
the P4's USB host port, the firmware picked on the touch screen. (A working name.)

**What it does**:
- lists [the FM-1 Firmware Hub](https://thefirmwarehub.com/)'s firmwares (its `catalog.json`), M-VAVE's
  official V15 as the Hub keeps it, the latest [fm1-dock](https://github.com/joakimeriksson/fm1-dock) release,
  and the `.fwsc` packages on the SD card (its root or `/fm1`);
- shows what the FM-1 on the USB port runs (its identity, named from the catalog);
- fetches a package into PSRAM and checks it before anything is sent: size and SHA-256 against the catalog,
  the package identity, every file's CRC (fm1-dock's `package_damage`), the board layout (an FM-1 package is
  never written to an SMK-37, nor the other way round), and Felucca's update loader;
- installs it over USB-MIDI the way the Hub, fm1-dock's and Felucca's installers do (below), after the
  "at my own risk" box is ticked, with the steps on screen;
- finishes an install that was cut off: an FM-1 left in Felucca's update mode (`ota-FM-1_9xx`) gets the
  write again; another firmware's update mode is left alone;
- deploys fm1-dock modules (the safe mode, the Modules tab): fm1-dock's page's and the card's `.dock`,
  checked against what fm1-dock reports (ABI, size, room) before anything is sent; the FM-1's modules played
  or deleted; boot modules and probes refused;
- keeps the Hub on the SD card for offline (`/sdcard/fm1/hub`, `/sdcard/fm1/modules`): the lists and what
  you save, one firmware or module at a time or all at once; without WiFi it all comes from the card,
  checked the same way.

**Installing firmware is at your own risk.** What can go wrong, and the ways back:
- cut off in step 1 (the running firmware reads the package): nothing was written;
- cut off in step 2 (the loader writes): the FM-1 stays in update mode; plug it in again and install again;
- a firmware that runs but has no working USB: fm1-dock's own ways back (OCT− at power-on: only the updater;
  HOME at power-on), else the chip's ROM bootloader (the Hub's rescue, `F0 22 24 35 7D F7` then the
  "UBOOT1.00" disk, from Linux; not here yet).

## The update protocol

All SysEx over USB-MIDI, the device asking and the flasher answering (fm1-dock's `tools/fm1_install.py`,
after Felucca's `web/fm1ota.js`, GPL-3.0):

1. identity query `F0 00 32 45 00 00 00 40 7F F7`, the answer `FM-1_975` (model, number);
2. `F0 22 24 35 7F F7` starts the update: the running firmware sends read requests (address, length up to
   512, 7-bit packed with a checksum), each answered with that part of the package, until it asks for
   `0xE0000000`; it restarts as the update loader (`ota-FM-1_9xx`);
3. the same start to the loader, which reads the whole package and writes it, until `0xF0000000`;
4. the FM-1 restarts, and its identity must be the package's.

## Layout

- `components/fm1/`: the package checks and the install in plain C, no ESP-IDF (`fm1_pkg.c`, `fm1_ota.c`);
  `fm1_dock.c`: fm1-dock's deploy protocol and its probes (RUN / RESULT, a probe's data patched), plain C too;
- `components/flasher/`: the flasher without a screen or a board, shared with [esp32-dock](../esp32-dock):
  - `usb_midi.c`: a USB-MIDI host on the USB Host Library: the MIDI streaming interface's bulk endpoints,
    SysEx in and out of 4-byte event packets;
  - `net.c`: WiFi through the C6 (ESP-Hosted), the network in NVS, HTTPS into PSRAM;
  - `catalog.c`: the Hub, the official V15, fm1-dock's release and modules, the SD card and its mirror;
    a package's checks;
  - `flasher.c`: the worker task (the device's identity, downloads, the install, WiFi, the library: the
    browser probe onto an fm1-dock, its pick acted on);
  - `ui.h`: what the worker tells the app (the app provides these); `board.h`: what the board provides;
- `main/ui.c`: the screen, landscape as in scintilla-p4-lcd35; `main/board.c`: the Waveshare board through
  its BSP (the OTG port as USB host, the card).

## Build and test

ESP-IDF 5.5.4 (`~/esp/esp-idf-v5.5.4`), Waveshare's BSP 2.0.2:

```sh
. ~/esp/esp-idf-v5.5.4/export.sh
idf.py build flash monitor -b 230400
```

At 230400 baud, as for scintilla-p4-lcd35 (faster stops part way on this board). The console is UART0, the
board's USB-C (a CH343); the FM-1 goes on the other USB port, the P4's own. Without ESP-IDF installed,
[`../p4.sh`](../p4.sh) builds in the `espressif/idf:v5.5.4` Docker image and flashes with `esptool` through
`uv` (`../p4.sh build`, `flash`, `monitor` in the project's directory).

The protocol and the checks on the computer, against fm1-dock's Python on real packages (the package lines
diffed against `fm1_install.py`'s, then whole installs against a simulated FM-1: the full way, a resumed
write, another firmware's loader, an SMK-37, the cable out, a firmware that does not answer, another
identity after the write, nothing on the cable):

```sh
test/run.sh ~/path/to/fm1-dock fm1-dock-0.6.fwsc FM-1_015.fwsc zp12-2.5.fwsc ... docs/modules/*.dock docs/probes/*.dock
```

The `.dock` files the same way against `tools/hbdeploy.py` (the deploy frames byte for byte), then
deploys against a simulated fm1-dock: list, play, delete, lost frames, no room, a lost COMMIT answer,
another ABI, a boot module, no fm1-dock. The probes (`.dock` files with the probe flag): their frames end
with RUN as `probe.py` sends it; against the simulated fm1-dock a run and its RESULT, a patched byte
(the file CRC redone, as fm1-dock checks it), a lost RUN and a lost RUN answer (found out through
HELLO and RESULT), a COMMIT of a probe refused.

The official V15 fails fm1-dock's file CRC check (its `USR` file), in the Python too: it is taken by its
SHA-256 alone (the digest Felucca and the Hub pin). Firmwares with M-VAVE's loader (the Hub's `loader:
"mvave"`) or their own installer are left to the Hub's page.

## Status

On the board (2026-10-28): the OTG port powers the FM-1, a full-speed MIDI interface; fm1-dock 0.6
(`FM-1_956`) answers the updater's handshake and its own HELLO; modules deploy (and are refused when there
is no room); the whole Hub (115 firmwares, 67 MB) on the card. Not yet: a firmware install on the FM-1.

**Power**: the board, its screen, WiFi and the FM-1 on one USB port of a computer dropped the board's
USB-UART and dimmed the screen. Give the board a stronger supply (a powered hub, a charger) when the FM-1 is
on it; the boot log says `started after BROWNOUT` when it is that.

Next ([PLAN.md](PLAN.md)): a real install, the ROM-bootloader rescue (the Linux-only one, here), the
SMK-37. The FM-1 browsing the P4 is [esp32-dock](../esp32-dock), the same worker without a screen.

GPL-3.0-only ([LICENSE](LICENSE)), as the installers it follows.
