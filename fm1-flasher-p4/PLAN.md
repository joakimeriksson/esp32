# fm1-flasher-p4: the plan

The FM-1's firmware installed from the P4 board (2026-10-28): the Hub's catalog, fm1-dock, the official one
and the SD card, over USB-MIDI from the P4's USB host port; fm1-dock's modules the same way.

## Two modes

1. **Firmware, at your own risk**: a whole `.fwsc` through the FM-1's updater (the Hub's, fm1-dock's and
   Felucca's protocol). Checks before anything is sent; the risk box on screen.
2. **fm1-dock modules, safe**: a `.dock` into a running fm1-dock (`tools/dock.py deploy`; firmware/src/dock.c):
   frames `F0 7D 48 42 cmd seq pack7(payload + CRC-32) F7`, each answered over USB (`cmd | 0x40`, a status
   byte). HELLO, LIST, BEGIN, DATA (256-byte chunks), COMMIT, DELETE, SELECT. The modules from fm1-dock's page
   (`docs/modules/modules.json`) and the card. The same USB-MIDI link. Boot modules and probes are refused.

## Decided

- The protocol code is plain C (`components/fm1`), checked on the computer against fm1-dock's
  `tools/fm1_install.py` and `tools/hbdeploy.py` (the reference, not reimplemented by reading): the
  package functions keep their names.
- Only packages with Felucca's update loader, and M-VAVE's V15 by its digest (as fm1-dock's and Felucca's
  installers): the loader is what makes a cut-off install resumable. The Hub's M-VAVE-loader firmwares and
  those with their own installer stay on the Hub's page.
- A cut-off install is finished only with Felucca's own loader (`ota-FM-1_9xx`), as fm1-dock does.
- The package in PSRAM (the FM-1 has 1 MB of flash; packages are 600-700 KB); the catalog's cJSON in PSRAM.
- One device on the USB port; the worker task owns the link.
- The SD card mirrors the Hub and fm1-dock's page (`/sdcard/fm1/hub`, `/sdcard/fm1/modules`): the lists
  and what is saved, chosen one by one or all at once; offline, the card's copies are checked the same way.
  The card is mounted at 20 MHz, its LDO released when a mount fails (the BSP's 40 MHz mount and kept LDO
  left a card that would not start unmountable until reseated).

## Milestones

1. The protocol and the checks, tested on the computer. **Done 2026-10-28** (9 packages, the same lines as
   fm1_install.py; eight install cases against a simulated FM-1).
2. The firmware builds: USB-MIDI host, WiFi, catalog, SD, screen. **Done 2026-10-28**.
3. On the board: the FM-1's identity on screen. **Done 2026-10-28**: the OTG port powers the FM-1; it is a
   full-speed MIDI interface ("Felucca", interface 1, bulk 0x81 / 0x01, 64 bytes); `FM-1_956` (fm1-dock 0.6)
   answers the handshake and fm1-dock's HELLO (ABI 2, 5 modules, 28 KB free).
4. fm1-dock modules (mode 2). **Done 2026-10-28**: the frames byte for byte hbdeploy.py's (5 modules),
   deploy / list / select / delete and lossy, no-room and lost-COMMIT cases against a simulated fm1-dock;
   on the FM-1, AMB-1 refused for no room (76 KB, 28 KB free; now said before anything is sent) and a
   module deployed.
5. The card mirror: **Done 2026-10-28**, all 115 firmwares on a 60 GB card (67 MB).
6. Power: the board, its screen, WiFi and the FM-1 on one USB port of the computer dropped the board's
   USB-UART and dimmed the screen while the FM-1 was on the OTG port. To measure (`started after ...` at
   boot says when it is a brownout); a stronger supply or a powered hub for now.
