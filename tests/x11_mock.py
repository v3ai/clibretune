#!/usr/bin/env python3
"""Fake X server for tests/test_x11.c and tools/bench_present.sh. usage: x11_mock.py SOCKET_PATH
Logs 'op=<opcode> len=<words>' per request to SOCKET_PATH.log (MIT-SHM requests also log
'shm=<minor>'). Keeps a framebuffer of the window: PutImage and ShmPutImage write into it, and
on DestroyWindow it is saved to SOCKET_PATH.fb ("W H\\n" then W*H*4 bytes B,G,R,0).
After the first image request it sends Expose (whole window), KeyPress('a'), ButtonPress(1),
ConfigureNotify (400x300), Expose and ClientMessage (WM_DELETE_WINDOW). Answers GetProperty
(RESOURCE_MANAGER = Xft.dpi 144), an XSETTINGS manager, TranslateCoordinates and GetInputFocus.
MIT-SHM 1.2 is offered in desktop mode: ShmAttach maps the client's SysV segment (shmat,
read-only) and ShmPutImage copies from it and sends the completion event.
LT_X11_MOCK_MODE:
  desktop  (default) as above
  bare     the desktop publishes nothing (no RESOURCE_MANAGER, no XSETTINGS), no MIT-SHM, and
           RANDR reports a disconnected output plus a primary 2880x1800 panel of 302x189 mm
  shmfail  like desktop, but ShmAttach fails with BadAccess (what a remote server does)
  shmforeign  like desktop, but ShmAttach maps a different segment of the same size (a client
           in a container with its own IPC namespace): the attach "works", the pixels do not
  bench    no scripted events, 256 KB maximum request (like Xorg), MIT-SHM unless
           LT_X11_MOCK_SHM=0; images are copied into the framebuffer like a server would
Exits when the client disconnects."""
import ctypes, os, socket, struct, sys

path = sys.argv[1]
MODE = os.environ.get("LT_X11_MOCK_MODE", "desktop")
BARE = MODE == "bare"
BENCH = MODE == "bench"
SHM = MODE in ("desktop", "shmfail", "shmforeign") or (BENCH and os.environ.get("LT_X11_MOCK_SHM", "1") != "0")
RR = 140                      # RANDR major opcode
RR_TS = 0x1234                # config timestamp the client must echo
SHM_MAJOR, SHM_EVENT, SHM_ERROR = 130, 65, 128
if os.path.exists(path):
    os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(1)
log = open(path + ".log", "w")
conn, _ = srv.accept()
conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)

libc = ctypes.CDLL(None, use_errno=True)
libc.shmat.restype = ctypes.c_void_p
libc.shmat.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
libc.shmdt.argtypes = [ctypes.c_void_p]
libc.shmget.argtypes = [ctypes.c_int, ctypes.c_size_t, ctypes.c_int]
libc.shmctl.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
SHM_RDONLY = 0o10000


def recvn(n):
    out = bytearray(n)
    mv = memoryview(out)
    got = 0
    while got < n:
        k = conn.recv_into(mv[got:], n - got)
        if k == 0:
            raise EOFError
        got += k
    return out


def pad(n):
    return (n + 3) & ~3


class Fb:
    """window contents, 4 bytes per pixel (B, G, R, pad)"""
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.data = bytearray(w * h * 4)
        self.c = (ctypes.c_char * len(self.data)).from_buffer(self.data)
        self.base = ctypes.addressof(self.c)

    def put(self, src_addr, src_stride, w, h, dx, dy):
        """copy w x h pixels from memory at src_addr (row stride src_stride) to (dx, dy)"""
        if dx < 0 or dy < 0 or w <= 0 or h <= 0:
            return
        w = min(w, self.w - dx)
        h = min(h, self.h - dy)
        if w <= 0 or h <= 0:
            return
        stride = self.w * 4
        if dx == 0 and w == self.w and src_stride == stride:
            ctypes.memmove(self.base + dy * stride, src_addr, h * stride)
            return
        for y in range(h):
            ctypes.memmove(self.base + (dy + y) * stride + dx * 4, src_addr + y * src_stride, w * 4)

    def save(self, fname):
        if not BENCH:      # the pad byte is undefined on the wire: compare B, G, R only
            for i in range(3, len(self.data), 4):
                self.data[i] = 0
        with open(fname, "wb") as f:
            f.write(b"%d %d\n" % (self.w, self.h))
            f.write(self.data)


