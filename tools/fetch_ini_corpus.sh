#!/bin/sh
# tools/fetch_ini_corpus.sh - download a broad set of real ECU definition files (INIs) into
# build/ini_corpus/ (gitignored) for tools/ini_corpus_check.sh and the INI fuzzer seeds.
#
#   tools/fetch_ini_corpus.sh [--force]
#
# Sources (pinned to commits/tags so a run is reproducible; override with the env vars):
#   rusefi/       every generated board INI of rusEFI (firmware/tunerstudio/generated) at
#                 RUSEFI_REF, plus rusefi_proteus_f4.ini at two older commits
#   fome/         every generated board INI of FOME at FOME_REF
#   speeduino/    reference/speeduino.ini of Speeduino releases (git tags)
#   megasquirt/   MS1 / MS1-Extra / MS2 / MS2-Extra / MS3 INIs collected in the public
#                 scudderfish/Normaliser repository (inis/), at MS_REF
# Needs curl. Unauthenticated GitHub API calls are limited to 60 an hour (this script makes
# 3); set GITHUB_TOKEN to raise that. Files already present are kept unless --force.
# The INIs are third-party files under their own licenses: they stay in build/, never in git.
set -e
cd "$(dirname "$0")/.."

RUSEFI_REF=${RUSEFI_REF:-a6f2700be2026f5f458d0a2913d5861a56d08c15}
RUSEFI_OLD=${RUSEFI_OLD:-"bebe581ca6ecfd549ad8e98ffd9aea5e8a17ed87 88deee26e09d24b823ddfc8247f5dce1b5bb08be"}
FOME_REF=${FOME_REF:-71a6e6ec246ee16b87e0667df1b79e8e838a05b2}
MS_REF=${MS_REF:-83e320b61ccad30e45b98acebf812e55f405091a}
SPEEDUINO_TAGS=${SPEEDUINO_TAGS:-"202501.7 202501 202402.3 202310 202305 202207.2 202202 202108 \
202012 202005 201911 201905 201810 201712"}

OUT=build/ini_corpus
RAW=https://raw.githubusercontent.com
API=https://api.github.com
FORCE=0
[ "${1:-}" = --force ] && FORCE=1
command -v curl >/dev/null 2>&1 || { echo "fetch_ini_corpus: curl is required" >&2; exit 1; }

api() {   # $1 = API path; prints the JSON
    if [ -n "${GITHUB_TOKEN:-}" ]; then
        curl -fsSL --retry 3 -H "Authorization: Bearer $GITHUB_TOKEN" "$API/$1"
    else
        curl -fsSL --retry 3 "$API/$1"
    fi
}

# "name" values of a contents listing, one per line (no jq needed)
names_of() { tr ',' '\n' | sed -n 's/^ *"name": *"\(.*\)" *$/\1/p'; }

urlenc() { printf '%s' "$1" | sed 's/%/%25/g; s/ /%20/g; s/&/%26/g; s/#/%23/g; s/+/%2B/g'; }

get() {   # $1 = url, $2 = destination
    if [ "$FORCE" = 0 ] && [ -s "$2" ]; then return 0; fi
    mkdir -p "$(dirname "$2")"
    if curl -fsSL --retry 3 -o "$2.part" "$1"; then
        mv -f "$2.part" "$2"
        echo "  $2"
    else
        rm -f "$2.part"
        echo "  FAILED $1" >&2
    fi
}

mkdir -p "$OUT"
: > "$OUT/SOURCES.txt.new"

echo "rusEFI @ $RUSEFI_REF"
api "repos/rusefi/rusefi/contents/firmware/tunerstudio/generated?ref=$RUSEFI_REF" | names_of |
    grep '\.ini$' | while read -r n; do
        get "$RAW/rusefi/rusefi/$RUSEFI_REF/firmware/tunerstudio/generated/$n" "$OUT/rusefi/$n"
    done
for r in $RUSEFI_OLD; do
    get "$RAW/rusefi/rusefi/$r/firmware/tunerstudio/generated/rusefi_proteus_f4.ini" \
        "$OUT/rusefi_old/proteus_f4_$(printf '%s' "$r" | cut -c1-8).ini"
done
echo "rusefi/rusefi $RUSEFI_REF (+ $RUSEFI_OLD) firmware/tunerstudio/generated GPL-3.0" >> "$OUT/SOURCES.txt.new"

echo "FOME @ $FOME_REF"
api "repos/FOME-Tech/fome-fw/contents/firmware/tunerstudio/generated?ref=$FOME_REF" | names_of |
    grep '\.ini$' | while read -r n; do
        get "$RAW/FOME-Tech/fome-fw/$FOME_REF/firmware/tunerstudio/generated/$n" "$OUT/fome/$n"
    done
echo "FOME-Tech/fome-fw $FOME_REF firmware/tunerstudio/generated GPL-3.0" >> "$OUT/SOURCES.txt.new"

echo "Speeduino tags"
for t in $SPEEDUINO_TAGS; do
    get "$RAW/noisymime/speeduino/$t/reference/speeduino.ini" "$OUT/speeduino/speeduino_$t.ini"
done
echo "noisymime/speeduino tags $SPEEDUINO_TAGS reference/speeduino.ini GPL-2.0" >> "$OUT/SOURCES.txt.new"

echo "MegaSquirt family @ $MS_REF"
# the whole directory: several of these INIs #include siblings (lambdaSensors.ini, ...)
api "repos/scudderfish/Normaliser/contents/inis?ref=$MS_REF" | names_of |
    grep '\.ini$' | while read -r n; do
        get "$RAW/scudderfish/Normaliser/$MS_REF/inis/$(urlenc "$n")" "$OUT/megasquirt/$n"
    done
echo "scudderfish/Normaliser $MS_REF inis/ (MS1/MS2/MS3 INIs, vendor licenses)" >> "$OUT/SOURCES.txt.new"

mv -f "$OUT/SOURCES.txt.new" "$OUT/SOURCES.txt"
echo "fetch_ini_corpus: $(find "$OUT" -name '*.ini' | wc -l | tr -d ' ') INIs in $OUT"
