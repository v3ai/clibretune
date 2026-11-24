#!/bin/sh
# tools/bench_ui.sh - frame-time benchmark of the GUI (headless, demo mode).
#
#   tools/bench_ui.sh [options]
#
# Every case renders the demo app offscreen (LT_SCREENSHOT) with the real clock (LT_PERF), skips
# a warm-up and prints the frame time (draw + present, ms) per case and size: average, 95th
# percentile and maximum. Steady cases measure N frames of an open screen (the live demo keeps
# changing values); interaction cases replay a scripted input sequence (LT_SHOT_SCRIPT, one frame
# per step: hover sweeps, typing into a table, scrolling a list, switching tabs, opening dialogs).
# Options:
#   --bin PATH        app binary (default build/libretune)
#   --frames N        measured frames per steady case (default 60)
#   --sizes LIST      "WxH@PCT ..." (default "1600x1000@100 3200x2000@200 3840x2160@200 3840x2160@150")
#   --cases LIST      subset of: dashboard realtime ve_table ve_3d autotune datalog curve
#                     hover_sidebar hover_table type_table scroll_list switch_tabs open_dialog
#   --sleep MS        pause between frames (default 16, like a 60 Hz display)
#   --out FILE        also write the results as TSV (case size avg p95 max)
#   --base FILE       TSV of an earlier run: print it next to this one (before / after)
#   --full            re-render the whole window every frame (LT_RENDER_FULL=1): the cost of a
#                     full redraw (dialog closing, screen switch, scale change) on each screen
# Settings come from a private config directory, so the user's own settings do not matter.
set -e
cd "$(dirname "$0")/.."

BIN=build/libretune
FRAMES=60
SIZES="1600x1000@100 3200x2000@200 3840x2160@200 3840x2160@150"
CASES="dashboard realtime ve_table ve_3d autotune datalog curve hover_sidebar hover_table type_table scroll_list switch_tabs open_dialog"
SLEEP=16
OUT=
BASE=
FULL=0
while [ $# -gt 0 ]; do
    case $1 in
        --bin) shift; BIN=$1 ;;
        --frames) shift; FRAMES=$1 ;;
        --sizes) shift; SIZES=$1 ;;
        --cases) shift; CASES=$1 ;;
        --sleep) shift; SLEEP=$1 ;;
        --out) shift; OUT=$1 ;;
        --base) shift; BASE=$1 ;;
        --full) FULL=1 ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -x "$BIN" ] || { echo "no binary $BIN (./build.sh app)" >&2; exit 2; }

W=$(mktemp -d "${TMPDIR:-/tmp}/bench_ui.XXXXXX")
trap 'rm -rf "$W"' EXIT INT TERM
mkdir -p "$W/home" "$W/cfg" "$W/proj"
RES=$W/res.tsv
: > "$RES"

# $1 = scale percent, then logical x y pairs -> device coordinates "X/Y"
dev() { awk -v s="$1" -v x="$2" -v y="$3" 'BEGIN { printf "%d/%d", x * s / 100, y * s / 100 }'; }

# script for an interaction case at scale $2 (logical layout coordinates scaled to the device)
script_for() {
    s=$2
    case $1 in
        hover_sidebar)     # mouse down the sidebar tree and back
            i=0; out=""
            while [ $i -lt 50 ]; do out="$out m:$(dev "$s" 120 $((130 + i * 14)))"; i=$((i + 1)); done
            while [ $i -gt 0 ]; do i=$((i - 1)); out="$out m:$(dev "$s" 140 $((130 + i * 14)))"; done
            echo "$out" ;;
        hover_table)       # diagonal sweep over the VE table grid
            i=0; out=""
            while [ $i -lt 100 ]; do
                out="$out m:$(dev "$s" $((340 + i * 12)) $((190 + i * 7)))"; i=$((i + 1))
            done
            echo "$out" ;;
        type_table)        # click a cell, type a value, Enter; next cell
            i=0; out=""
            while [ $i -lt 20 ]; do
                c=$(dev "$s" $((440 + (i % 10) * 68)) $((214 + (i / 10) * 22)))
                out="$out d:$c u:$c text:5 text:1 enter"; i=$((i + 1))
            done
            echo "$out" ;;
        scroll_list)       # wheel over the datalog channel list, down then up
            c=$(dev "$s" 400 700); i=0; out=""
            while [ $i -lt 50 ]; do out="$out w:$c/-1"; i=$((i + 1)); done
            while [ $i -gt 0 ]; do out="$out w:$c/1"; i=$((i - 1)); done
            echo "$out" ;;
        switch_tabs)       # open a third tab from the sidebar, then cycle with Ctrl+Tab
            c=$(dev "$s" 77 408); i=0; out="d:$c u:$c m:$(dev "$s" 900 600) reset"
            while [ $i -lt 60 ]; do out="$out ctrl+tab"; i=$((i + 1)); done
            echo "$out" ;;
        open_dialog)       # Settings dialog open / close
            i=0; out=""
            while [ $i -lt 30 ]; do out="$out ctrl+comma m:$(dev "$s" 800 $((500 + i))) esc"; i=$((i + 1)); done
            echo "$out" ;;
    esac
}

