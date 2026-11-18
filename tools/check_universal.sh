#!/bin/sh
# tools/check_universal.sh - THE portability gate (docs/UNIVERSAL_C.md section 13).
#
#   tools/check_universal.sh [--quick] [--only DIR]... [--no-tests] [--no-static] [-v]
#
# What it runs (a compact table is printed at the end; exit status is nonzero
# if anything failed):
#   compile   every universal .c under src/ and tests/ with gcc AND clang for
#             -std=c89/c99/c11/c17/c2x and the full UNIVERSAL_C flag set
#             (-Wall -Wextra -pedantic-errors -Werror -Wconversion ...).
#             Per-platform files for the host OS get the same flags plus the
#             build's feature-test macros; Windows files are compiled with
#             mingw when installed.
#   audit     tools/audit_names.py (reserved names, banned library calls,
#             translation limits, compiler-macro tests outside src/lt.h)
#   cppcheck  cppcheck --std=c89, only real errors fail
#   tests     ./build.sh test: native gcc, native clang, ASan+UBSan,
#             -funsigned-char, -m32, s390x (qemu, big-endian; fuzz tests skipped
#             there, they are slow when emulated), Windows (wine)
# Options:
#   --quick      gcc+clang c89/c2x only, native gcc tests only
#   --only DIR   restrict compile/audit/cppcheck to files under DIR (repeatable,
#                e.g. --only src/ini --only tests) so parallel agents can check
#                their own slice; tests still run unless --no-tests
#   --no-tests   skip the test matrix        --no-static   skip audit+cppcheck
#   -v           print the first diagnostics of every failure (full log is
#                always written to build/check_universal.<pid>.log)
cd "$(dirname "$0")/.."
. tools/target.sh

QUICK=0; ONLY=""; RUN_TESTS=1; RUN_STATIC=1; VERBOSE=0
while [ $# -gt 0 ]; do
    case $1 in
        --quick) QUICK=1 ;;
        --only) shift; ONLY="$ONLY $(printf '%s' "$1" | sed 's|^\./||; s|/*$||')" ;;
        --no-tests) RUN_TESTS=0 ;;
        --no-static) RUN_STATIC=0 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help) sed -n '2,/^cd /p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

WARN="-Wall -Wextra -pedantic-errors -Werror -Wconversion -Wsign-conversion -Wshadow \
-Wformat=2 -Wcast-align -Wstrict-prototypes -Wmissing-prototypes"
JOBS=${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 1)}
mkdir -p build
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ltcheck.XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM
LOG="build/check_universal.$$.log"
: > "$LOG"
: > "$TMP/fails.txt"
mkdir -p "$TMP/out"

if [ "$QUICK" = 1 ]; then STDS="c89 c2x"; else STDS="c89 c99 c11 c17 c2x"; fi
have() { command -v "$1" >/dev/null 2>&1; }

