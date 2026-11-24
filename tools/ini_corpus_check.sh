#!/bin/sh
# tools/ini_corpus_check.sh - run real-world INIs through the parser and the whole app.
#
#   tools/ini_corpus_check.sh [options] [FILE.ini|DIR]...     (default: build/ini_corpus)
#
# For every INI:
#   1. parse it with an optimized build and time it (flagged SLOW above 2 s)
#   2. start the app headless in demo mode with it under an ASan+UBSan build
#      (LT_SCREENSHOT=x.ppm build/... --demo --ini FILE)
#   3. the same once per [Menu] target (LT_SHOT_OPEN=<target>), targets listed by the parser
#      exactly as the app resolves them (preprocessor applied, submenus followed)
# Any sanitizer report, crash, nonzero exit or timeout is a FAIL; its log is kept.
# Options:
#   --jobs N        parallel app runs (default: CPUs)
#   --targets N     at most N menu targets per INI (default: all)
#   --timeout S     per app run (default 180)
#   --no-build      reuse build/ini_check/clibretune-asan and ini_check_parse
#   --no-app        parse/timing only
# Output: build/ini_check/results.txt (one line per run: STATUS seconds ini target),
#         build/ini_check/parse.txt, logs of failures in build/ini_check/logs/.
# The ASan build goes to its own object directory; build/clibretune is rebuilt normally after.
set -e
cd "$(dirname "$0")/.."

if [ "${1:-}" = --worker ]; then
    # one app run: $2 = timeout, $3 = ini, $4 = target ("-" = none)
    to=$2; ini=$3; tg=$4
    D=build/ini_check
    key=$(printf '%s|%s' "$ini" "$tg" | cksum | cut -d' ' -f1)
    log=$D/logs/$key.log
    wd=$D/run/$key
    rm -rf "$wd" && mkdir -p "$wd/home" "$wd/cfg" "$wd/proj"
    t0=$(date +%s)
    set +e
    if [ "$tg" = - ]; then
        env HOME="$PWD/$wd/home" CLIBRETUNE_CONFIG_DIR="$PWD/$wd/cfg" CLIBRETUNE_PROJECTS="$PWD/$wd/proj" \
            LT_SCREENSHOT="$wd/x.ppm" LT_SHOT_FRAMES="${LT_SHOT_FRAMES:-4}" \
            timeout -k 5 "$to" "$D/clibretune-asan" --demo --ini "$ini" > "$log" 2>&1
    else
        env HOME="$PWD/$wd/home" CLIBRETUNE_CONFIG_DIR="$PWD/$wd/cfg" CLIBRETUNE_PROJECTS="$PWD/$wd/proj" \
            LT_SCREENSHOT="$wd/x.ppm" LT_SHOT_FRAMES="${LT_SHOT_FRAMES:-4}" LT_SHOT_OPEN="$tg" \
            timeout -k 5 "$to" "$D/clibretune-asan" --demo --ini "$ini" > "$log" 2>&1
    fi
    rc=$?
    set -e
    dt=$(( $(date +%s) - t0 ))
    st=OK
    if [ $rc = 124 ] || [ $rc = 137 ]; then st=TIMEOUT
    elif grep -q 'Sanitizer\|runtime error:' "$log"; then st=SANITIZER
    elif [ $rc != 0 ]; then st="EXIT$rc"
    elif [ ! -s "$wd/x.ppm" ]; then st=NOSHOT
    fi
    if [ $st = OK ]; then rm -f "$log"; fi
    rm -rf "$wd"
    printf '%s\t%s\t%s\t%s\t%s\n' "$st" "$dt" "$ini" "$tg" "$key"
    exit 0
fi

JOBS=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 1)
MAXT=0; TO=180; BUILD=1; APP=1
while [ $# -gt 0 ]; do
    case $1 in
        --jobs) shift; JOBS=$1 ;;
        --targets) shift; MAXT=$1 ;;
        --timeout) shift; TO=$1 ;;
        --no-build) BUILD=0 ;;
        --no-app) APP=0 ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
    shift
