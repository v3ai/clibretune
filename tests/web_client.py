#!/usr/bin/env python3
"""Drives `test_web serve PORT`: fetches the page, a frame, posts input,
decodes the RLE frame and compares its checksum with the C side.
Usage: web_client.py path/to/test_web [port]"""
import http.client, re, struct, subprocess, sys, threading, time

def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h

def rle_decode(buf, npx):
    out = bytearray(); i = 0
    while i < len(buf):
        c = buf[i]; i += 1
        if c & 0x80:
            out += buf[i:i+4] * ((c & 0x7F) + 1); i += 4
        else:
            n = (c + 1) * 4; out += buf[i:i+n]; i += n
    assert len(out) == npx * 4, (len(out), npx)
    return out

def parse_frame(body, canvas=None):
    seq, w, h, nt, tl = struct.unpack_from('<IHHHH', body, 0)
    o = 12; title = body[o:o+tl].decode(); o += tl
    if canvas is None:
        canvas = bytearray(w * h * 4)
    for _ in range(nt):
        x, y, tw, th, el = struct.unpack_from('<HHHHI', body, o); o += 12
        px = rle_decode(body[o:o+el], tw * th); o += el
        for r in range(th):
            d = ((y + r) * w + x) * 4
            canvas[d:d+tw*4] = px[r*tw*4:(r+1)*tw*4]
    assert o == len(body)
    return seq, w, h, nt, title, canvas

def main():
    exe = sys.argv[1]; port = int(sys.argv[2]) if len(sys.argv) > 2 else 18710
    p = subprocess.Popen([exe, 'serve', str(port)], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    err_lines = []; out_lines = []
    threading.Thread(target=lambda: [err_lines.append(l) for l in p.stderr], daemon=True).start()
    threading.Thread(target=lambda: [out_lines.append(l.strip()) for l in p.stdout], daemon=True).start()
    def wait(pred, t=5):
        end = time.time() + t
        while time.time() < end:
            for l in list(err_lines) + list(out_lines):
                m = pred(l)
                if m: return m
            time.sleep(0.05)
        raise SystemExit('timeout waiting for server output: %r %r' % (err_lines, out_lines))
    m = wait(lambda l: re.search(r'http://127\.0\.0\.1:(\d+)/\?t=([0-9a-f]+)', l))
    port, token = int(m.group(1)), m.group(2)
    sum1 = wait(lambda l: l.startswith('CHECKSUM ') and l.split()[1])
    ok = True
    def check(c, msg):
        nonlocal ok
        print(('ok   ' if c else 'FAIL ') + msg)
        ok = ok and c
    def req(method, path, body=None, headers=None):
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        c.request(method, path, body, headers or {})
        r = c.getresponse(); d = r.read(); return r, d
    r, _ = req('GET', '/'); check(r.status == 403, 'page without token rejected')
    r, _ = req('GET', '/', headers={'Host': 'evil.com'}); check(r.status == 403, 'bad Host rejected')
    r, d = req('GET', '/?t=' + token)
    check(r.status == 200 and b'<canvas' in d, 'page served')
    cookie = r.getheader('Set-Cookie').split(';')[0]
    h = {'Cookie': cookie}
    r, d = req('GET', '/frame?since=0', headers=h)
    check(r.status == 200, 'frame 200')
    seq, w, hh, nt, title, canvas = parse_frame(d)
    check((w, hh, nt) == (150, 100, 6), 'frame header %r' % ((w, hh, nt),))
    check(title == 'Serve Title', 'title')
    check('%08x' % fnv(canvas) == sum1, 'checksum matches decoded canvas')
    t = time.time(); r, d = req('GET', '/frame?since=%d' % seq, headers=h)
    check(r.status == 204 and 0.8 < time.time() - t < 2.5, 'long-poll times out with 204')
    body = 'mm 7 8\nmd 12 34 1\nmu 12 34 1\nkd 256 2\nku 256 2\ntx \xc3\xa9\nwh 5 5 -1\nrs 800 600\nfo\n'
    r, _ = req('POST', '/input', body.encode('latin-1'), h); check(r.status == 204, 'input accepted')
    sum2 = wait(lambda l: l.startswith('CHECKSUM2 ') and l.split()[1])
    r, d = req('GET', '/frame?since=%d' % seq, headers=h)
    seq2, w, hh, nt2, title, canvas = parse_frame(d, canvas)
    check(r.status == 200 and seq2 > seq and nt2 == 1, 'incremental frame has 1 dirty tile (%d)' % nt2)
    check('%08x' % fnv(canvas) == sum2, 'checksum after incremental update')
    rc = p.wait(timeout=15)
    wait(lambda l: l == 'DONE 1')
    check(rc == 0, 'C side received all events')
    print('\n'.join(l for l in out_lines if l.startswith('EVENT')))
    sys.exit(0 if ok else 1)

main()
