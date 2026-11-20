#!/usr/bin/env python3
"""gen_basetunes.py - embed the Speeduino base tunes into src/tune/basetune_data.c.

    python3 tools/gen_basetunes.py            regenerate src/tune/basetune_data.c
    python3 tools/gen_basetunes.py --check    exit 1 if the generated file is out of date
    python3 tools/gen_basetunes.py --empty    write an empty catalog (a build without the tunes)
    python3 tools/gen_basetunes.py --stats    print sizes only

Input : assets/basetunes/speeduino/index.json, the .msq files it lists (<make>/<filename>),
        README.md (source commit). The tunes are CC BY-NC 4.0, NOT GPL: see
        assets/basetunes/speeduino/README.md and LICENSE.
Output: src/tune/basetune_data.c (catalog table lt_basetune_tab, declared in src/tune/basetune.h)

All files are concatenated in catalog order and packed with a small LZ77 coder (lzb_compress
below; the format is documented at lt_lzb_decode in src/tune/basetune.h, which unpacks it at run
time). The packed bytes are written as unsigned char arrays of at most ZBLOCK bytes, since
Universal C section 9 limits objects to 32767 bytes. The tunes come out byte for byte as
upstream ships them; packing is lossless and --check verifies the round trip.
"""
import json
import os
import re
import sys
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "assets", "basetunes", "speeduino")
OUT = os.path.join(ROOT, "src", "tune", "basetune_data.c")
UPSTREAM = "https://github.com/speeduino/Tunes"
ZBLOCK = 32000       # bytes per packed array (objects <= 32767 bytes)


def cesc_list(data):
    """bytes -> list of C escaped characters (each element counts as one literal character)."""
    out = []
    for b in data:
        c = chr(b)
        if b == 0:
            raise SystemExit("NUL byte in input")
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "?":
            out.append("\\?")
        elif c == "\n":
            out.append("\\n")
        elif c == "\r":
            out.append("\\r")
        elif c == "\t":
            out.append("\\t")
        elif 0x20 <= b < 0x7F:
            out.append(c)
        else:
            out.append("\\%03o" % b)
    return out


def cstr(s):
    e = cesc_list(s.encode("utf-8"))
    if len(e) > 509:
        raise SystemExit("string too long for one literal: %r" % s[:40])
    return '"' + "".join(e) + '"'


# ---- LZ77 packer (format: lt_lzb_decode in src/tune/basetune.h) ----
MINM = 5
def lzb_compress(data, maxchain=64):
    n = len(data); out = bytearray(); heads = {}; chain = [0]*n
    i = 0; lit = 0
    def ext(v):
        while v >= 255: out.append(255); v -= 255
        out.append(v)
    def emit(litstart, litlen, off, mlen):
        ln = 15 if litlen >= 15 else litlen
        mn = 0 if mlen == 0 else (15 if mlen - 4 >= 15 else mlen - 4)
        out.append((ln << 4) | mn)
        if litlen >= 15: ext(litlen - 15)
        out.extend(data[litstart:litstart+litlen])
        if mlen:
            out.extend(bytes((off & 255, (off >> 8) & 255, (off >> 16) & 255)))
            if mlen - 4 >= 15: ext(mlen - 4 - 15)
    def insert(p):
        if p + 4 <= n:
            k = data[p:p+4]; chain[p] = heads.get(k, -1); heads[k] = p
    def best(p):
        if p + 4 > n: return 0, 0
        c = heads.get(data[p:p+4], -1); bl = 0; bo = 0; tries = 0
        while c >= 0 and tries < maxchain:
            if p - c < (1 << 24):
                l = 0; m = n - p
                # fast compare
                while l < m and data[c+l] == data[p+l]: l += 1
                if l > bl: bl, bo = l, p - c
                if l >= 4096: break
            c = chain[c]; tries += 1
        return bl, bo
    start = 0
    while i < n:
        bl, bo = best(i)
        if bl >= MINM:
            # lazy: check i+1
            insert(i)
            bl2, bo2 = best(i+1)
            if bl2 > bl + 1:
                i += 1; continue
            emit(start, i - start, bo, bl)
            for p in range(i+1, i+bl): insert(p)
            i += bl; start = i
        else:
            insert(i); i += 1
    emit(start, n - start, 0, 0)
    return bytes(out)
