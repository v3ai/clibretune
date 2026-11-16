#!/bin/sh
# build_cosmo.sh - build the single-file Actually Portable Executable build/libretune.com
# Requires cosmocc (default: .toolchain/cosmocc/bin/cosmocc, or set COSMOCC).
set -e
cd "$(dirname "$0")"
COSMOCC=${COSMOCC:-.toolchain/cosmocc/bin/cosmocc}
mkdir -p build
$COSMOCC -O2 -Isrc src/main.c $(find src -name '*.c' ! -path src/main.c | sort) -o build/libretune.com
ls -l build/libretune.com
