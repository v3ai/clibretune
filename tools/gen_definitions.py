#!/usr/bin/env python3
"""gen_definitions.py - embed the bundled ECU definitions (INI files) into src/project/inidb_data.c.

    python3 tools/gen_definitions.py              regenerate src/project/inidb_data.c
    python3 tools/gen_definitions.py --check      exit 1 if the generated file is out of date
    python3 tools/gen_definitions.py --stats      print sizes only
    python3 tools/gen_definitions.py --only F,G   bundle only these families (e.g. --only speeduino
                                                  for a build without the GPL-3.0 rusEFI/FOME files)
    python3 tools/gen_definitions.py --empty      write an empty library
    python3 tools/gen_definitions.py --extract    unpack the files of the committed inidb_data.c
                                                  into build/definitions/ (sha256 checked)

Input : assets/definitions/<family>/sources.json (written by tools/fetch_definitions.py) and the
        files it lists, in build/definitions/<file> (tools/fetch_definitions.py downloads them,
        --extract recovers them from the committed bundle).
Output: src/project/inidb_data.c (table lt_inidb_tab, declared in src/project/inidb.h)

Storage: every file is packed on its own with tools/lzpack.py, most of them against a preset
history of one or two similar files that are already in the library (lt_lzb_decode_hist):
  Speeduino   the newest release alone, each older release against the next newer one
  rusEFI      per release one anchor (proteus_f4): the newest alone, older ones against the
              anchor of the next newer release; every other board against its release's anchor
              plus the same board in the next newer release
  FOME        the anchor against the closest rusEFI anchor, other boards against the FOME anchor
  miata-ecus  against whichever rusEFI file packs it smallest
so nearly identical board files cost a few kilobytes. Packing is lossless; every file is checked
to unpack byte for byte (and to its sha256) before anything is written.
"""
import hashlib
import json
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzpack  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets", "definitions")
CACHE = os.path.join(ROOT, "build", "definitions")
OUT = os.path.join(ROOT, "src", "project", "inidb_data.c")
FAMILIES = ["speeduino", "rusefi", "fome", "miata-ecus"]
ANCHOR = "proteus_f4"
SPDX = {"speeduino": "GPL-2.0-or-later", "rusefi": "GPL-3.0-only", "fome": "GPL-3.0-only",
        "miata-ecus": "GPL-3.0-only"}


def load_entries(families):
    out = []
    for fam in families:
        p = os.path.join(ASSETS, fam, "sources.json")
        with open(p, encoding="utf-8") as f:
            ents = json.load(f)
        if fam == "speeduino":
            ents.sort(key=lambda e: tag_key(e["version"]), reverse=True)
        else:
            rels = []
            for e in ents:
                if e["version"] not in rels:
                    rels.append(e["version"])
            ents.sort(key=lambda e: (rels.index(e["version"]), e["board"] != ANCHOR,
                                     e["board"].lower()))
        out.extend(ents)
    return out


def tag_key(t):
    m = re.match(r"^(\d{6})(?:[.-]?([0-9a-z]+))?$", t)
    return (int(m.group(1)), m.group(2) or "") if m else (0, "")


def read_file(e):
    with open(os.path.join(CACHE, e["file"]), "rb") as f:
        data = f.read()
    if hashlib.sha256(data).hexdigest() != e["sha256"]:
        raise SystemExit("%s: sha256 differs from the manifest" % e["file"])
    return data


def version_label(e):
    t = e.get("tags") or [e["version"]]
    return t[0] if len(t) == 1 else "%s to %s" % (t[0], t[-1])


def candidates(ents, i):
    """lists of history entry indexes worth trying for entry i (all < i)"""
    e = ents[i]
    fam = e["family"]
    same = [j for j in range(i) if ents[j]["family"] == fam]
    if fam == "speeduino":
        return [[same[-1]]] if same else [[]]
    if fam == "miata-ecus":
        return [[j] for j in range(i) if ents[j]["family"] == "rusefi"
                and ents[j]["version"] == ents_first_release(ents, "rusefi")] or [[]]
    rels = []
    for j in same + [i]:
        if ents[j]["version"] not in rels:
            rels.append(ents[j]["version"])
    k = rels.index(e["version"])
    anchor = [j for j in same if ents[j]["version"] == e["version"] and ents[j]["board"] == ANCHOR]
    if e["board"] == ANCHOR or not anchor:
        if k > 0:
            prev = [j for j in same if ents[j]["version"] == rels[k - 1] and ents[j]["board"] == ANCHOR]
            if prev:
                return [prev]
        if fam == "fome":       # a FOME anchor: against the rusEFI anchors
            return [[j] for j in range(i) if ents[j]["family"] == "rusefi"
                    and ents[j]["board"] == ANCHOR] or [[]]
        return [[]]
    c = [anchor[0]]
    if k > 0:
        older = [j for j in same if ents[j]["version"] == rels[k - 1]
                 and ents[j]["board"] == e["board"]]
        if older:
            return [[anchor[0], older[0]]]
    return [c]