only_filter() {
    if [ -z "$ONLY" ]; then cat; return; fi
    while read -r p; do
        for o in $ONLY; do
            case $p in "$o"/*|"$o") echo "$p"; break ;; esac
        done
    done
}
is_marked() { grep -q "PLATFORM FILE" "$1"; }   # file documents an OS-header exception

# ---- file sets ---------------------------------------------------------
UNIV=""; HOSTPLAT=""; WINPLAT=""; EXTRAPLAT=""
for f in $(find src tests -name '*.c' | sort | only_filter); do
    case $f in
        tests/os/$PLAT/*) HOSTPLAT="$HOSTPLAT $f" ;;
        tests/os/win32/*) if is_marked "$f"; then WINPLAT="$WINPLAT $f"; fi ;;
        tests/os/*) ;;
        src/plat/win32/*|src/gui/os/win32/*)
            if [ "$PLAT" = win32 ]; then HOSTPLAT="$HOSTPLAT $f"
            elif is_marked "$f"; then WINPLAT="$WINPLAT $f"
            else UNIV="$UNIV $f"; fi ;;
        src/plat/posix/*|src/gui/os/*)
            case " $(lt_platform_files | tr '\n' ' ') " in
                *" $f "*) HOSTPLAT="$HOSTPLAT $f" ;;
                *) if is_marked "$f"; then
                       # other POSIX flavours that only need POSIX headers
                       case $f in
                           src/plat/posix/os_bsd.c) EXTRAPLAT="$EXTRAPLAT $f" ;;
                           # be_cocoa.c declares the Objective-C runtime by hand (no Apple
                           # headers), so every host compiler can check it
                           src/gui/os/macos/be_cocoa.c) EXTRAPLAT="$EXTRAPLAT $f" ;;
                       esac
                   else UNIV="$UNIV $f"; fi ;;
            esac ;;
        *) UNIV="$UNIV $f" ;;
    esac
done

# ---- compile matrix ------------------------------------------------------
cat > "$TMP/job.sh" <<'JOB'
#!/bin/sh
# args: id compiler std kind file
id=$1; cc=$2; std=$3; kind=$4; f=$5
extra=""
case $kind in plat) extra="$LT_PLAT_CPPFLAGS" ;; esac
if out=$($cc -std="$std" $LT_WARN $extra -Isrc -Itests -c -o /dev/null "$f" 2>&1); then
    :
else
    printf '%s %s %s %s\n' "$cc" "$std" "$kind" "$f" >> "$LT_TMP/fails.txt"
    { printf '=== %s -std=%s %s\n' "$cc" "$std" "$f"; printf '%s\n' "$out"; } > "$LT_TMP/out/$id"
fi
JOB
chmod +x "$TMP/job.sh"
LT_WARN="$WARN"; LT_PLAT_CPPFLAGS="$PLAT_CPPFLAGS"; LT_TMP="$TMP"
export LT_WARN LT_PLAT_CPPFLAGS LT_TMP

: > "$TMP/jobs.txt"
n=0
add_jobs() { # cc kind files...
    cc=$1; kind=$2; shift 2
    for f in "$@"; do
        for s in $STDS; do
            n=$((n + 1))
            echo "$n $cc $s $kind $f" >> "$TMP/jobs.txt"
        done
    done
}
HOSTCCS="gcc clang"
for cc in $HOSTCCS; do
    have "$cc" || continue
    add_jobs "$cc" univ $UNIV
    add_jobs "$cc" plat $HOSTPLAT $EXTRAPLAT
done
MINGW=""
if [ "$TARGET" != windows ] && have x86_64-w64-mingw32-gcc && [ -n "$WINPLAT" ]; then
    MINGW=x86_64-w64-mingw32-gcc
    add_jobs "$MINGW" univ $WINPLAT
    # universal files too: Windows is LLP64 (32-bit long), which gcc/clang on
    # Linux (LP64) cannot catch -- e.g. 4294967296L does not fit in a long.
    add_jobs "$MINGW" univ $UNIV
fi
nuniv=$(echo $UNIV | wc -w); nplat=$(echo $HOSTPLAT $EXTRAPLAT | wc -w)
echo "compile: $nuniv universal + $nplat platform + $(echo $WINPLAT | wc -w) windows files, $(wc -l < "$TMP/jobs.txt") jobs on $JOBS cpus"
xargs -P "$JOBS" -L 1 "$TMP/job.sh" < "$TMP/jobs.txt"
cat "$TMP/out"/* >> "$LOG" 2>/dev/null

count_cell() { # compiler std -> number of failing files
    awk -v c="$1" -v s="$2" '$1==c && $2==s {print $4}' "$TMP/fails.txt" | sort -u | wc -l | tr -d ' '
}

# ---- static analysis -----------------------------------------------------
AUDIT_RC=-1; AUDIT_N=""; CPP_RC=-1; CPP_N=""
if [ "$RUN_STATIC" = 1 ]; then
    if have python3; then
        set --; for o in $ONLY; do set -- "$@" --only "$o"; done
        python3 tools/audit_names.py "$@" > "$TMP/audit.txt" 2>&1
        AUDIT_RC=$?
        { echo "=== audit_names.py"; cat "$TMP/audit.txt"; } >> "$LOG"
        AUDIT_N=$(grep -c '^[a-z].*:[0-9]*: ' "$TMP/audit.txt" | tr -d ' ')
    fi
    if have cppcheck; then
        cpp_targets=""
        for d in src tests; do
            if [ -z "$ONLY" ]; then cpp_targets="$cpp_targets $d"; continue; fi
            for o in $ONLY; do
                case $o in "$d"|"$d"/*) cpp_targets="$cpp_targets $o" ;; esac
            done
        done
        if [ -n "$cpp_targets" ]; then
            # only real errors fail the gate; style/performance noise is not enabled
            cppcheck --std=c89 --error-exitcode=1 --quiet --inline-suppr \
                --template='{file}:{line}: [{id}] {message}' \
                --enable=warning,portability --suppress=missingIncludeSystem \
                --suppress=unusedFunction --suppress=checkersReport \
                --suppress=nullPointerRedundantCheck --suppress=nullPointerOutOfMemory \
                --suppress=ctunullpointer --suppress=ctunullpointerOutOfMemory \
                --suppress=localtimeCalled --suppress=gmtimeCalled \
                --library=posix $PLAT_CPPFLAGS -Isrc -Itests \
                -i src/plat/win32 -i src/gui/os/win32 -i src/gui/os/macos \
                $cpp_targets > "$TMP/cpp.txt" 2>&1
            CPP_RC=$?
            { echo "=== cppcheck"; cat "$TMP/cpp.txt"; } >> "$LOG"
            CPP_N=$(grep -c '^[a-z][^ ]*:[0-9]*: \[' "$TMP/cpp.txt" | tr -d ' ')
        fi
    fi
fi

# ---- test matrix -----------------------------------------------------------
: > "$TMP/tests.txt"
run_tests() { # label, then VAR=value pairs
    label=$1; shift
    printf 'tests: %s ... ' "$label" >&2
    if env "$@" JOBS="$JOBS" ./build.sh test > "$TMP/t.out" 2>&1; then rc=0; else rc=1; fi
    { echo "=== tests $label"; cat "$TMP/t.out"; } >> "$LOG"
    summary=$(grep '^test programs:' "$TMP/t.out" | tail -n 1)
    [ -n "$summary" ] || summary="build failed"
    if [ "$rc" = 0 ]; then echo "PASS ($summary)" >&2; else echo "FAIL ($summary)" >&2; fi
    printf '%s|%s|%s\n' "$label" "$rc" "$summary" >> "$TMP/tests.txt"
}
skip_tests() { printf 'tests: %s ... SKIP (%s)\n' "$1" "$2" >&2; printf '%s|skip|%s\n' "$1" "$2" >> "$TMP/tests.txt"; }

# can the host compile+link+run with these extra flags? (probe in $TMP)
probe() { # compiler flags... ; runner in $RUNNER_PROBE
    cc=$1; shift
    printf '#include <errno.h>\n#include <stdio.h>\nint main(void){puts("ok");return 0;}\n' > "$TMP/p.c"
    $cc "$@" -o "$TMP/p.bin" "$TMP/p.c" >/dev/null 2>&1 && ${RUNNER_PROBE:-} "$TMP/p.bin" >/dev/null 2>&1
}

if [ "$RUN_TESTS" = 1 ]; then
    CCNATIVE=gcc; have gcc || CCNATIVE=cc
    run_tests "native $CCNATIVE" CC="$CCNATIVE" TARGET="$TARGET"
    # the X11 backend against the mock X server (protocol, scale detection, RandR)
    case $TARGET in
        linux|bsd)
            if have python3; then
                printf 'tests: X11 mock server ... ' >&2
                if CC="$CCNATIVE" tests/run_x11_mock.sh > "$TMP/x11.out" 2>&1; then rc=0; else rc=1; fi
                { echo "=== tests X11 mock server"; cat "$TMP/x11.out"; } >> "$LOG"
                if [ "$rc" = 0 ]; then echo "PASS" >&2; else echo "FAIL" >&2; fi
                printf '%s|%s|%s\n' "X11 mock server" "$rc" "desktop + bare (RandR) modes" >> "$TMP/tests.txt"
            else skip_tests "X11 mock server" "python3 not installed"; fi ;;
    esac
    if [ "$QUICK" = 0 ]; then
        if have clang; then run_tests "native clang" CC=clang TARGET="$TARGET"
        else skip_tests "native clang" "clang not installed"; fi
        SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
        if probe "$CCNATIVE" $SAN; then
            run_tests "ASan+UBSan" CC="$CCNATIVE" TARGET="$TARGET" \
                CFLAGS="-O1 -g -fno-omit-frame-pointer $SAN" LDFLAGS="$SAN" \
                ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
        else skip_tests "ASan+UBSan" "sanitizers unavailable"; fi
        run_tests "unsigned char" CC="$CCNATIVE" TARGET="$TARGET" CFLAGS="-O2 -funsigned-char"
        M32="-m32"
        [ -d /usr/include/x86_64-linux-gnu/asm ] && [ ! -d /usr/include/asm ] && \
            M32="-m32 -idirafter /usr/include/x86_64-linux-gnu"
        if probe "$CCNATIVE" $M32; then
            run_tests "32-bit (-m32)" CC="$CCNATIVE" TARGET="$TARGET" CFLAGS="-O2 $M32" LDFLAGS="$M32"
        else skip_tests "32-bit (-m32)" "no 32-bit toolchain"; fi
        if have s390x-linux-gnu-gcc && have qemu-s390x && \
           RUNNER_PROBE=qemu-s390x probe s390x-linux-gnu-gcc -static; then
            # fuzz tests only mutate bytes (nothing endian-specific) and are ~50x slower emulated
            SLOW=$(cd tests && ls test_*fuzz*.c | sed 's/\.c$//' | tr '\n' ' ')
            run_tests "s390x big-endian (qemu)" TARGET="$TARGET" CC=s390x-linux-gnu-gcc \
                LDFLAGS=-static RUNNER=qemu-s390x SKIP_TESTS="$SLOW"
        else skip_tests "s390x big-endian (qemu)" "s390x gcc/qemu not installed"; fi
        if [ "$TARGET" != windows ] && have x86_64-w64-mingw32-gcc && have wine; then
            run_tests "Windows LLP64 (wine)" TARGET=windows CC=x86_64-w64-mingw32-gcc \
                WINEDEBUG=-all
        elif [ "$TARGET" = windows ]; then skip_tests "Windows LLP64 (wine)" "host is windows"
        else skip_tests "Windows LLP64 (wine)" "mingw/wine not installed"; fi
    fi
fi

# ---- summary -----------------------------------------------------------------
fail=0
echo
echo "== compile matrix: files failing (0 = clean) =="
printf '%-26s' ""
for s in $STDS; do printf '%6s' "$s"; done
echo
for cc in $HOSTCCS $MINGW; do
    have "$cc" || continue
    label=$cc
    [ "$cc" = "$MINGW" ] && label="mingw-gcc (LLP64)"
    printf '%-26s' "$label"
    for s in $STDS; do
        c=$(count_cell "$cc" "$s")
        printf '%6s' "$c"
        [ "$c" != 0 ] && fail=1
    done
    echo
done
if [ -s "$TMP/fails.txt" ]; then
    echo
    echo "failing files by directory (any compiler/standard):"
    awk '{print $4}' "$TMP/fails.txt" | sort -u | sed 's|/[^/]*$||' | sort | uniq -c | \
        awk '{printf "  %4d  %s\n", $1, $2}'
    echo "first diagnostics (full log: $LOG):"
    if [ "$VERBOSE" = 1 ]; then lim=400; else lim=24; fi
    grep -hE '^[^ ]+:[0-9]+:[0-9]+: (error|warning)' "$TMP/out"/* 2>/dev/null | sort -u | head -n "$lim" | sed 's/^/  /'
fi

echo
echo "== other checks =="
report() { # name rc count
    case $2 in
        -1) printf '  %-34s SKIP (tool missing or disabled)\n' "$1" ;;
        0) printf '  %-34s PASS\n' "$1" ;;
        *) printf '  %-34s FAIL (%s findings)\n' "$1" "$3"; fail=1 ;;
    esac
}
report "audit_names.py" "$AUDIT_RC" "$AUDIT_N"
report "cppcheck --std=c89" "$CPP_RC" "$CPP_N"
if [ "$RUN_TESTS" = 1 ]; then
    while IFS='|' read -r label rc summary; do
        case $rc in
            0) printf '  tests %-28s PASS  %s\n' "$label" "$summary" ;;
            skip) printf '  tests %-28s SKIP  %s\n' "$label" "$summary" ;;
            *) printf '  tests %-28s FAIL  %s\n' "$label" "$summary"; fail=1 ;;
        esac
    done < "$TMP/tests.txt"
fi
echo
if [ "$fail" = 0 ]; then echo "check_universal: PASS"; else echo "check_universal: FAIL (log: $LOG)"; fi
[ "$fail" = 0 ] && rm -f "$LOG"
exit "$fail"
