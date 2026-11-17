#!/usr/bin/env python3
"""gen_embed.py - embed the IBM Plex TrueType fonts into src/gui/fonts_data.c.

    python3 tools/gen_embed.py            regenerate src/gui/fonts_data.c
    python3 tools/gen_embed.py --stats    print sizes only

Input : assets/fonts/IBMPlex{Sans,Mono}-*.ttf (SIL OFL 1.1, see assets/fonts/OFL.txt)
Output: src/gui/fonts_data.c

Universal C section 9 limits a single object to 32767 bytes, so every font is
emitted as several static const unsigned char arrays of at most CHUNK bytes
plus a table of (pointer, length) pairs; lt_ttf_open_chunks() concatenates
them at startup.

To keep the binary small the fonts are subset by a small pure-python TrueType
subsetter (no fonttools): the glyph IDs are kept (so GPOS kerning stays valid)
but outlines of glyphs outside the wanted code point set are dropped, the
cmap is rewritten as a compact format 4 table and tables the rasterizer does
not use (hinting programs, name, post, ...) are removed. The originals stay
untouched in assets/fonts/.
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(ROOT, "assets", "fonts")
OUT = os.path.join(ROOT, "src", "gui", "fonts_data.c")
CHUNK = 24000

# (enum name, file) in lt_font_id order
FONTS = [
    ("SANS", "IBMPlexSans-Regular.ttf"),
    ("SANS_MEDIUM", "IBMPlexSans-Medium.ttf"),
    ("SANS_SEMIBOLD", "IBMPlexSans-SemiBold.ttf"),
    ("MONO", "IBMPlexMono-Regular.ttf"),
    ("MONO_MEDIUM", "IBMPlexMono-Medium.ttf"),
]

# Code points kept in the embedded fonts: Latin-1, Latin Extended-A, Greek,
# punctuation, arrows and a few symbols tuners use. Box drawing and block
# elements are NOT embedded: they are drawn by the 8x16 bitmap font so they
# tile the cell grid exactly.
KEEP = []
KEEP += range(0x20, 0x7F)
KEEP += range(0xA0, 0x180)
KEEP += range(0x391, 0x3CA)
KEEP += [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2020, 0x2021,
         0x2022, 0x2026, 0x2030, 0x2032, 0x2033, 0x2039, 0x203A, 0x20AC, 0x2122]
KEEP += range(0x2190, 0x2196)
KEEP += [0x21B5, 0x2212, 0x2215, 0x2219, 0x221A, 0x221E, 0x2248, 0x2260, 0x2264, 0x2265,
         0x25A0, 0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x25CF, 0x2713, 0x2717, 0x2715]
KEEP = sorted(set(KEEP))


def u16(d, o):
    return struct.unpack(">H", d[o:o + 2])[0]


def s16(d, o):
    return struct.unpack(">h", d[o:o + 2])[0]


def u32(d, o):
    return struct.unpack(">I", d[o:o + 4])[0]


def tables(d):
    n = u16(d, 4)
    t = {}
    for i in range(n):
        tag, _cs, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        t[tag.decode("latin-1")] = d[off:off + ln]
    return t


def read_cmap(cm):
    """Return {codepoint: gid} from a format 4 or 12 subtable."""
    n = u16(cm, 2)
    best = None
    for i in range(n):
        pid, eid, off = struct.unpack(">HHI", cm[4 + 8 * i:12 + 8 * i])
        fmt = u16(cm, off)
        if fmt in (4, 12) and (pid, eid) in ((3, 10), (3, 1), (0, 3), (0, 4)):
            if best is None or fmt == 12:
                best = off
    m = {}
    fmt = u16(cm, best)
    if fmt == 12:
        ng = u32(cm, best + 12)
        for g in range(ng):
            s, e, sg = struct.unpack(">III", cm[best + 16 + 12 * g:best + 28 + 12 * g])
            for c in range(s, e + 1):
                m[c] = sg + c - s
        return m
    segs = u16(cm, best + 6) // 2
    ends = best + 14
    starts = ends + 2 * segs + 2
    deltas = starts + 2 * segs
    ranges = deltas + 2 * segs
    for i in range(segs):
        e, s = u16(cm, ends + 2 * i), u16(cm, starts + 2 * i)
        dl, ro = u16(cm, deltas + 2 * i), u16(cm, ranges + 2 * i)
        for c in range(s, min(e, 0xFFFE) + 1):
            if ro == 0:
                g = (c + dl) & 0xFFFF
            else:
                g = u16(cm, ranges + 2 * i + ro + 2 * (c - s))
                if g:
                    g = (g + dl) & 0xFFFF
            if g:
                m[c] = g
    return m


def build_cmap4(m):
    """Compact (3,1) format 4 cmap for {cp<=0xFFFF: gid}."""
    items = sorted((c, g) for c, g in m.items() if c < 0xFFFF)
    segs = []
    for c, g in items:
        if segs and segs[-1][1] + 1 == c and (segs[-1][2] - segs[-1][0]) == (g - c):
            segs[-1][1] = c
        else:
            segs.append([c, c, g])
    segs_out = [(s, e, (g - s) & 0xFFFF) for s, e, g in segs]
    segs_out.append((0xFFFF, 0xFFFF, 1))
    n = len(segs_out)
    p2 = 1
    while p2 * 2 <= n:
        p2 *= 2
    search = 2 * p2
    sel = p2.bit_length() - 1
    body = struct.pack(">HHH", 4, 16 + 8 * n, 0) + struct.pack(">HHHH", n * 2, search, sel, n * 2 - search)
    body += b"".join(struct.pack(">H", e) for _s, e, _d in segs_out)
    body += struct.pack(">H", 0)
    body += b"".join(struct.pack(">H", s) for s, _e, _d in segs_out)
    body += b"".join(struct.pack(">H", d) for _s, _e, d in segs_out)
    body += b"".join(struct.pack(">H", 0) for _ in segs_out)
    hdr = struct.pack(">HH", 0, 1) + struct.pack(">HHI", 3, 1, 12)
    return hdr + body


def glyph_slices(glyf, loca, long_loca, n):
    offs = []
    for i in range(n + 1):
        offs.append(u32(loca, 4 * i) if long_loca else 2 * u16(loca, 2 * i))
    return [glyf[offs[i]:offs[i + 1]] for i in range(n)]


def components(gd):
    if len(gd) < 10 or s16(gd, 0) >= 0:
        return []
    out = []
    p = 10
    while True:
        flags, gi = u16(gd, p), u16(gd, p + 2)
        out.append(gi)
        p += 4
        p += 4 if flags & 1 else 2
        if flags & 8:
            p += 2
        elif flags & 0x40:
            p += 4
        elif flags & 0x80:
            p += 8
        if not flags & 0x20:
            break
    return out


def checksum(b):
    b = b + b"\0" * (-len(b) % 4)
    return sum(struct.unpack(">%dI" % (len(b) // 4), b)) & 0xFFFFFFFF


def subset(d):
    t = tables(d)
    head = bytearray(t["head"])
    n = u16(t["maxp"], 4)
    long_loca = s16(head, 50) != 0
    slices = glyph_slices(t["glyf"], t["loca"], long_loca, n)
    cmap = read_cmap(t["cmap"])
    keep_map = {c: cmap[c] for c in KEEP if c in cmap}
    want = {0}
    stack = list(keep_map.values())
    while stack:
        g = stack.pop()
        if g in want or g >= n:
            continue
        want.add(g)
        stack.extend(components(slices[g]))
    glyf = bytearray()
    offs = []
    for g in range(n):
        offs.append(len(glyf))
        if g in want:
            glyf += slices[g]
            glyf += b"\0" * (-len(glyf) % 4)
    offs.append(len(glyf))
    use_long = len(glyf) > 0x1FFFE
    if use_long:
        loca = b"".join(struct.pack(">I", o) for o in offs)
    else:
        loca = b"".join(struct.pack(">H", o // 2) for o in offs)
    struct.pack_into(">h", head, 50, 1 if use_long else 0)
    struct.pack_into(">I", head, 8, 0)
    out = {
        "GPOS": t["GPOS"], "cmap": build_cmap4(keep_map), "glyf": bytes(glyf),
        "head": bytes(head), "hhea": t["hhea"], "hmtx": t["hmtx"],
        "loca": loca, "maxp": t["maxp"],
    }
    tags = sorted(out)
    nt = len(tags)
    p2 = 1
    while p2 * 2 <= nt:
        p2 *= 2
    hdr = struct.pack(">IHHHH", 0x00010000, nt, p2 * 16, p2.bit_length() - 1, nt * 16 - p2 * 16)
    off = 12 + 16 * nt
    recs = b""
    body = b""
    head_rec_off = None
    for i, tag in enumerate(tags):
        data = out[tag]
        if tag == "head":
            head_rec_off = 12 + 16 * i + 8
        recs += struct.pack(">4sIII", tag.encode(), checksum(data), off + len(body), len(data))
        body += data + b"\0" * (-len(data) % 4)
    font = bytearray(hdr + recs + body)
    adj = (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF
    ho = struct.unpack(">I", bytes(font[head_rec_off:head_rec_off + 4]))[0]
    struct.pack_into(">I", font, ho + 8, adj)
    return bytes(font), len(want), len(keep_map)


def c_array(name, data):
    lines = ["static const lt_u8 %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 32):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 32]) + ("," if i + 32 < len(data) else ""))
    lines.append("};")
    return "\n".join(lines)


def main():
    stats = "--stats" in sys.argv
    parts = []
    tab = []
    total = 0
    for fi, (ename, fname) in enumerate(FONTS):
        raw = open(os.path.join(FONT_DIR, fname), "rb").read()
        sub, nglyphs, ncp = subset(raw)
        total += len(sub)
        print("%-28s %7d -> %7d bytes (%d glyphs, %d code points)" % (fname, len(raw), len(sub), nglyphs, ncp))
        chunks = [sub[i:i + CHUNK] for i in range(0, len(sub), CHUNK)]
        names = []
        for ci, ch in enumerate(chunks):
            nm = "fd%d_%d" % (fi, ci)
            names.append(nm)
            parts.append(c_array(nm, ch))
        rows = ",\n".join("    { %s, %d }" % (nm, len(ch)) for nm, ch in zip(names, chunks))
        parts.append("static const lt_ttf_chunk fc%d[%d] = {\n%s\n};" % (fi, len(chunks), rows))
        tab.append((ename, fi, len(chunks)))
    print("total %d bytes" % total)
    if stats:
        return 0
    out = []
    out.append("""/* fonts_data.c - embedded IBM Plex fonts (subset). GENERATED by tools/gen_embed.py,
 * DO NOT EDIT BY HAND.
 *
 * IBM Plex Sans / IBM Plex Mono, Copyright 2017 IBM Corp., licensed under the
 * SIL Open Font License 1.1 (assets/fonts/OFL.txt). Reserved Font Name "Plex".
 * The fonts are subset (glyph outlines for Latin, Greek and a few symbols);
 * the unmodified originals are in assets/fonts/.
 *
 * Each font is split into arrays of at most %d bytes (UNIVERSAL_C section 9).
 * SPDX-License-Identifier: OFL-1.1
 */
#include "fonts_data.h"
""" % CHUNK)
    out.append("\n\n".join(parts))
    out.append("")
    out.append("static const lt_ttf_chunk *const font_tab[LT_FONT_DATA_COUNT] = {")
    out.append(",\n".join("    fc%d" % fi for _e, fi, _n in tab))
    out.append("};")
    out.append("static const int font_nchunks[LT_FONT_DATA_COUNT] = { %s };" % ", ".join(str(n) for _e, _f, n in tab))
    out.append("""
const lt_ttf_chunk *lt_font_data(int id, int *count)
{
    if (id < 0 || id >= LT_FONT_DATA_COUNT) {
        *count = 0;
        return NULL;
    }
    *count = font_nchunks[id];
    return font_tab[id];
}
""")
    with open(OUT, "w") as fh:
        fh.write("\n".join(out))
    print("wrote", os.path.relpath(OUT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
