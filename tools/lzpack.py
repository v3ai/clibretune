"""lzpack.py - the LZ77 packer and C-source helpers shared by the generators that embed data
files in the program (tools/gen_basetunes.py, tools/gen_definitions.py).

Format (decoded by lt_lzb_decode / lt_lzb_decode_hist in src/lt_lzb.c): a sequence of
    token        high nibble = literal count, low nibble = match length - 4
                 (15 = more length bytes follow; each 255 adds 255 and continues)
    literals
    distance     3 bytes little endian, back into the output (and, with a history, into the
                 history that precedes it) - absent after the last literals
    more match length bytes when the low nibble is 15
The stream ends when the output is complete.

    lzb_compress(data)            the original packer (lazy matching, hash chains of 4-byte keys);
                                  its output is part of src/tune/basetune_data.c and must not change
    lzb_pack(data, hist=b"")      a faster packer for big, highly redundant inputs: `hist` is a
                                  preset history (another file the decoder has already unpacked),
                                  so a file can be stored as its differences to a similar one
    lzb_decompress(z, n, hist)    reference decoder (round-trip checks)
    cesc_list / cstr / cbytes     C literal helpers (Universal C: string literals <= 509 chars)
"""

MINM = 5
ZBLOCK = 32000       # bytes per packed array (Universal C: objects <= 32767 bytes)
MAXDIST = 1 << 24


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


def cbytes(data, ind="    "):
    lines = []
    for k in range(0, len(data), 20):
        lines.append(ind + ",".join(str(b) for b in data[k:k + 20]) + ",")
    return "\n".join(lines)


def common_len(s, a, b, limit):
    """length of the common prefix of s[a:] and s[b:], at most limit (exact; slice compares)"""
    l = 0
    step = 64
    while l < limit:
        st = min(step, limit - l)
        if s[a + l:a + l + st] == s[b + l:b + l + st]:
            l += st
            if step < 65536:
                step *= 2
        elif st <= 8:
            while l < limit and s[a + l] == s[b + l]:
                l += 1
            return l
        else:
            step = st // 2
    return l


class _Out(object):
    def __init__(self):
        self.b = bytearray()

    def ext(self, v):
        while v >= 255:
            self.b.append(255)
            v -= 255
        self.b.append(v)

    def emit(self, data, litstart, litlen, off, mlen):
        ln = 15 if litlen >= 15 else litlen
        mn = 0 if mlen == 0 else (15 if mlen - 4 >= 15 else mlen - 4)
        self.b.append((ln << 4) | mn)
        if litlen >= 15:
            self.ext(litlen - 15)
        self.b.extend(data[litstart:litstart + litlen])
        if mlen:
            self.b.extend(bytes((off & 255, (off >> 8) & 255, (off >> 16) & 255)))
            if mlen - 4 >= 15:
                self.ext(mlen - 4 - 15)


def lzb_compress(data, maxchain=64):
    """the original base-tune packer (same output as before it moved here)"""
    n = len(data)
    o = _Out()
    heads = {}
    chain = [0] * n
    i = 0

    def insert(p):
        if p + 4 <= n:
            k = data[p:p + 4]
            chain[p] = heads.get(k, -1)
            heads[k] = p

    def best(p):
        if p + 4 > n:
            return 0, 0
        c = heads.get(data[p:p + 4], -1)
        bl = 0
        bo = 0
        tries = 0
        while c >= 0 and tries < maxchain:
            if p - c < MAXDIST:
                l = common_len(data, c, p, n - p)
                if l > bl:
                    bl, bo = l, p - c
                if l >= 4096:
                    break
            c = chain[c]
            tries += 1
        return bl, bo

    start = 0
    while i < n:
        bl, bo = best(i)
        if bl >= MINM:
            insert(i)               # lazy: a longer match one byte later wins
            bl2, _ = best(i + 1)
            if bl2 > bl + 1:
                i += 1
                continue
            o.emit(data, start, i - start, bo, bl)
            for p in range(i + 1, i + bl):
                insert(p)
            i += bl
            start = i
        else:
            insert(i)
            i += 1
    o.emit(data, start, n - start, 0, 0)
    return bytes(o.b)


def lzb_pack(data, hist=b"", key=8, maxchain=24, match_stride=16):
    """pack data with an optional preset history (decoded with lt_lzb_decode_hist).

    Same format as lzb_compress, tuned for multi-megabyte inputs that are mostly copies of the
    history: keys are `key` bytes, every history position is indexed, positions inside a match
    only every `match_stride` bytes. Not byte-compatible with lzb_compress (it need not be)."""
    s = bytes(hist) + bytes(data)
    base = len(hist)
    n = len(s)
    if n > MAXDIST:
        raise SystemExit("lzb_pack: history + data must stay below 16 MB")
    o = _Out()
    heads = {}
    chain = [-1] * n
    get = heads.get
    for p in range(0, base - key + 1):
        k = s[p:p + key]
        chain[p] = get(k, -1)
        heads[k] = p

    def insert(p):
        if p + key <= n:
            k = s[p:p + key]
            chain[p] = get(k, -1)
            heads[k] = p

    def best(p):
        if p + key > n:
            return 0, 0
        c = get(s[p:p + key], -1)
        bl = 0
        bo = 0
        tries = 0
        while c >= 0 and tries < maxchain:
            l = common_len(s, c, p, n - p)
            if l > bl:
                bl, bo = l, p - c
                if l >= 65536:
                    break
            c = chain[c]
            tries += 1
        return bl, bo

    i = base
    start = base
    while i < n:
        bl, bo = best(i)
        if bl >= key:
            insert(i)
            if bl < 256:
                bl2, _ = best(i + 1)
                if bl2 > bl + 1:
                    i += 1
                    continue
            o.emit(s, start, i - start, bo, bl)
            end = i + bl
            for p in range(i + match_stride, end, match_stride):
                insert(p)
            for p in range(max(i + 1, end - key), end):
                insert(p)
            i = end
            start = i
        else:
            insert(i)
            i += 1
    o.emit(s, start, n - start, 0, 0)
    return bytes(o.b)


def lzb_decompress(z, n, hist=b""):
    """reference decoder: n bytes from z, back references may reach into hist"""
    o = bytearray(hist)
    base = len(hist)
    i = 0
    while True:
        t = z[i]
        i += 1
        l = t >> 4
        if l == 15:
            while True:
                b = z[i]
                i += 1
                l += b
                if b != 255:
                    break
        o += z[i:i + l]
        i += l
        if len(o) - base == n:
            return bytes(o[base:])
        off = z[i] | z[i + 1] << 8 | z[i + 2] << 16
        i += 3
        m = (t & 15) + 4
        if (t & 15) == 15:
            while True:
                b = z[i]
                i += 1
                m += b
                if b != 255:
                    break
        s = len(o) - off
        if off <= 0 or s < 0:
            raise ValueError("bad distance")
        if off >= m:
            o += o[s:s + m]
        else:
            for k in range(m):
                o.append(o[s + k])


def blocks(z, size=ZBLOCK):
    return [z[k:k + size] for k in range(0, len(z), size)]