def ents_first_release(ents, fam):
    for e in ents:
        if e["family"] == fam:
            return e["version"]
    return ""


def pack_all(ents, verbose=False):
    datas = [read_file(e) for e in ents]
    packed = []
    for i, e in enumerate(ents):
        best = None
        for c in candidates(ents, i):
            hist = b"".join(datas[j] for j in c)
            z = lzpack.lzb_pack(datas[i], hist) if c else lzpack.lzb_compress(datas[i])
            if best is None or len(z) < len(best[1]):
                best = (c, z)
        c, z = best
        hist = b"".join(datas[j] for j in c)
        if lzpack.lzb_decompress(z, len(datas[i]), hist) != datas[i]:
            raise SystemExit("%s: packer round trip failed" % e["file"])
        packed.append((c, z))
        if verbose:
            print("  %-64s %8d -> %7d  hist %s" % (e["file"], len(datas[i]), len(z), c))
    return datas, packed


HEADER = """\
/* inidb_data.c - the ECU definitions (INI files) bundled with the program. GENERATED by
 * tools/gen_definitions.py from assets/definitions/<family>/sources.json, DO NOT EDIT BY HAND.
 *
 * LICENCES: the embedded files are third-party works, unmodified (stored packed, lossless; each
 * unpacks byte for byte, checked against its size and CRC-32 here and sha256 in the manifest):
 *   Speeduino   GPL-2.0-or-later   Speeduino project (Josh Stewart and contributors)
 *   rusEFI      GPL-3.0 + the rusEFI additional terms (off-road use notices),
 *                                  rusEFI LLC and contributors
 *   FOME        GPL-3.0 + the same terms, FOME contributors
 *   miata-ecus  GPL-3.0 (generated by rusEFI from the MIT-licensed miata-ecus board files)
 * LibreTune is GPL-2.0-or-later; a binary that contains the GPL-3.0 files is distributed under
 * GPL-3.0. `python3 tools/gen_definitions.py --only speeduino` builds a library without them.
 * Sources, tags, commits and licences: assets/definitions/README.md and the sources.json files.
 *
 * Library: %d files, %d bytes unpacked, %d bytes packed,
 *          in blocks of at most %d bytes.
 * SPDX-License-Identifier: %s
 */
#include "inidb.h"

"""

EMPTY_HEADER = """\
/* inidb_data.c - empty ECU definition library. GENERATED by `tools/gen_definitions.py --empty`,
 * DO NOT EDIT BY HAND. Run tools/gen_definitions.py without --empty to bundle the definitions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "inidb.h"

"""


def generate(families, verbose=False):
    ents = load_entries(families) if families else []
    datas, packed = pack_all(ents, verbose) if ents else ([], [])
    z = bytearray()
    rows = []
    for i, e in enumerate(ents):
        c, zz = packed[i]
        rows.append((e, len(z), len(zz), c, zlib.crc32(datas[i]) & 0xFFFFFFFF, len(datas[i])))
        z += zz
    z = bytes(z)
    blks = lzpack.blocks(z)
    raw = sum(len(d) for d in datas)
    spdx = " AND ".join(sorted(set(SPDX[f] for f in families))) if ents else "GPL-2.0-or-later"
    out = [HEADER % (len(ents), raw, len(z), lzpack.ZBLOCK, spdx) if ents else EMPTY_HEADER]
    for i, b in enumerate(blks):
        out.append("static const unsigned char idz%d[%d] = {\n%s\n};\n" % (i, len(b), lzpack.cbytes(b)))
    if not blks:
        out.append("static const unsigned char idz0[1] = { 0 };\n")
    nb = max(1, len(blks))
    out.append("\nconst unsigned char *const lt_inidb_z[%d] = {\n" % nb)
    names = ["idz%d" % i for i in range(nb)]
    for k in range(0, nb, 10):
        out.append("    %s%s\n" % (", ".join(names[k:k + 10]), "," if k + 10 < nb else ""))
    out.append("};\n")
    out.append("const size_t lt_inidb_zlen[%d] = {\n" % nb)
    lens = [str(len(b)) for b in blks] or ["0"]
    for k in range(0, nb, 10):
        out.append("    %s%s\n" % (", ".join(lens[k:k + 10]), "," if k + 10 < nb else ""))
    out.append("};\n")
    out.append("const size_t lt_inidb_zn = %d;\n" % len(blks))
    out.append("const size_t lt_inidb_ztotal = %dUL;\n\n" % len(z))
    out.append("const size_t lt_inidb_tab_n = %d;\n" % len(ents))
    out.append("const lt_inidb_def lt_inidb_tab[%d] = {\n" % max(1, len(ents)))
    for i, (e, off, zl, c, crc, size) in enumerate(rows):
        h = c + [-1, -1]
        out.append("    { %s, %s, %s,\n" % (lzpack.cstr(e["family"]), lzpack.cstr(e["board"]),
                                            lzpack.cstr(version_label(e))))
        out.append("      %s,\n" % lzpack.cstr(e["signature"]))
        out.append("      %s,\n" % lzpack.cstr(os.path.basename(e["file"])))
        out.append("      %s,\n" % lzpack.cstr(e["source"]))
        out.append("      %s,\n" % lzpack.cstr(SPDX[e["family"]]))
        out.append("      %dUL, 0x%08XUL, %d, %d, %dUL, %dUL }%s\n"
                   % (size, crc, h[0], h[1], off, zl, "," if i + 1 < len(rows) else ""))
    if not rows:
        out.append('    { "", "", "", "", "", "", "", 0UL, 0x00000000UL, -1, -1, 0UL, 0UL }\n')
    out.append("};\n")
    return "".join(out), ents, datas, len(z)


