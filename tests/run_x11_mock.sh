#!/bin/sh
# Build and run test_x11 against the python mock X server (Linux/BSD only).
set -e
cd "$(dirname "$0")/.."
. tools/target.sh
mkdir -p build
S="$PWD/build/x11mock:0"
# self-contained link: x11_stubs.c replaces gfx.c so we do not depend on it
PLAT_SRC="src/plat/plat_common.c src/plat/usb_common.c $(lt_platform_files | grep '^src/plat/')"
${CC:-gcc} -Wall -Wextra $PLAT_CPPFLAGS -Isrc -o build/test_x11 tests/test_x11.c \
    src/gui/be_x11.c src/lt_base.c $PLAT_SRC tests/x11_stubs.c -lm
# desktop: publishes its scale, MIT-SHM works; bare: only RandR knows the panel, no MIT-SHM;
# shmfail: MIT-SHM offered but ShmAttach refused (what a remote server does) -> PutImage;
# shmforeign: the server maps another segment (container with its own IPC namespace) -> PutImage
RC=0
for MODE in desktop bare shmfail shmforeign; do
    rm -f "$S"
    LT_X11_MOCK_MODE=$MODE python3 tests/x11_mock.py "$S" &
    P=$!
    i=0; while [ ! -S "$S" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    set +e
    DISPLAY="$S" XAUTHORITY=/nonexistent LT_X11_MOCK="$S" LT_X11_MOCK_MODE=$MODE \
        LT_SCALE_DEBUG=1 GDK_SCALE= QT_SCALE_FACTOR= timeout 20 build/test_x11
    R=$?
    wait $P
    set -e
    echo "x11 mock ($MODE): exit $R"
    [ "$R" = 0 ] || RC=$R
done
exit $RC