try:
    h = recvn(12)
    an, ad = struct.unpack("<HH", h[6:10])
    recvn(pad(an) + pad(ad))
    vendor = b"MOCK"
    fmts = struct.pack("<BBB5x", 24, 32, 32)
    vis = struct.pack("<IBBHIII4x", 0x21, 4, 8, 256, 0xFF0000, 0xFF00, 0xFF)
    depth = struct.pack("<BxH4x", 24, 1) + vis
    screen = struct.pack("<IIIIIHHHHHHIBBBB", 0x100, 0x22, 0xFFFFFF, 0, 0,
                         640, 480, 169, 127, 1, 1, 0x21, 0, 0, 24, 1) + depth
    body = struct.pack("<IIIIHHBBBBBBBB4x", 1, 0x200000, 0x1FFFFF, 256,
                       len(vendor), 65535 if BENCH else 4096, 1, 1, 0, 0, 32, 32, 8, 255)
    body += vendor + fmts + screen
    conn.sendall(struct.pack("<BBHHH", 1, 0, 11, 0, len(body) // 4) + body)

    atoms = {}
    next_atom = 100
    window = 0x200001
    fb = Fb(1, 1)
    pixmaps = {}              # pixmap id -> Fb
    segs = {}                 # ShmSeg xid -> mapped address
    seq = 0
    sent = False
    while True:
        r = recvn(4)
        op, b1, ln = struct.unpack("<BBH", r)
        rest = recvn(ln * 4 - 4) if ln > 1 else b""
        seq = (seq + 1) & 0xFFFF
        if op == SHM_MAJOR and SHM:
            log.write("op=%d shm=%d len=%d\n" % (op, b1, ln))
        elif not BENCH or op != 72:
            log.write("op=%d len=%d\n" % (op, ln))
        if not BENCH:
            log.flush()
        image = False
        if op == 1:
            window = struct.unpack("<I", rest[0:4])[0]
            w, hh = struct.unpack("<HH", rest[12:16])
            fb = Fb(w, hh)
        elif op == 4:
            fb.save(path + ".fb")
        elif op == 16:
            nl = struct.unpack("<H", rest[0:2])[0]
            name = bytes(rest[4:4 + nl])
            atoms[name] = next_atom
            conn.sendall(struct.pack("<BBHII20x", 1, 0, seq, 0, next_atom))
            next_atom += 1
        elif op == 53:       # CreatePixmap
            pid, _drw, pw, ph = struct.unpack("<IIHH", rest[0:12])
            pixmaps[pid] = Fb(pw, ph)
        elif op == 54:       # FreePixmap
            pixmaps.pop(struct.unpack("<I", rest[0:4])[0], None)
        elif op == 73:       # GetImage (ZPixmap) of a pixmap or the window
            drw, gx, gy, gw, gh = struct.unpack("<IhhHH", rest[0:12])
            src = fb if drw == window else pixmaps.get(drw)
            data = b""
            if src is not None:
                for y in range(gh):
                    o = ((gy + y) * src.w + gx) * 4
                    data += bytes(src.data[o:o + gw * 4])
            conn.sendall(struct.pack("<BBHII20x", 1, 24, seq, len(data) // 4, 0) + data)
        elif op == 43:       # GetInputFocus (clients use it as a round trip)
            conn.sendall(struct.pack("<BBHII20x", 1, 1, seq, 0, window))
        elif op == 98:       # QueryExtension: RANDR in bare mode, MIT-SHM where offered
            nl = struct.unpack("<H", rest[0:2])[0]
            name = bytes(rest[4:4 + nl])
            if BARE and name == b"RANDR":
                info = (1, RR, 89, 147)
            elif SHM and name == b"MIT-SHM":
                info = (1, SHM_MAJOR, SHM_EVENT, SHM_ERROR)
            else:
                info = (0, 0, 0, 0)
            conn.sendall(struct.pack("<BxHIBBBB20x", 1, seq, 0, *info))
        elif op == SHM_MAJOR and SHM:
            if b1 == 0:      # ShmQueryVersion -> 1.2
                conn.sendall(struct.pack("<BBHIHHHHB15x", 1, 0, seq, 0, 1, 2, 0, 0, 2))
            elif b1 == 1:    # ShmAttach
                xid, shmid, ro = struct.unpack("<IIB", rest[0:9])
                addr = None
                if MODE == "shmforeign":      # the same id names another segment here
                    other = libc.shmget(0, 64 << 20, 0o1600)
                    if other >= 0:
                        shmid = other
                if MODE != "shmfail":
                    a = libc.shmat(shmid, None, SHM_RDONLY if ro else 0)
                    if a is not None and a != ctypes.c_void_p(-1).value:
                        addr = a
                if MODE == "shmforeign" and shmid >= 0:
                    libc.shmctl(shmid, 0, None)       # IPC_RMID: gone once detached
                if addr is None:  # BadAccess
                    conn.sendall(struct.pack("<BBHIHB21x", 0, 10, seq, shmid, 1, SHM_MAJOR))
                else:
                    segs[xid] = addr
            elif b1 == 2:    # ShmDetach
                xid = struct.unpack("<I", rest[0:4])[0]
                if xid in segs:
                    libc.shmdt(segs.pop(xid))
                else:        # BadShmSeg
                    conn.sendall(struct.pack("<BBHIHB21x", 0, SHM_ERROR, seq, xid, 2, SHM_MAJOR))
            elif b1 == 3:    # ShmPutImage
                (drw, gc, tw, th, sx, sy, sw, sh, dx, dy, dep, fmt, send, xid,
                 off) = struct.unpack("<IIHHHHHHhhBBBxII", rest[0:36])
                target = fb if drw == window else pixmaps.get(drw)
                if xid not in segs or sx + sw > tw or sy + sh > th or fmt != 2 or target is None:
                    conn.sendall(struct.pack("<BBHIHB21x", 0, SHM_ERROR, seq, xid, 3, SHM_MAJOR))
                else:
                    stride = tw * 4
                    target.put(segs[xid] + off + sy * stride + sx * 4, stride, sw, sh, dx, dy)
                    if send:
                        conn.sendall(struct.pack("<BBHIHBxII12x", SHM_EVENT, 0, seq, drw, 3,
                                                 SHM_MAJOR, xid, off))
                    image = drw == window      # the verification pixmap does not count
        elif op == RR and BARE:
            if b1 == 0:      # RRQueryVersion -> 1.5
                conn.sendall(struct.pack("<BxHIII16x", 1, seq, 0, 1, 5))
            elif b1 == 31:   # RRGetOutputPrimary
                conn.sendall(struct.pack("<BxHII20x", 1, seq, 0, 0x62))
            elif b1 == 25:   # RRGetScreenResourcesCurrent: crtcs 0x51 0x52, outputs 0x61 0x62
                data = struct.pack("<IIII", 0x51, 0x52, 0x61, 0x62)
                conn.sendall(struct.pack("<BxHIIIHHHH8x", 1, seq, len(data) // 4, 0x1000, RR_TS,
                                         2, 2, 0, 0) + data)
            elif b1 == 9:    # RRGetOutputInfo
                out, ts = struct.unpack("<II", rest[0:8])
                assert ts == RR_TS, "client must pass the config timestamp"
                if out == 0x61:      # HDMI-1: disconnected, no crtc
                    name, crtc, mmw, mmh, connection = b"HDMI-1", 0, 0, 0, 1
                else:                # eDP-1: the laptop panel
                    name, crtc, mmw, mmh, connection = b"eDP-1", 0x51, 302, 189, 0
                tail = struct.pack("<HH", 0, len(name)) + struct.pack("<I", 0x51)
                tail += name + b"\0" * (pad(len(name)) - len(name))
                conn.sendall(struct.pack("<BBHIIIIIBBHHH", 1, 0, seq, len(tail) // 4, 0x1000, crtc,
                                         mmw, mmh, connection, 0, 1, 0, 0) + tail)
            elif b1 == 20:   # RRGetCrtcInfo
                crtc, ts = struct.unpack("<II", rest[0:8])
                assert crtc == 0x51 and ts == RR_TS
                data = struct.pack("<II", 0x62, 0x62)
                conn.sendall(struct.pack("<BBHIIhhHHIHHHH", 1, 0, seq, len(data) // 4, 0x1000,
                                         0, 0, 2880, 1800, 0x71, 1, 63, 1, 1) + data)
            else:            # unknown RANDR request: BadRequest
                conn.sendall(struct.pack("<BBHIHB21x", 0, 1, seq, 0, b1, RR))
        elif op == 23:       # GetSelectionOwner: we own _XSETTINGS_S0 with window 0x55
            sel = struct.unpack("<I", rest[0:4])[0]
            owner = 0x55 if (sel == atoms.get(b"_XSETTINGS_S0") and not BARE) else 0
            conn.sendall(struct.pack("<BBHII20x", 1, 0, seq, 0, owner))
        elif op == 20:
            prop = struct.unpack("<I", rest[4:8])[0]
            if prop == atoms.get(b"_XSETTINGS_SETTINGS"):
                def xs_int(name, v):
                    return (struct.pack("<BBH", 0, 0, len(name)) + name + b"\0" * (pad(len(name)) - len(name))
                            + struct.pack("<Ii", 0, v))
                def xs_str(name, v):
                    return (struct.pack("<BBH", 1, 0, len(name)) + name + b"\0" * (pad(len(name)) - len(name))
                            + struct.pack("<II", 0, len(v)) + v + b"\0" * (pad(len(v)) - len(v)))
                body = (xs_str(b"Net/ThemeName", b"Adwaita") + xs_int(b"Xft/DPI", 192 * 1024)
                        + xs_int(b"Gdk/WindowScalingFactor", 2))
                val = struct.pack("<BxxxII", 0, 1, 3) + body
                conn.sendall(struct.pack("<BBHIIII12x", 1, 8, seq, len(val) // 4, prop, 0, len(val)) + val)
            elif prop == 23 and not BARE:   # RESOURCE_MANAGER on the root
                val = b"*.background:\tblack\nXft.dpi:\t144\nXft.antialias:\t1\n"
                data = val + b"\0" * (pad(len(val)) - len(val))
                conn.sendall(struct.pack("<BBHIIII12x", 1, 8, seq, len(data) // 4, 31, 0, len(val)) + data)
            else:            # no such property
                conn.sendall(struct.pack("<BBHIIII12x", 1, 0, seq, 0, 0, 0, 0))
        elif op == 40:       # TranslateCoordinates -> window origin (7, 9)
            conn.sendall(struct.pack("<BBHIIhh16x", 1, 1, seq, 0, 0, 7, 9))
        elif op == 101:
            first, count = rest[0], rest[1]
            kpk = 2
            data = b""
            for kc in range(first, first + count):
                if kc == 38:
                    data += struct.pack("<II", 0x61, 0x41)
                else:
                    data += struct.pack("<II", 0, 0)
            conn.sendall(struct.pack("<BBHI24x", 1, kpk, seq, len(data) // 4) + data)
        elif op == 72:       # PutImage (ZPixmap, depth 24, 32 bpp)
            w, hh, dx, dy = struct.unpack("<HHhh", rest[8:16])
            if b1 == 2 and len(rest) - 20 >= w * hh * 4:
                img = (ctypes.c_char * len(rest)).from_buffer(rest)
                fb.put(ctypes.addressof(img) + 20, w * 4, w, hh, dx, dy)
                del img
            image = True
        if image and not sent and not BENCH:
            sent = True
            # Expose of the whole window: the client answers it itself (it presented a full
            # frame) with another image request
            ev = struct.pack("<BxHIHHHHH14x", 12, seq, window, 0, 0, fb.w, fb.h, 0)
            ev += struct.pack("<BBHIIIIhhhhHBx", 2, 38, seq, 1, 0x100, window, 0, 5, 5, 5, 5, 0, 1)
            ev += struct.pack("<BBHIIIIhhhhHBx", 4, 1, seq, 2, 0x100, window, 0, 10, 20, 10, 20, 0, 1)
            # the window grows: the Expose after it must reach the app (nothing covers 400x300)
            ev += struct.pack("<BxHIIIhhHHHB5x", 22, seq, window, window, 0, 30, 40, 400, 300, 0, 0)
            ev += struct.pack("<BxHIHHHHH14x", 12, seq, window, 0, 0, 400, 300, 0)
            ev += struct.pack("<BBHII", 33, 32, seq, window, atoms[b"WM_PROTOCOLS"])
            ev += struct.pack("<I", atoms[b"WM_DELETE_WINDOW"]) + b"\0" * 16
            conn.sendall(ev)
except (EOFError, ConnectionResetError):   # reset: the client closed with events unread
    pass
log.close()
for a in segs.values():
    libc.shmdt(a)
conn.close()
os.unlink(path)