def lzb_decompress(z, n):
    o = bytearray(); i = 0
    while True:
        t = z[i]; i += 1; l = t >> 4
        if l == 15:
            while True:
                b = z[i]; i += 1; l += b
                if b != 255: break
        o += z[i:i+l]; i += l
        if len(o) == n: return bytes(o)
        off = z[i] | z[i+1] << 8 | z[i+2] << 16; i += 3
        m = (t & 15) + 4
        if (t & 15) == 15:
            while True:
                b = z[i]; i += 1; m += b
                if b != 255: break
        s = len(o) - off
        for k in range(m): o.append(o[s+k])


def source_commit():
    with open(os.path.join(SRC_DIR, "README.md"), encoding="utf-8") as f:
        m = re.search(r"Commit \| ([0-9a-f]{40})", f.read())
    if m is None:
        raise SystemExit("README.md: no 'Commit | <sha>' row")
    return m.group(1)


def attr(text, name):
    m = re.search(r'<versionInfo\b[^>]*\b' + name + r'="([^"]*)"', text)
    return m.group(1) if m else ""


def load_catalog():
    with open(os.path.join(SRC_DIR, "index.json"), encoding="utf-8") as f:
        idx = json.load(f)
    tunes = []
    for order, e in enumerate(idx):
        fn = urllib.parse.unquote(e["filename"])
        rel = e["make"] + "/" + fn
        with open(os.path.join(SRC_DIR, e["make"], fn), "rb") as f:
            data = f.read()
        head = data[:4096].decode("latin-1")
        tunes.append({
            "key": rel, "name": e.get("displayName", fn), "make": e.get("make", ""),
            "provider": e.get("provider", ""), "kind": e.get("type", ""),
            "board": e.get("board", ""), "description": e.get("description", ""),
            "firmware": int(e.get("firmware", 0)), "signature": attr(head, "signature"),
            "fwinfo": attr(head, "firmwareInfo"), "data": data, "order": order,
        })
    # Generic first, then by make; upstream order inside a make
    tunes.sort(key=lambda t: (t["make"] != "Generic", t["make"].lower(), t["order"]))
    return tunes


HEADER = """\
/* basetune_data.c - the Speeduino base tunes embedded in the program. GENERATED by
 * tools/gen_basetunes.py from assets/basetunes/speeduino/, DO NOT EDIT BY HAND.
 *
 * LICENCE: THIS FILE IS NOT UNDER THE GPL. The tune files, names and descriptions embedded here
 * are licensed under the Creative Commons Attribution-NonCommercial 4.0 International licence
 * (CC BY-NC 4.0, https://creativecommons.org/licenses/by-nc/4.0/; full text in
 * assets/basetunes/speeduino/LICENSE).
 *   Attribution: Speeduino project and contributors, %s
 *                (commit %s); author of each tune: the provider field below.
 *   Changes:     none: the files are stored packed (lossless LZ77) and unpack byte for byte.
 *   NonCommercial: a program or bundle containing this file must not be sold. LibreTune is free
 *                software and is never sold; to distribute it commercially, regenerate this file
 *                with `python3 tools/gen_basetunes.py --empty` first.
 *
 * Packed library: %d bytes unpacked, %d bytes packed, in blocks of at most %d bytes.
 * SPDX-License-Identifier: CC-BY-NC-4.0
 */
#include "basetune.h"

"""


