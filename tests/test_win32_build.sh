#!/bin/sh
# Build checks for src/gui/be_win32.c (no Windows needed).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
SRC=src/gui/be_win32.c

echo "[1/3] plain gcc strict"
gcc -std=c89 -pedantic -Wall -Wextra -Werror -c $SRC -o build/be_win32_c89.o
gcc -std=c2x -Wall -Wextra -Werror -c $SRC -o build/be_win32_c2x.o

COSMO=.toolchain/cosmocc/bin/cosmocc
if [ ! -x "$COSMO" ]; then
    echo "cosmocc not found, skipping cosmo checks"
    exit 0
fi

echo "[2/3] cosmocc compile, zero warnings"
out=$($COSMO -c $SRC -o build/be_win32.o -Wall -Wextra 2>&1) || { echo "$out"; exit 1; }
if [ -n "$out" ]; then echo "$out"; echo "FAIL: warnings"; exit 1; fi

echo "[3/3] cosmocc link + run"
cat > build/win32_probe.c <<'EOP'
#include <stdio.h>
#include "../src/gui/gui.h"
int main(void)
{
    lt_backend *b = lt_backend_win32_create();
    if (!b) { puts("win32 backend unavailable"); return 0; }
    puts("win32 backend available");
    return 0;
}
EOP
# lt_base.c supplies lt_malloc, lt_utf8_next etc.; stub any missing gui symbols
LTSRC="src/lt_base.c"
GUISRC=""
for f in src/gui/font8x16.c src/gui/gfx.c; do [ -f "$f" ] && GUISRC="$GUISRC $f"; done
$COSMO -o build/win32_probe.com build/win32_probe.c $SRC $LTSRC $GUISRC -Wall -Wextra 2>build/win32_probe.log || {
    cat build/win32_probe.log; echo "FAIL: link"; exit 1; }
res=$(./build/win32_probe.com)
echo "$res"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) [ "$res" = "win32 backend unavailable" ] || { echo "FAIL: unexpected output"; exit 1; } ;;
esac
echo "OK"
