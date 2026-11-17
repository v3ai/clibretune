#!/usr/bin/env python3
"""ppm2png.py - convert binary PPM (P6) to PNG, optionally scaled up.

    python3 tools/ppm2png.py in.ppm out.png [zoom]
"""
import struct
import sys
import zlib


def read_ppm(path):
    d = open(path, "rb").read()
    pos = 0
    toks = []
    while len(toks) < 4:
        while d[pos:pos + 1].isspace():
            pos += 1
        if d[pos:pos + 1] == b"#":
            while d[pos:pos + 1] != b"\n":
                pos += 1
            continue
        e = pos
        while not d[e:e + 1].isspace():
            e += 1
        toks.append(d[pos:e])
        pos = e
    pos += 1
    w, h = int(toks[1]), int(toks[2])
    return w, h, d[pos:pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    w, h, rgb = read_ppm(sys.argv[1])
    z = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    if z > 1:
        rows = []
        for y in range(h):
            row = b"".join(rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] * z for x in range(w))
            rows.extend([row] * z)
        rgb = b"".join(rows)
        w *= z
        h *= z
    write_png(sys.argv[2], w, h, rgb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
