#!/bin/sh
# build.sh - portable build driver. Usage: ./build.sh [app|test|clean]
# Env: CC (default cc), CFLAGS (default -O2), LDFLAGS, STRICT=1 (C89 pedantic) | 23 (C2x).
set -e
cd "$(dirname "$0")"

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2}
LDFLAGS=${LDFLAGS:-}
case ${STRICT:-} in
    1)  CFLAGS="$CFLAGS -std=c89 -pedantic -Wall -Wextra -Werror" ;;
    23) CFLAGS="$CFLAGS -std=c2x -Wall -Wextra -Werror" ;;
esac

LIBS="-lm"
case $(uname -s 2>/dev/null) in
    *) ;; # platform layer adds per-OS libs here later (e.g. SunOS: -lsocket -lnsl)
esac

target=${1:-app}
OBJDIR=build/obj

if [ "$target" = clean ]; then
    rm -rf build
    exit 0
fi

mkdir -p "$OBJDIR"
# Rebuild everything when the compiler or flags change.
stamp="$CC $CFLAGS"
if [ ! -f build/.flags ] || [ "$(cat build/.flags)" != "$stamp" ]; then
    rm -rf "$OBJDIR"
    mkdir -p "$OBJDIR"
    printf '%s\n' "$stamp" > build/.flags
fi

OBJS=""
for src in $(find src -name '*.c' ! -path src/main.c | sort); do
    obj="$OBJDIR/${src#src/}.o"
    mkdir -p "$(dirname "$obj")"
    if [ ! -f "$obj" ] || [ -n "$(find src \( -name '*.c' -o -name '*.h' \) -newer "$obj" | head -n 1)" ]; then
        echo "CC $src"
        $CC $CFLAGS -Isrc -c "$src" -o "$obj"
    fi
    OBJS="$OBJS $obj"
done

case $target in
    app)
        if [ -f src/main.c ]; then
            echo "LD build/libretune"
            $CC $CFLAGS -Isrc src/main.c $OBJS -o build/libretune $LDFLAGS $LIBS
        else
            echo "no src/main.c; objects built"
        fi
        ;;
    test)
        mkdir -p build/test
        total=0
        failed=0
        for t in tests/test_*.c; do
            [ -f "$t" ] || continue
            name=$(basename "$t" .c)
            echo "CC $t"
            # tests marked LT_TEST_STANDALONE #include what they need and link alone
            if grep -q LT_TEST_STANDALONE "$t"; then
                $CC $CFLAGS -Isrc -Itests "$t" -o "build/test/$name" $LDFLAGS $LIBS
            else
                $CC $CFLAGS -Isrc -Itests "$t" $OBJS -o "build/test/$name" $LDFLAGS $LIBS
            fi
            total=$((total + 1))
            echo "RUN $name"
            if ! "./build/test/$name"; then
                failed=$((failed + 1))
            fi
        done
        echo "test programs: $total, failed: $failed"
        [ "$failed" -eq 0 ]
        ;;
    *)
        echo "usage: $0 [app|test|clean]" >&2
        exit 2
        ;;
esac