done
[ $# -gt 0 ] || set -- build/ini_corpus
D=build/ini_check
mkdir -p "$D/logs" "$D/run"
find "$@" -name '*.ini' -type f | sort > "$D/inis.txt"
[ -s "$D/inis.txt" ] || { echo "no INIs (run tools/fetch_ini_corpus.sh first)" >&2; exit 2; }
echo "ini_corpus_check: $(wc -l < "$D/inis.txt" | tr -d ' ') INIs"

if [ "$BUILD" = 1 ]; then
    . tools/target.sh
    echo "building the parse helper (optimized)"
    srcs=""
    for s in $(lt_sources); do srcs="$srcs $s"; done
    ${CC:-cc} -O2 $PLAT_CPPFLAGS -Isrc tools/ini_check_parse.c $srcs -o "$D/ini_check_parse" $TARGET_LIBS
    if [ "$APP" = 1 ]; then
        echo "building the ASan+UBSan app"
        SANF="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
        CFLAGS="$SANF" LDFLAGS="$SANF" ./build.sh app > "$D/build.log" 2>&1 ||
            { tail -n 30 "$D/build.log"; exit 1; }
        cp build/clibretune "$D/clibretune-asan"
        ./build.sh app >> "$D/build.log" 2>&1 || { tail -n 30 "$D/build.log"; exit 1; }
    fi
fi

# ---- 1. parse + timing, menu targets
: > "$D/parse.txt"
: > "$D/targets.txt"
while read -r f; do
    "$D/ini_check_parse" "$f" > "$D/one.txt" 2>&1 || echo "E	$f	parser crashed ($?)" >> "$D/one.txt"
    grep '^[PE]	' "$D/one.txt" >> "$D/parse.txt" || true
    if [ "$MAXT" -gt 0 ]; then
        grep '^T	' "$D/one.txt" | head -n "$MAXT" >> "$D/targets.txt" || true
    else
        grep '^T	' "$D/one.txt" >> "$D/targets.txt" || true
    fi
done < "$D/inis.txt"
rm -f "$D/one.txt"
awk -F'\t' '$1 == "P" && $2 > 2000 { print "SLOW parse " $2 " ms: " $3 }' "$D/parse.txt"
awk -F'\t' '$1 == "E" { print "PARSE ERROR " $2 ": " $3 }' "$D/parse.txt"
awk -F'\t' '$1 == "P" { n++; s += $2; if ($2 > m) { m = $2; mf = $3 } }
    END { if (n) printf "parsed %d INIs, mean %.0f ms, max %d ms (%s)\n", n, s / n, m, mf }' "$D/parse.txt"
echo "menu targets: $(wc -l < "$D/targets.txt" | tr -d ' ')"
[ "$APP" = 1 ] || exit 0

# ---- 2+3. app runs
{ awk -F'\t' '$1 == "P" { print $3 "\t-" }' "$D/parse.txt"
  awk -F'\t' '{ print $2 "\t" $3 }' "$D/targets.txt"; } > "$D/jobs.txt"
echo "app runs: $(wc -l < "$D/jobs.txt" | tr -d ' ') with $JOBS jobs"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
tr '\t' '\n' < "$D/jobs.txt" | tr '\n' '\0' |
    xargs -0 -n 2 -P "$JOBS" sh "$0" --worker "$TO" > "$D/results.txt" 2>/dev/null || true
awk -F'\t' '{ c[$1]++ } END { for (k in c) printf "%s %d\n", k, c[k] }' "$D/results.txt" | sort
awk -F'\t' '$1 != "OK" { print $1 "\t" $3 "\t" $4 "\tlog: build/ini_check/logs/" $5 ".log" }' \
    "$D/results.txt" | sort | head -n 100
awk -F'\t' '$2 > 60 { print "SLOW run " $2 " s: " $3 " " $4 }' "$D/results.txt" | head -n 20
! grep -qv '^OK	' "$D/results.txt"
