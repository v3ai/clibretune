#!/bin/sh
# Build and run test_x11 against the python mock X server.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
S="$PWD/build/x11mock:0"
GFX=""
GFX="tests/x11_stubs.c" # self-contained: avoids depending on gfx.c link deps
${CC:-gcc} -Wall -Wextra -o build/test_x11 tests/test_x11.c src/gui/be_x11.c \
    src/lt_base.c src/plat/plat_posix.c $GFX -lm
python3 tests/x11_mock.py "$S" &
P=$!
i=0; while [ ! -S "$S" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
set +e
DISPLAY="$S" XAUTHORITY=/nonexistent LT_X11_MOCK="$S" timeout 20 build/test_x11
R=$?
wait $P
exit $R
