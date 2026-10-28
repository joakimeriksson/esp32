#!/bin/sh
# The P4 projects here without ESP-IDF on the computer: the build in Espressif's Docker image, the flash and
# the monitor through uv (esptool, pyserial), since Docker on a Mac cannot reach the serial port.
#
#   cd fm1-flasher-p4        (or esp32-dock, ...)
#   ../p4.sh build           idf.py set-target esp32p4 (the first time) and build, in the image
#   ../p4.sh flash [PORT]    esptool at 230400 baud (faster stops part way on the Waveshare board)
#   ../p4.sh monitor [PORT]  the console at 115200 baud; ctrl-] leaves
#   ../p4.sh idf ARGS...     any other idf.py command in the image (menuconfig, size, fullclean)
#
# PORT: the board's UART (/dev/cu.wchusbserial* on a Mac); found by itself when there is one.
set -e
IMAGE=${IDF_IMAGE:-espressif/idf:v5.5.4}
root=$(cd "$(dirname "$0")" && pwd)
case "$PWD" in "$root"/*) ;; *) echo "run it from a project directory under $root" >&2; exit 2 ;; esac
proj=${PWD#"$root"/}

in_image() {
    # the whole repo mounted: esp32-dock takes its components from ../fm1-flasher-p4
    tty=""; [ -t 0 ] && tty="-it"   # a terminal for menuconfig; none when run from a script
    docker run --rm $tty -v "$root:/work" -w "/work/$proj" -e IDF_COMPONENT_MANAGER=1 "$IMAGE" sh -c "$*"
}

port() {
    if [ -n "$1" ]; then echo "$1"; return; fi
    set -- /dev/cu.wchusbserial* /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*
    for p in "$@"; do [ -e "$p" ] && { echo "$p"; return; }; done
    echo "no board on USB (no serial port found); give the port: $0 flash /dev/cu.xxx" >&2
    exit 1
}

cmd=$1; [ $# -gt 0 ] && shift
case "$cmd" in
build)
    [ -f sdkconfig ] || in_image "idf.py set-target esp32p4"
    in_image "idf.py build"
    ;;
idf)
    in_image "idf.py $*"
    ;;
flash)
    p=$(port "$1")
    [ -f build/flash_args ] || { echo "nothing built: $0 build first" >&2; exit 1; }
    # as idf.py flash: the arguments the build wrote (bootloader, partition table, the app)
    (cd build && uv run --with esptool python -m esptool --chip esp32p4 -p "$p" -b 230400 --before default_reset --after hard_reset \
        write_flash @flash_args)
    ;;
monitor)
    p=$(port "$1")
    uv run --with esptool python -m serial.tools.miniterm --raw --exit-char 29 "$p" 115200
    ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
