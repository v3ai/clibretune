#!/bin/sh
# build.sh - portable POSIX-sh build driver.
#
#   ./build.sh [app|test|clean|list]
#
# Env:
#   TARGET   linux | macos | windows | bsd   (default: detected from uname)
#   CC       compiler (default cc); e.g. CC=x86_64-w64-mingw32-gcc TARGET=windows
#   CFLAGS   default -O2;  LDFLAGS extra link flags
#   LT_VERSION  version the binary reports (release builds), e.g. 0.1.3 or v0.1.3
#   STRICT   1 = -std=c89 + the full UNIVERSAL_C warning set, 23 = same with -std=c2x
#   RUNNER   prefix for running test binaries (e.g. RUNNER=qemu-s390x);
#            defaults to wine when TARGET=windows is cross-built on a non-Windows host
#   JOBS     parallel compile jobs (default: number of CPUs)
#   TESTS      only build/run these test programs, e.g. TESTS="test_dash test_ini_parse"
#   SKIP_TESTS do not build/run these test programs (slow ones under qemu, ...)
# Cross examples:
#   TARGET=windows CC=x86_64-w64-mingw32-gcc ./build.sh test
#   CC=s390x-linux-gnu-gcc LDFLAGS=-static RUNNER=qemu-s390x ./build.sh test
# Output: build/clibretune[.exe]; per-configuration objects in build/obj-*/.
set -e
cd "$(dirname "$0")"
. tools/target.sh

WARN="-Wall -Wextra -pedantic-errors -Werror -Wconversion -Wsign-conversion -Wshadow \
-Wformat=2 -Wcast-align -Wstrict-prototypes -Wmissing-prototypes"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2}
if [ -n "${LT_VERSION:-}" ]; then
    LT_VERSION=${LT_VERSION#v}
    case $LT_VERSION in
        *[!0-9A-Za-z.+-]*|'') echo "build.sh: bad LT_VERSION '$LT_VERSION'" >&2; exit 1 ;;
    esac
    CFLAGS="$CFLAGS -DLT_APP_VERSION=\"$LT_VERSION\""
fi
LDFLAGS=${LDFLAGS:-}
case ${STRICT:-} in
    1)  CFLAGS="$CFLAGS -std=c89 $WARN" ;;
    23) CFLAGS="$CFLAGS -std=c2x $WARN" ;;
esac
LIBS="$TARGET_LIBS"

target=${1:-app}
if [ "$target" = clean ]; then
    rm -rf build/obj-* build/clibretune build/clibretune.exe
    exit 0
fi
if [ "$target" = list ]; then
    lt_sources
    exit 0
fi

if [ -z "${RUNNER:-}" ] && [ "$TARGET" = windows ] && [ "$(lt_host_target)" != windows ]; then
    RUNNER=wine
    WINEDEBUG=${WINEDEBUG:--all}
    export WINEDEBUG
    # Private wine prefix with the null graphics driver: window creation works
    # (so the Win32 backend test really runs) with or without a display, never
    # pops windows up, and the user's own ~/.wine is left alone.
    if [ -z "${WINEPREFIX:-}" ]; then
        WINEPREFIX="$PWD/build/wineprefix"
        WINEDLLOVERRIDES="mscoree,mshtml="
        export WINEPREFIX WINEDLLOVERRIDES
        if [ ! -f "$WINEPREFIX/.lt_null_gfx" ]; then
            echo "wine: creating test prefix $WINEPREFIX"
            mkdir -p "$WINEPREFIX"
            wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /t REG_SZ /d null /f \
                >/dev/null 2>&1 && : > "$WINEPREFIX/.lt_null_gfx"
        fi
    fi
fi
RUNNER=${RUNNER:-}

JOBS=${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 1)}
stamp="$TARGET $CC $CFLAGS $PLAT_CPPFLAGS"
OBJDIR="build/obj-$TARGET-$(printf '%s' "$stamp" | cksum | cut -d' ' -f1)"
mkdir -p "$OBJDIR/test"
rm -f "$OBJDIR/.fail"

# $1 = output object, $2 = source; platform files get the feature-test flags.
compile_one() {
    cppf=""
    if lt_is_platform_file "$2"; then cppf="$PLAT_CPPFLAGS"; fi
    echo "CC $2"
    # write to a private name first so concurrent builds never see partial objects
    if $CC $CFLAGS $cppf -Isrc -c "$2" -o "$1.$$.tmp"; then
        mv -f "$1.$$.tmp" "$1"
    else
        rm -f "$1.$$.tmp"
        : > "$OBJDIR/.fail"
    fi
}

