#!/bin/sh
# tools/bench_present.sh - present-path micro-benchmark of the pixel backends.
#
#   tools/bench_present.sh                  # X11: PutImage, MIT-SHM copy, MIT-SHM zero copy;
#                                           # Win32 under wine when mingw + wine are installed
#   BASE=<git rev> tools/bench_present.sh   # also the backends of that revision
#   SIZES="3200x2000 3840x2160" FRAMES=20 CC=gcc NO_WIN32=1
#
# tests/bench_present.c is linked with a backend and presents a full frame, one 256x64
# rectangle and 24 scattered 128x64 rectangles per frame (see the file for what is timed).
# X11 runs against tests/x11_mock.py in bench mode (no X server needed): 256 KB requests like
# Xorg, and the mock copies every image into a framebuffer like a server does, from the socket
# for PutImage and from the shared segment for ShmPutImage. Win32 runs under wine with the null
# graphics driver (the test prefix of build.sh), so its numbers show the client-side cost only.
# Output: ms per frame, wall clock / client CPU. Linux/BSD host (python3, SysV shared memory).
set -e
cd "$(dirname "$0")/.."
. tools/target.sh
mkdir -p build/bench
CC=${CC:-gcc}
SIZES=${SIZES:-"3200x2000 3840x2160"}
FRAMES=${FRAMES:-20}
PLAT_SRC="src/plat/plat_common.c src/plat/usb_common.c $(lt_platform_files | grep '^src/plat/')"
build() { # output, x11 backend source
    $CC -O2 $PLAT_CPPFLAGS -Isrc/gui -Isrc -o "$1" tests/bench_present.c "$2" \
        src/lt_base.c $PLAT_SRC src/gui/os/$GUIOS/stubs.c tests/x11_stubs.c -lm
}
build build/bench/bench_cur src/gui/be_x11.c
if [ -n "${BASE:-}" ]; then
    git show "$BASE:src/gui/be_x11.c" > build/bench/be_x11_base.c
    build build/bench/bench_base build/bench/be_x11_base.c
fi
S="$PWD/build/bench/x11:0"
run() { # label, binary, mock offers MIT-SHM (0/1), client LT_X11_SHM, size, [zc]
    rm -f "$S"
    LT_X11_MOCK_MODE=bench LT_X11_MOCK_SHM=$3 python3 tests/x11_mock.py "$S" &
    P=$!
    i=0; while [ ! -S "$S" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    w=${5%x*}; h=${5#*x}
    DISPLAY="$S" XAUTHORITY=/nonexistent LT_X11_SHM=$4 timeout 300 "$2" "$w" "$h" "$FRAMES" $6 |
        sed "s/^/$(printf '%-26s' "$1")/"
    wait $P
}
echo "ms per frame, wall / client CPU"
for SZ in $SIZES; do
    if [ -n "${BASE:-}" ]; then run "x11 base PutImage" build/bench/bench_base 0 1 "$SZ"; fi
    run "x11 PutImage (no MIT-SHM)" build/bench/bench_cur 0 1 "$SZ"
    run "x11 MIT-SHM copy" build/bench/bench_cur 1 1 "$SZ"
    run "x11 MIT-SHM zero copy" build/bench/bench_cur 1 1 "$SZ" zc
done

# ---- Win32 under wine ----
command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 && command -v wine >/dev/null 2>&1 || exit 0
[ -z "${NO_WIN32:-}" ] || exit 0
WPLAT=$(ls src/plat/win32/*.c)
wbuild() { # output, win32 backend source
    x86_64-w64-mingw32-gcc -O2 -Isrc/gui/os/win32 -Isrc -o "$1" tests/bench_present.c "$2" \
        src/gui/be_x11.c src/gui/os/win32/stubs.c src/lt_base.c src/plat/plat_common.c \
        src/plat/usb_common.c $WPLAT tests/x11_stubs.c src/gui/font8x16.c \
        -lws2_32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lsetupapi -lwinusb
}
wbuild build/bench/bench_w32.exe src/gui/os/win32/be_win32.c
if [ -n "${BASE:-}" ]; then
    git show "$BASE:src/gui/os/win32/be_win32.c" > build/bench/be_win32_base.c
    wbuild build/bench/bench_w32_base.exe build/bench/be_win32_base.c
fi
WINEPREFIX=${WINEPREFIX:-$PWD/build/wineprefix}
WINEDEBUG=-all
WINEDLLOVERRIDES="mscoree,mshtml="
export WINEPREFIX WINEDEBUG WINEDLLOVERRIDES
if [ ! -f "$WINEPREFIX/.lt_null_gfx" ]; then
    mkdir -p "$WINEPREFIX"
    wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /t REG_SZ /d null /f \
        >/dev/null 2>&1 && : > "$WINEPREFIX/.lt_null_gfx"
fi
wrun() { # label, exe, size, [zc]
    w=${3%x*}; h=${3#*x}
    timeout 300 wine "$2" "$w" "$h" "$FRAMES" win32 $4 2>/dev/null | tr -d '\r' |
        sed "s/^/$(printf '%-26s' "$1")/"
}
for SZ in $SIZES; do
    if [ -n "${BASE:-}" ]; then wrun "win32 base SetDIBits" build/bench/bench_w32_base.exe "$SZ"; fi
    wrun "win32 SetDIBitsToDevice" build/bench/bench_w32.exe "$SZ"
    wrun "win32 DIB section BitBlt" build/bench/bench_w32.exe "$SZ" zc
done
