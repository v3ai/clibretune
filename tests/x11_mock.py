#!/usr/bin/env python3
"""Fake X server for tests/test_x11.c. usage: x11_mock.py SOCKET_PATH
Logs 'op=<opcode> len=<words>' per request to SOCKET_PATH.log. After the first
PutImage it sends Expose, KeyPress('a'), ButtonPress(1) and ClientMessage
(WM_DELETE_WINDOW). Answers GetProperty (RESOURCE_MANAGER = Xft.dpi 144), an XSETTINGS manager
and TranslateCoordinates.
With LT_X11_MOCK_MODE=bare the desktop publishes nothing (no RESOURCE_MANAGER, no XSETTINGS)
and the RANDR extension reports a disconnected output plus a primary 2880x1800 panel of
302x189 mm, as a HiDPI laptop running a bare window manager would.
Exits when the client disconnects."""
import os, socket, struct, sys

path = sys.argv[1]
BARE = os.environ.get("LT_X11_MOCK_MODE") == "bare"
RR = 140                      # RANDR major opcode
RR_TS = 0x1234                # config timestamp the client must echo
if os.path.exists(path):
    os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(1)
log = open(path + ".log", "w")
conn, _ = srv.accept()

def recvn(n):
    d = b""
    while len(d) < n:
        c = conn.recv(n - len(d))
        if not c:
            raise EOFError
        d += c
    return d

def pad(n):
    return (n + 3) & ~3

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
                       len(vendor), 4096, 1, 1, 0, 0, 32, 32, 8, 255)
    body += vendor + fmts + screen
    conn.sendall(struct.pack("<BBHHH", 1, 0, 11, 0, len(body) // 4) + body)

    atoms = {}
    next_atom = 100
    window = 0x200001
    seq = 0
    sent = False
    while True:
        r = recvn(4)
        op, b1, ln = struct.unpack("<BBH", r)
        rest = recvn(ln * 4 - 4) if ln > 1 else b""
        seq = (seq + 1) & 0xFFFF
        log.write("op=%d len=%d\n" % (op, ln))
        log.flush()
        if op == 1:
            window = struct.unpack("<I", rest[0:4])[0]
        elif op == 16:
            nl = struct.unpack("<H", rest[0:2])[0]
            name = rest[4:4 + nl]
            atoms[name] = next_atom
            conn.sendall(struct.pack("<BBHII20x", 1, 0, seq, 0, next_atom))
            next_atom += 1
        elif op == 98:       # QueryExtension: only RANDR, only in bare mode
            nl = struct.unpack("<H", rest[0:2])[0]
            present = 1 if (BARE and rest[4:4 + nl] == b"RANDR") else 0
            conn.sendall(struct.pack("<BxHIBBBB20x", 1, seq, 0, present, RR if present else 0,
                                     89 if present else 0, 147 if present else 0))
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
        elif op == 72 and not sent:
            sent = True
            ev = struct.pack("<BxHIHHHHH14x", 12, seq, window, 0, 0, 640, 480, 0)
            ev += struct.pack("<BBHIIIIhhhhHBx", 2, 38, seq, 1, 0x100, window, 0, 5, 5, 5, 5, 0, 1)
            ev += struct.pack("<BBHIIIIhhhhHBx", 4, 1, seq, 2, 0x100, window, 0, 10, 20, 10, 20, 0, 1)
            ev += struct.pack("<BBHII", 33, 32, seq, window, atoms[b"WM_PROTOCOLS"])
            ev += struct.pack("<I", atoms[b"WM_DELETE_WINDOW"]) + b"\0" * 16
            conn.sendall(ev)
except EOFError:
    pass
log.close()
conn.close()
os.unlink(path)