7. A real install: back to the official V15, fm1-dock from its release, a Hub firmware, one from the card.
8. The ROM-bootloader rescue (the Linux-only `fm1_rescue.py` on the P4): the key over MIDI
   (`F0 22 24 35 7D F7`), the "UBOOT1.00" disk (USB mass storage, bulk-only, JieLi's 16-byte CDBs `FB` /
   `FC` / `FD`), the flash helper (`wl82loader.bin` from jl-uboot-tool, MIT, by its SHA-256, kept on the
   card) into RAM, the flash ID (`856014`) and the FM-1's own chip key checked, then only the app area
   (0x4000..0x93000), only the sectors that differ, each read back. A check-only mode first (reads and
   compares, writes nothing: off and on brings the FM-1 back as it was). V15 after it is a normal install.
9. The SMK-37 family (the checks already tell the boards apart).
10. The FM-1 browses the P4 (below).

## The FM-1 browses the P4: esp32-dock (2026-10-28)

Done the small way, in [esp32-dock](../esp32-dock) with this project's `components/`: the browser is a
*probe* (fm1-dock's own: sent as a deploy, RUN instead of COMMIT, run from RAM, nothing written) that the P4
sends when an fm1-dock is plugged in, the library list patched into its `.data` (`library.h`); its `run()`
returns the pick, read with RESULT, then the normal deploy or install. On the P4: `fm1_dock_run` /
`fm1_dock_run_wait` / `fm1_dock_result` / `fm1_dock_probe_patch` in `components/fm1`, tested against
`probe.py`'s frames and a simulated fm1-dock; the worker's library mode. Still to do on the FM-1 side:
fm1-dock's probe context gains `poll`, the panel and the encoders (about 150 bytes of host flash), and
`probes/browser.c` itself. The plan below (a module started from RAM, frames up the cable, a picker line)
stays for "try before you keep" and tunes from the card.

## Later: the FM-1 browses the P4's card, the long way

The FM-1's flash is small (28 KB free with 5 modules); the card holds everything. The FM-1 picks from the
card on its own screen and keys, and the P4 serves it.

**A RAM module streamed by the P4, not a browser built into fm1-dock.** fm1-dock's probes already are code
sent as a deploy (BEGIN / DATA), relocated and run from RAM, never written (`pr_run` in dock.c; up to
192 KB of code at HB_MOD_RAM, 80 KB of .data / .bss at HB_PROBE_RAM). A probe runs once with fm1-dock's
loop on hold; the browser has to stay up. So:

1. **fm1-dock: start a received module from RAM** as the one playing (a command beside RUN: `pr_run`'s
   checks and relocation, then made the active module instead of called once), with fm1-dock's loop, USB,
   screen and keys running and the module's own callbacks (`ui`, `draw`, `note`). Gone at power-off; the
   picker and the flash slots as they were.
2. **fm1-dock: a host call to send up the cable** (FM-1 to P4): SysEx does not reach a module ("SysEx is
   the platform's"), so the browser asks through the host, as a new dock frame the P4 listens for.
3. **P4 to FM-1 is there already**: MODULE frames reach the playing module's `data()` (as FM-1 SID takes
   tunes): the list pages, 32 entries at a time (name, version, size, fits the free flash or not).
4. **fm1-dock: a picker line, "P4 library"**, that asks the P4 for the browser; or the P4 pushes it from
   its screen.
5. **The P4: listen for the FM-1's frames** (unasked frames from the device; today it only answers what it
   asked), answer from the card, and on "this one" deploy it (or delete one first when there is no room).

The frames to agree on first: the browser's request (list from n, deploy id, run id, tune id) and the
P4's pages, in fm1-dock's frame format (`F0 7D 48 42 cmd seq ... F7`), new command numbers.

Then, on the same path:
- **Try before you keep**: a module run straight from the card in RAM (code up to 192 KB, data up to
  80 KB: FoMni and AMB-1 by file size, FM-1 SID at 270 KB not), kept only with a normal deploy.
- **SID tunes from the card**: FM-1 SID browses a folder of tunes on the P4 (HVSC fits many times over) and
  plays them through MODULE frames: nothing written to flash.

Sizes (fm1-dock 0.5's symbols, `app.ld`): the host's code area is 192 KB up to the slot table at 0x02030000,
the host about 50-60 KB of it. The browser itself is not in fm1-dock at all; what fm1-dock gains (the
RAM start, the send-up call, the picker line) is a few KB, and none of it from the module slots.