run_case() {
    name=$1; size=$2
    wh=${size%@*}; pct=${size#*@}
    open=; view=; scr=; frames=$((FRAMES + 10)); skip=10
    case $name in
        dashboard|hover_sidebar|open_dialog) ;;
        realtime) open=std_realtime ;;
        ve_table|hover_table|type_table|switch_tabs) open=veTableTbl ;;
        ve_3d) open=veTableTbl; view=3d ;;
        autotune) open=autotune ;;
        datalog|scroll_list) open=datalog ;;
        curve) open=cltFuelCorrCurveDialog ;;
        *) echo "unknown case $name" >&2; return 0 ;;
    esac
    case $name in
        hover_*|type_*|scroll_*|switch_*|open_*) scr=$(script_for "$name" "$pct"); frames=12 ;;
    esac
    log=$W/log.txt
    env HOME="$W/home" LIBRETUNE_CONFIG_DIR="$W/cfg" LIBRETUNE_PROJECTS="$W/proj" \
        LT_RENDER_FULL=$FULL LT_PERF=1 LT_PERF_SKIP=$skip LT_SHOT_SLEEP="$SLEEP" LT_SCALE="$pct" \
        LT_SCREENSHOT="$W/x.ppm" LT_SHOT_SIZE="$wh" LT_SHOT_FRAMES=$frames \
        LT_SHOT_OPEN="$open" LT_TABLE_VIEW="$view" LT_SHOT_SCRIPT="$scr" \
        "$BIN" --demo > "$log" 2>&1 || { echo "$name $size: run failed" >&2; tail -5 "$log" >&2; return 0; }
    grep '^perf:' "$log" | tail -1 |
        awk -v n="$name" -v sz="$size" '{
            for (i = 1; i <= NF; i++) {
                if ($i == "avg" && $(i - 1) == "frame") a = $(i + 1)
                if ($i == "p95") p = $(i + 1)
                if ($i == "max" && m == "") m = $(i + 1)
            }
            printf "%s\t%s\t%s\t%s\t%s\n", n, sz, a, p, m }' >> "$RES"
}

for c in $CASES; do
    for z in $SIZES; do
        run_case "$c" "$z"
        tail -1 "$RES" | awk -F'\t' '{ printf "  %-14s %-16s avg %8s  p95 %8s  max %8s\n", $1, $2, $3, $4, $5 }' >&2
    done
done

echo
if [ -n "$BASE" ] && [ -f "$BASE" ]; then
    awk -F'\t' 'NR == FNR { k = $1 "\t" $2; ba[k] = $3; bp[k] = $4; bm[k] = $5; next }
        BEGIN { printf "%-14s %-16s %26s   %26s\n", "", "", "before (ms)", "after (ms)"
                printf "%-14s %-16s %8s %8s %8s   %8s %8s %8s\n", "case", "size", "avg", "p95", "max", "avg", "p95", "max" }
        { k = $1 "\t" $2
          printf "%-14s %-16s %8s %8s %8s   %8s %8s %8s\n", $1, $2, ba[k], bp[k], bm[k], $3, $4, $5 }' \
        "$BASE" "$RES"
else
    awk -F'\t' 'BEGIN { printf "%-14s %-16s %8s %8s %8s\n", "case", "size", "avg", "p95", "max" }
        { printf "%-14s %-16s %8s %8s %8s\n", $1, $2, $3, $4, $5 }' "$RES"
fi
echo "frame = draw + present, ms; $FRAMES frames per steady case, sleep $SLEEP ms"
if [ -n "$OUT" ]; then cp "$RES" "$OUT"; fi