running=0
OBJS=""
for src in $(lt_sources); do
    obj="$OBJDIR/$(printf '%s' "${src#src/}" | tr '/' '_').o"
    OBJS="$OBJS $obj"
    if [ -f "$obj" ] && [ -z "$(find src \( -name '*.c' -o -name '*.h' \) -newer "$obj" | head -n 1)" ]; then
        continue
    fi
    compile_one "$obj" "$src" &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then
        wait
        running=0
    fi
done
wait
[ ! -f "$OBJDIR/.fail" ] || { echo "build failed" >&2; exit 1; }

case $target in
    app)
        echo "LD build/clibretune$EXE"
        mkdir -p build
        # main.c is compiled here on every build, so it carries the build id (git commit)
        BUILD_ID=$(git rev-parse --short HEAD 2>/dev/null || echo dev)
        if [ "$BUILD_ID" != dev ] && [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
            BUILD_ID="$BUILD_ID+local"
        fi
        $CC $CFLAGS "-DLT_BUILD_ID=\"$BUILD_ID\"" -Isrc src/main.c $OBJS \
            -o "build/clibretune$EXE.$$.tmp" $LDFLAGS $LIBS
        mv -f "build/clibretune$EXE.$$.tmp" "build/clibretune$EXE"
        ;;
    test)
        tests=""
        for t in tests/test_*.c tests/os/$PLAT/test_*.c; do
            [ -f "$t" ] || continue
            name=$(basename "$t" .c)
            case " ${TESTS:-$name} " in *" $name "*) ;; *) continue ;; esac
            case " ${SKIP_TESTS:-} " in *" $name "*) continue ;; esac
            tests="$tests $t"
        done
        # compile in parallel, run sequentially (tests share ports and temp dirs)
        running=0
        for t in $tests; do
            name=$(basename "$t" .c)
            cppf=""
            if lt_is_platform_file "$t"; then cppf="$PLAT_CPPFLAGS"; fi
            rm -f "$OBJDIR/test/$name$EXE"
            if grep -q LT_TEST_STANDALONE "$t"; then
                objs=""
            else
                objs="$OBJS"
            fi
            (
                echo "CC $t"
                $CC $CFLAGS $cppf -Isrc -Itests "$t" $objs -o "$OBJDIR/test/$name$EXE" $LDFLAGS $LIBS \
                    || : > "$OBJDIR/.fail"
            ) &
            running=$((running + 1))
            if [ "$running" -ge "$JOBS" ]; then
                wait
                running=0
            fi
        done
        wait
        [ ! -f "$OBJDIR/.fail" ] || { echo "test build failed" >&2; exit 1; }
        total=0
        failed=0
        for t in $tests; do
            name=$(basename "$t" .c)
            total=$((total + 1))
            echo "RUN $name"
            # private config/home/projects per test program: tests never touch the
            # user's real settings, and concurrent runs cannot interfere. (Not for
            # Windows targets: wine finds its prefix through HOME.)
            # relative paths work for native and wine-run binaries alike
            tdir="$OBJDIR/test/env-$name"
            rm -rf "$tdir" && mkdir -p "$tdir/home" "$tdir/projects"
            tenv="env CLIBRETUNE_CONFIG_DIR=$tdir/config CLIBRETUNE_PROJECTS=$tdir/projects"
            if [ "$TARGET" != windows ]; then
                tenv="$tenv HOME=$PWD/$tdir/home XDG_CONFIG_HOME=$PWD/$tdir/home/.config"
            fi
            if ! $tenv $RUNNER "./$OBJDIR/test/$name$EXE"; then
                failed=$((failed + 1))
                echo "FAILED: $name"
            fi
        done
        # Windows serial under wine needs a pty peer: a helper drives it.
        if [ "$RUNNER" = wine ] && [ -f "$OBJDIR/test/test_win32_serial$EXE" ] &&
           [ -f tests/os/win32/serial_wine.sh ]; then
            total=$((total + 1))
            echo "RUN test_win32_serial (pty peer under wine)"
            if ! sh tests/os/win32/serial_wine.sh "$OBJDIR/test/test_win32_serial$EXE"; then
                failed=$((failed + 1))
                echo "FAILED: test_win32_serial (pty peer)"
            fi
        fi
        echo "test programs: $total, failed: $failed"
        [ "$failed" -eq 0 ]
        ;;
    *)
        echo "usage: $0 [app|test|clean|list]" >&2
        exit 2
        ;;
esac
