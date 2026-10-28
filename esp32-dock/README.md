# esp32-dock

[fm1-flasher-p4](../fm1-flasher-p4) without a screen: an ESP32-P4 board with the M-VAVE FM-1 on its USB host
port, the FM-1 browsing the library on **its own** screen and keys. What it picks is installed (a firmware,
at your own risk) or deployed (an fm1-dock module). A console on the board's UART for the rest.

**How the FM-1 browses.** fm1-dock runs *probes*: programs sent over USB-MIDI like a module (BEGIN / DATA)
but started with RUN instead of COMMIT, relocated and run from the FM-1's RAM, nothing written to flash. The
browser is one (fm1-dock's `probes/browser.c`). This board patches the library list into the browser's data
(`components/flasher/library.h` has the layout), sends it when an fm1-dock is plugged in, and waits; the
browser shows the list, the user turns SELECT and picks (holding SAVE for a firmware: the risk box), and
the probe returns the pick. Then the ordinary deploy or install follows, with fm1-dock's own receiving
screen or the updater's. The browser is gone at power-off; it is sent again at the next plug-in, or with
`browse`.

The browser needs fm1-dock 0.7 or later (its probe context gained the panel and `poll`), and the probe
itself: `probes/browser.dock` built into this firmware when the file is there at build time, else
`/fm1/browser.dock` on the SD card. Until it exists the console does everything the screen does.

## The console

On UART0 (the board's USB-C) at 115200 baud (`idf.py monitor`, or any terminal). `help` lists the
commands:

    wifi SSID [password]      the network, saved in NVS; `wifi` alone says which
    scan                      the networks around
    dev                       what is on the USB port: the FM-1, its fm1-dock modules
    ls                        the library: modules m0.., firmwares f0..
    get f3                    fetch a firmware and check it (size, SHA-256, identity, CRCs, loader)
    install yes               install the one fetched, at your own risk (fm1-flasher-p4's README)
    get m2                    deploy a module onto the FM-1
    play NAME / delete NAME   the FM-1's modules
    save f3 | m2 | all        onto the card's mirror (offline use); rm takes it off
    browse                    the browser onto the FM-1 now
    library on | off          whether it is sent by itself when an fm1-dock is plugged in

## The board

The Waveshare **ESP32-P4-WIFI6** (no display), with plain ESP-IDF and no BSP: `main/board.c` is the board, the
USB host port and the TF card. The FM-1 goes on the 4-pin USB 2.0 OTG header (MX1.25) and is powered from its
VBUS; the TF card is on SDMMC slot 0 behind a load switch on GPIO 45; the C6 is on SDIO slot 1 with its reset
on GPIO 54, which are ESP-Hosted's defaults (`sdkconfig.defaults`). The console is the Type-C port (a CH343).

The same firmware on the ESP32-P4-WIFI6-Touch-LCD-3.5 with its screen off: `idf.py -DDOCK_BOARD=lcd35 build`
(`../p4.sh idf -DDOCK_BOARD=lcd35 build`); only the card's power differs (the chip's LDO 4).

## Build

ESP-IDF 5.5.4; everything but `main/` comes from `../fm1-flasher-p4/components` (`EXTRA_COMPONENT_DIRS`):

```sh
. ~/esp/esp-idf-v5.5.4/export.sh
idf.py set-target esp32p4 build
idf.py -b 230400 flash monitor
```

Without ESP-IDF installed, [`../p4.sh`](../p4.sh) does the same with the `espressif/idf:v5.5.4` Docker image
for the build and `esptool` through `uv` for the flash (the serial port is the computer's, which Docker on a
Mac cannot reach):

```sh
../p4.sh build            # in this project's directory
../p4.sh flash            # the port found by itself (one board on USB), at 230400 baud
../p4.sh monitor          # 115200 baud; ctrl-] leaves
```

GPL-3.0-only ([LICENSE](LICENSE)).
