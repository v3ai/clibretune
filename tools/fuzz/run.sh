#!/bin/sh
# tools/fuzz/run.sh - build and run the libFuzzer harnesses (clang only, not part of ./build.sh).
#
#   tools/fuzz/run.sh NAME SECONDS [libFuzzer args...]   fuzz one harness for SECONDS
#   tools/fuzz/run.sh NAME repro FILE...                  run inputs once (crash reproduction)
#   tools/fuzz/run.sh build                               build every harness, regenerate seeds
#   tools/fuzz/run.sh list                                harness names
#
# NAME is a tools/fuzz/fuzz_NAME.c harness: ini expr msq msqpart mlg msl csv dash fw ttf
# project tsimport proto. The library is compiled once with
# -fsanitize=fuzzer-no-link,address,undefined into build/fuzz/obj; harnesses link against it.
# Corpus: build/fuzz/corpus/NAME (seeded from tests/fixtures, assets and tools/fuzz/seedgen.c,
# plus build/ini_corpus for "ini" when tools/fetch_ini_corpus.sh has been run).
# Crashes / leaks / timeouts land in build/fuzz/artifacts/NAME/ (the log in build/fuzz/NAME.log).
# ini/expr get tools/fuzz/ini.dict (and -max_len=32768), msq/msqpart/dash/tsimport xml.dict.
# Several harnesses at once: run "build" first, then e.g.
#   for n in $(tools/fuzz/run.sh list); do tools/fuzz/run.sh $n 3600 -fork=1 -ignore_crashes=1 & done
# Env: CC (default clang), JOBS, FUZZ_CFLAGS (extra flags), FUZZ_TIMEOUT (per input, s, 10),
#      FUZZ_RSS_MB (2048).
set -e
cd "$(dirname "$0")/../.."
. tools/target.sh

CC=${CC:-clang}
JOBS=${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 1)}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
CF="-g -O1 $SAN ${FUZZ_CFLAGS:-}"
B=build/fuzz
OBJ=$B/obj
LIB=$B/libltfuzz.a

usage() { sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 2; }

