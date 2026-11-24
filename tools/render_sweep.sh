#!/bin/sh
# tools/render_sweep.sh - check retained rendering on every screen of the demo.
#
#   tools/render_sweep.sh [options]
#
# Opens each screen headless in demo mode (the dashboard, every [Menu] target of the demo INI as
# the parser lists it, every dialog/tab kind registered in src/app as LT_SHOT_OPEN=dlg:<kind>, the
# 3D table view), runs LT_SHOT_FRAMES frames followed by a mouse sweep across the window
# (LT_SHOT_SCRIPT, one frame per step) with LT_RENDER_VERIFY=1: every frame's tile-rendered
# canvas is compared with a full render of the same frame, and a difference exits with status 3.
# With --base BIN the final screenshot is also compared byte for byte with what BIN (an earlier
# build, e.g. one with immediate rendering) produces for the same screen.
# Options:
#   --bin PATH      app binary (default build/libretune)
#   --base PATH     reference binary for the screenshot comparison (default: none)
#   --sizes LIST    "WxH@PCT ..." (default "1600x1000@100 3200x2000@200")
#   --jobs N        parallel runs (default: CPUs)
#   --limit N       only the first N screens (quick check)
#   --ini FILE      demo definition (default tests/fixtures/ini/demo.ini; its [Menu] targets are
#                   the screens), e.g. the bundled rusEFI one the app writes to <config>/demo.ini
# Output: one line per failure, then the totals; build/render_sweep/ keeps logs and images of
# failures. Exit status 1 when anything failed.
set -e
cd "$(dirname "$0")/.."

if [ "${1:-}" = --worker ]; then
    # $2 = bin, $3 = base ("-" none), $4 = size, $5 = screen
    bin=$2; base=$3; size=$4; scr=$5
    wh=${size%@*}; pct=${size#*@}
    D=build/render_sweep
    key=$(printf '%s|%s' "$size" "$scr" | cksum | cut -d' ' -f1)
    wd=$D/run/$key
    rm -rf "$wd" && mkdir -p "$wd/home" "$wd/cfg" "$wd/proj"
    open=$scr; view=
    case $scr in
        -) open= ;;
        3d:*) open=${scr#3d:}; view=3d ;;
    esac
    w=${wh%x*}; h=${wh#*x}
    scrpt=$(awk -v w="$w" -v h="$h" 'BEGIN { for (i = 1; i <= 6; i++) printf "m:%d/%d ", w * i / 7, h * i / 7 }')
    st=OK
    set +e
    env HOME="$PWD/$wd/home" LIBRETUNE_CONFIG_DIR="$PWD/$wd/cfg" LIBRETUNE_PROJECTS="$PWD/$wd/proj" \
        LT_RENDER_VERIFY=1 LT_SCALE="$pct" LT_SCREENSHOT="$wd/new.ppm" LT_SHOT_SIZE="$wh" \
        LT_SHOT_FRAMES=6 LT_SHOT_OPEN="$open" LT_TABLE_VIEW="$view" LT_SHOT_SCRIPT="$scrpt" \
        timeout -k 5 300 "$bin" --demo > "$wd/log.txt" 2>&1
    rc=$?
    set -e
    if [ $rc = 3 ]; then st=VERIFY
    elif [ $rc != 0 ]; then st="EXIT$rc"
    elif [ ! -s "$wd/new.ppm" ]; then st=NOSHOT
    fi
    if [ $st = OK ] && [ "$base" != - ]; then
        rm -rf "$wd/cfg" "$wd/home" "$wd/proj" && mkdir -p "$wd/home" "$wd/cfg" "$wd/proj"
        env HOME="$PWD/$wd/home" LIBRETUNE_CONFIG_DIR="$PWD/$wd/cfg" LIBRETUNE_PROJECTS="$PWD/$wd/proj" \
            LT_SCALE="$pct" LT_SCREENSHOT="$wd/base.ppm" LT_SHOT_SIZE="$wh" \
            LT_SHOT_FRAMES=6 LT_SHOT_OPEN="$open" LT_TABLE_VIEW="$view" LT_SHOT_SCRIPT="$scrpt" \
            timeout -k 5 300 "$base" --demo > "$wd/base_log.txt" 2>&1 || true
        cmp -s "$wd/new.ppm" "$wd/base.ppm" || st=DIFF
    fi
    if [ $st = OK ]; then
        rm -rf "$wd"
    else
        mkdir -p "$D/fail" && rm -rf "$D/fail/$key" && mv "$wd" "$D/fail/$key"
    fi
    printf '%s\t%s\t%s\t%s\n' "$st" "$size" "$scr" "$key"
    exit 0
fi

BIN=build/libretune
BASE=-
SIZES="1600x1000@100 3200x2000@200"
JOBS=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 1)
LIMIT=0
INI=tests/fixtures/ini/demo.ini
while [ $# -gt 0 ]; do
    case $1 in
        --bin) shift; BIN=$1 ;;
        --base) shift; BASE=$1 ;;
        --sizes) shift; SIZES=$1 ;;
        --jobs) shift; JOBS=$1 ;;
        --limit) shift; LIMIT=$1 ;;
        --ini) shift; INI=$1 ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -x "$BIN" ] || { echo "no binary $BIN (./build.sh app)" >&2; exit 2; }
[ -f "$INI" ] || { echo "no INI $INI" >&2; exit 2; }
case $INI in /*) ;; *) INI=$PWD/$INI ;; esac
LT_DEMO_INI=$INI
export LT_DEMO_INI
D=build/render_sweep
rm -rf "$D" && mkdir -p "$D/run"

# screens: dashboard, menu targets (as the parser lists them), dialog and tab kinds, 3D view
. tools/target.sh
srcs=""
for s in $(lt_sources); do case $s in src/main.c) ;; *) srcs="$srcs $s" ;; esac; done
${CC:-cc} -O1 $PLAT_CPPFLAGS -Isrc tools/ini_check_parse.c $srcs -o "$D/ini_check_parse" $TARGET_LIBS
{
    echo -
    "$D/ini_check_parse" "$INI" | awk -F'\t' '$1 == "T" { print $3 }'
    grep -rhoE '\.kind = "[^"]+"' src/app | sed 's/.*"\(.*\)"/dlg:\1/' | sort -u
    echo 3d:veTableTbl
} | awk '!seen[$0]++' > "$D/screens.txt"
if [ "$LIMIT" -gt 0 ]; then head -n "$LIMIT" "$D/screens.txt" > "$D/s.txt" && mv "$D/s.txt" "$D/screens.txt"; fi
for z in $SIZES; do
    awk -v z="$z" -v b="$BIN" -v r="$BASE" '{ print b "\t" r "\t" z "\t" $0 }' "$D/screens.txt"
done > "$D/jobs.txt"
echo "render_sweep: $(wc -l < "$D/screens.txt" | tr -d ' ') screens x $(echo $SIZES | wc -w | tr -d ' ') sizes, $JOBS jobs"
tr '\t' '\n' < "$D/jobs.txt" | xargs -d '\n' -n 4 -P "$JOBS" "$0" --worker > "$D/results.txt"
awk -F'\t' '$1 != "OK" { print $1 "\t" $2 "\t" $3 "\tbuild/render_sweep/fail/" $4 }' "$D/results.txt"
awk -F'\t' '{ n[$1]++; t++ } END { printf "render_sweep: %d runs:", t; for (k in n) printf " %s %d", k, n[k]; printf "\n" }' "$D/results.txt"
if grep -qv '^OK' "$D/results.txt"; then exit 1; fi