def parse_generated(text):
    """(stream bytes, rows) from a generated inidb_data.c"""
    blocks = {}
    for m in re.finditer(r"static const unsigned char idz(\d+)\[\d+\] = \{\n(.*?)\n\};", text, re.S):
        blocks[int(m.group(1))] = bytes(int(x) for x in re.findall(r"\d+", m.group(2)))
    nz = int(re.search(r"const size_t lt_inidb_zn = (\d+);", text).group(1))
    z = b"".join(blocks[i] for i in range(nz))
    rows = []
    pat = re.compile(r'    \{ "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", "(?:[^"\\]|\\.)*",\n'
                     r'      "((?:[^"\\]|\\.)*)",\n      "((?:[^"\\]|\\.)*)",\n.*?\n.*?\n'
                     r'      (\d+)UL, 0x([0-9A-F]+)UL, (-?\d+), (-?\d+), (\d+)UL, (\d+)UL \}')
    for m in pat.finditer(text):
        rows.append({"family": m.group(1), "board": m.group(2), "signature": m.group(3),
                     "file": m.group(4), "size": int(m.group(5)), "crc": int(m.group(6), 16),
                     "hist": [int(m.group(7)), int(m.group(8))], "off": int(m.group(9)),
                     "zlen": int(m.group(10))})
    return z, rows


def extract():
    with open(OUT, encoding="utf-8") as f:
        z, rows = parse_generated(f.read())
    datas = []
    for r in rows:
        hist = b"".join(datas[j] for j in r["hist"] if j >= 0)
        d = lzpack.lzb_decompress(z[r["off"]:r["off"] + r["zlen"]], r["size"], hist)
        if zlib.crc32(d) & 0xFFFFFFFF != r["crc"]:
            raise SystemExit("%s: CRC mismatch" % r["file"])
        datas.append(d)
    ents = load_entries([f for f in FAMILIES if os.path.exists(os.path.join(ASSETS, f, "sources.json"))])
    by_sha = {hashlib.sha256(d).hexdigest(): d for d in datas}
    n = 0
    for e in ents:
        d = by_sha.get(e["sha256"])
        if d is None:
            print("  not in the bundle: %s" % e["file"])
            continue
        p = os.path.join(CACHE, e["file"])
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(d)
        n += 1
    print("extracted %d files into %s" % (n, CACHE))
    return 0


def main(argv):
    if "--extract" in argv:
        return extract()
    fams = FAMILIES
    if "--only" in argv:
        fams = [f for f in argv[argv.index("--only") + 1].split(",") if f]
        for f in fams:
            if f not in FAMILIES:
                raise SystemExit("unknown family %s" % f)
    if "--empty" in argv:
        fams = []
    text, ents, datas, zsize = generate(fams, verbose="--stats" in argv or "-v" in argv)
    if "--stats" in argv:
        print("total %d bytes in %d files, packed %d bytes, generated source %d bytes"
              % (sum(len(d) for d in datas), len(ents), zsize, len(text)))
        return 0
    if "--check" in argv:
        try:
            with open(OUT, encoding="utf-8", newline="") as f:
                cur = f.read()
        except OSError:
            cur = ""
        if cur != text:
            print("%s is out of date: run python3 tools/gen_definitions.py" % OUT)
            return 1
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d files, %d bytes packed, %d bytes of source)"
          % (OUT, len(ents), zsize, len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