build_lib() {
    mkdir -p "$OBJ"
    objs=""
    running=0
    rm -f "$OBJ/.fail"
    for src in $(lt_sources); do
        o="$OBJ/$(printf '%s' "${src#src/}" | tr '/' '_').o"
        objs="$objs $o"
        if [ -f "$o" ] && [ -z "$(find src \( -name '*.c' -o -name '*.h' \) -newer "$o" | head -n 1)" ]; then
            continue
        fi
        cppf=""
        if lt_is_platform_file "$src"; then cppf="$PLAT_CPPFLAGS"; fi
        ( $CC $CF -fsanitize=fuzzer-no-link $cppf -Isrc -c "$src" -o "$o.tmp" && mv -f "$o.tmp" "$o" ) \
            || : > "$OBJ/.fail" &
        running=$((running + 1))
        if [ "$running" -ge "$JOBS" ]; then wait; running=0; fi
    done
    wait
    [ ! -f "$OBJ/.fail" ] || { echo "fuzz: library build failed" >&2; exit 1; }
    if [ ! -f "$LIB" ] || [ -n "$(find $objs -newer "$LIB" | head -n 1)" ]; then
        rm -f "$LIB"
        ar rcs "$LIB" $objs
    fi
}

build_one() {   # $1 = name
    h=tools/fuzz/fuzz_$1.c
    [ -f "$h" ] || { echo "fuzz: no harness $h" >&2; exit 2; }
    out=$B/fuzz_$1
    if [ ! -f "$out" ] || [ "$LIB" -nt "$out" ] || [ -n "$(find tools/fuzz -newer "$out" -name '*.[ch]' | head -n 1)" ]; then
        echo "LD $out"
        $CC $CF -fsanitize=fuzzer -Isrc -Itools/fuzz "$h" "$LIB" -o "$out" $TARGET_LIBS
    fi
}

gen_seeds() {   # synthetic seeds (run "build" first when fuzzing several harnesses at once)
    $CC $CF -Isrc tools/fuzz/seedgen.c "$LIB" -o "$B/seedgen.$$" $TARGET_LIBS
    rm -rf "$B/seeds"
    mkdir -p "$B/seeds"
    "$B/seedgen.$$" "$B/seeds"
    rm -f "$B/seedgen.$$"
    : > "$B/seeds/.done"
}

seeds() {   # $1 = name: fill build/fuzz/corpus/NAME if empty
    c=$B/corpus/$1
    mkdir -p "$c" "$B/artifacts/$1"
    [ -z "$(ls -A "$c")" ] || return 0
    [ -f "$B/seeds/.done" ] || gen_seeds
    if [ -d "$B/seeds/$1" ]; then cp "$B/seeds/$1"/* "$c"/; fi
    case $1 in
        ini)
            for f in tests/fixtures/*/*.ini tests/fixtures/project/ts_proj/projectCfg/*.ini; do
                cp "$f" "$c/$(printf '%s' "$f" | tr '/' '_')"
            done
            # big real INIs are cut into one seed per [Section] so mutation stays fast
            { echo tests/fixtures/ini/demo.ini
              if [ -d build/ini_corpus ]; then find build/ini_corpus -name '*.ini' | sort | head -n 30; fi
            } | while read -r f; do
                awk -v out="$c/sec_$(printf '%s' "$f" | cksum | cut -d' ' -f1)" \
                    -f tools/fuzz/split_ini.awk "$f"
            done ;;
        expr)
            for e in '{ rpm > 3000 && (map < 100 || tps >= 50) }' '{ bitStringValue(a, b) }' \
                     'x ? 1.5 : -2e3' '{ (coolant - 32) * 5 / 9 }' '"str" + 3 % 2 << 4 >> 1' \
                     'arrayValue(array.veTable, 3) + table(x, "t.inc")' '!(a & 3) | ~b ^ 7'; do
                printf '%s' "$e" > "$c/$(printf '%s' "$e" | cksum | cut -d' ' -f1)"
            done ;;
        ttf)
            for f in assets/fonts/*.ttf; do cp "$f" "$c/"; done ;;
        project)
            cp tests/fixtures/project/ts_proj/projectCfg/project.properties "$c/"
            printf '%s' '{"version":"1.0","name":"n","created":"2024-01-01T00:00:00Z","modified":"x","ecu_definition":"d.ini","signature":"s","connection":{"port":null,"baud_rate":115200,"timeout_ms":1000},"settings":{"auto_load_tune":true,"auto_save_tune":false,"auto_connect":false,"max_restore_points":20},"dashboard":"a.dash"}' > "$c/project.json"
            printf '%s' '{"name":"n","description":"d","created":"2024-01-01T00:00:00Z","created_by":"u","actions":[{"type":"TableEdit","table":"veTable","x":1,"y":2,"value":3.5,"old_value":1.0},{"Pause":{"ms":100}}]}' > "$c/actions.json"
            printf 'Proj_2025-01-02_03.04.05-2.msq' > "$c/restore_name"
            head -c 4000 tests/fixtures/project/mini.ini > "$c/mini.ini" ;;
        tsimport)
            d=tests/fixtures/project/ts_proj
            { cat "$d/projectCfg/project.properties"; printf '\n#~#~\n'; cat "$d/projectCfg/mainController.ini"
              printf '\n#~#~\n'; cat "$d/CurrentTune.msq"; printf '\n#~#~\n'; cat "$d/projectCfg/pcVariableValues.msq"
            } > "$c/ts_proj" ;;
    esac
}

case ${1:-} in
    ""|-h|--help) usage ;;
    list) ls tools/fuzz/fuzz_*.c | sed 's|.*/fuzz_||; s|\.c$||'; exit 0 ;;
    build)
        build_lib
        for h in tools/fuzz/fuzz_*.c; do n=${h##*/fuzz_}; build_one "${n%.c}"; done
        gen_seeds
        exit 0 ;;
esac

NAME=$1
SECS=${2:-60}
shift
[ $# -gt 0 ] && shift
build_lib
build_one "$NAME"
FUZZ_TMP=$B/tmp/$NAME
export FUZZ_TMP
rm -rf "$FUZZ_TMP" && mkdir -p "$FUZZ_TMP"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:allocator_may_return_null=1:abort_on_error=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
if [ "$SECS" = repro ]; then
    exec "$B/fuzz_$NAME" "$@"
fi
seeds "$NAME"
# per-harness defaults: keyword dictionaries, and short inputs for the INI parser
case $NAME in
    ini) set -- -dict=tools/fuzz/ini.dict -max_len=32768 "$@" ;;
    expr) set -- -dict=tools/fuzz/ini.dict "$@" ;;
    msq|msqpart|dash|tsimport) set -- -dict=tools/fuzz/xml.dict "$@" ;;
esac
echo "fuzz: $NAME for ${SECS}s (corpus $B/corpus/$NAME, artifacts $B/artifacts/$NAME/)"
"$B/fuzz_$NAME" -max_total_time="$SECS" -timeout="${FUZZ_TIMEOUT:-10}" \
    -rss_limit_mb="${FUZZ_RSS_MB:-2048}" -print_final_stats=1 \
    -artifact_prefix="$B/artifacts/$NAME/" "$@" "$B/corpus/$NAME" 2>&1 | tee "$B/$NAME.log" | \
    grep -E '^(#[0-9]+ +(INITED|DONE|NEW)|==[0-9]+==|SUMMARY|artifact_prefix|Test unit|stat::)' | tail -n 40
rm -rf "$FUZZ_TMP"
grep -q 'Test unit written\|SUMMARY:' "$B/$NAME.log" && { echo "fuzz: $NAME FOUND A PROBLEM (see $B/$NAME.log)"; exit 1; }
exit 0