EMPTY_HEADER = """\
/* basetune_data.c - empty Base Tune catalog. GENERATED by `tools/gen_basetunes.py --empty`,
 * DO NOT EDIT BY HAND. No tune is embedded (a build without the CC BY-NC 4.0 Speeduino tunes);
 * run tools/gen_basetunes.py without --empty to embed them again.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "basetune.h"

"""


def cbytes(data, ind="    "):
    lines = []
    for k in range(0, len(data), 20):
        lines.append(ind + ",".join(str(b) for b in data[k:k + 20]) + ",")
    return "\n".join(lines)


def generate(empty=False):
    commit = source_commit()
    tunes = [] if empty else load_catalog()
    raw = bytearray()
    for t in tunes:
        t["offset"] = len(raw)
        raw += t["data"]
    raw = bytes(raw)
    z = lzb_compress(raw) if raw else b""
    if raw and lzb_decompress(z, len(raw)) != raw:
        raise SystemExit("packer round trip failed")
    blocks = [z[k:k + ZBLOCK] for k in range(0, len(z), ZBLOCK)]
    out = [EMPTY_HEADER if empty else HEADER % (UPSTREAM, commit, len(raw), len(z), ZBLOCK)]
    for i, blk in enumerate(blocks):
        out.append("static const unsigned char btz%d[%d] = {\n%s\n};\n" % (i, len(blk), cbytes(blk)))
    if not blocks:
        out.append("static const unsigned char btz0[1] = { 0 };\n")
    out.append("\nconst unsigned char *const lt_basetune_z[%d] = { %s };\n"
               % (max(1, len(blocks)), ", ".join("btz%d" % i for i in range(max(1, len(blocks))))))
    out.append("const size_t lt_basetune_zlen[%d] = { %s };\n"
               % (max(1, len(blocks)), ", ".join(str(len(b)) for b in blocks) if blocks else "0"))
    out.append("const size_t lt_basetune_zn = %d;\n" % len(blocks))
    out.append("const size_t lt_basetune_raw = %d;\n\n" % len(raw))
    out.append("const char lt_basetune_source[] = %s;\n" % cstr(UPSTREAM))
    out.append("const char lt_basetune_commit[] = %s;\n" % cstr(commit))
    out.append("const size_t lt_basetune_tab_n = %d;\n" % len(tunes))
    out.append("const lt_basetune lt_basetune_tab[%d] = {\n" % max(1, len(tunes)))
    for i, t in enumerate(tunes):
        out.append("    { %s,\n" % cstr(t["key"]))
        out.append("      %s, %s,\n" % (cstr(t["name"]), cstr(t["make"])))
        out.append("      %s, %s, %s,\n" % (cstr(t["provider"]), cstr(t["kind"]), cstr(t["board"])))
        out.append("      %s,\n" % cstr(t["description"]))
        out.append("      %dL, %s, %s,\n" % (t["firmware"], cstr(t["signature"]), cstr(t["fwinfo"])))
        out.append("      %d, %d }%s\n" % (t["offset"], len(t["data"]),
                                           "," if i + 1 < len(tunes) else ""))
    if not tunes:
        out.append('    { "", "", "", "", "", "", "", 0L, "", "", 0, 0 }\n')
    out.append("};\n")
    return "".join(out), tunes, len(z)


def main(argv):
    empty = "--empty" in argv
    text, tunes, zsize = generate(empty)
    if "--stats" in argv:
        total = 0
        for t in tunes:
            total += len(t["data"])
            print("%-48s %7d bytes  %s" % (t["key"], len(t["data"]), t["signature"]))
        print("total %d bytes in %d tunes, packed %d bytes, generated source %d bytes"
              % (total, len(tunes), zsize, len(text)))
        return 0
    if "--check" in argv:
        try:
            with open(OUT, encoding="utf-8", newline="") as f:
                cur = f.read()
        except OSError:
            cur = ""
        if cur != text:
            print("%s is out of date: run python3 tools/gen_basetunes.py" % OUT)
            return 1
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d tunes, %d bytes)" % (OUT, len(tunes), len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
