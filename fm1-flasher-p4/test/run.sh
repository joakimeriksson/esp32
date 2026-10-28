#!/bin/sh
# components/fm1 on the computer, against fm1-dock's Python (tools/fm1_install.py, tools/hbdeploy.py):
#   test/run.sh FM1_DOCK_CHECKOUT PACKAGE.fwsc... MODULE.dock...
set -e
here=$(cd "$(dirname "$0")" && pwd)
dock=$1; shift
out=${TMPDIR:-/tmp}/fm1-flasher-test
mkdir -p "$out"
pkgs=""; mods=""
for f in "$@"; do
    case "$f" in
    *.dock) mods="$mods $f" ;;
    *) pkgs="$pkgs $f" ;;
    esac
done
cflags="-std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -I$here/../components/fm1/include"
src="$here/../components/fm1/fm1_pkg.c $here/../components/fm1/fm1_ota.c $here/../components/fm1/fm1_dock.c"
cc $cflags "$here/test_fm1.c" $src -o "$out/test_fm1"
cc $cflags "$here/test_dock.c" $src -o "$out/test_dock"
if [ -n "$pkgs" ]; then
    python3 -I "$here/pkg_ref.py" "$dock/tools" $pkgs > "$out/ref.txt"
    "$out/test_fm1" $pkgs > "$out/c.txt"
    diff "$out/ref.txt" "$out/c.txt" && echo "package lines: the same as fm1_install.py's ($(wc -l < "$out/c.txt" | tr -d ' ') packages)"
fi
if [ -n "$mods" ]; then
    python3 -I "$here/dock_ref.py" "$dock/tools" $mods > "$out/dref.txt"
    "$out/test_dock" $mods > "$out/dc.txt"
    diff "$out/dref.txt" "$out/dc.txt" && echo "deploy frames: the same as hbdeploy.py's ($(wc -l < "$out/dc.txt" | tr -d ' ') modules)"
fi
