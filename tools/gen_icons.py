#!/usr/bin/env python3
"""gen_icons.py - convert Lucide SVG icons to compact polyline data in C.

    python3 tools/gen_icons.py                       regenerate from assets/icons/*.svg
    python3 tools/gen_icons.py --fetch LUCIDE_DIR    first copy the wanted SVGs from an
                                                     extracted lucide-static package
                                                     (LUCIDE_DIR/icons/*.svg)

Output: src/gui/icons.h       LT_ICON_* ids (macros: C89 allows only 127 enumerators)
        src/gui/icons_data.c  polyline arrays + name/alias tables

Lucide (https://lucide.dev, ISC license, see assets/icons/LICENSE) icons are
24x24, stroke-only, round caps and joins. Supported SVG elements: path (all
commands incl. arcs, flattened to segments), line, polyline, polygon, rect
(with rx/ry), circle, ellipse. Curves are flattened with 0.02 unit tolerance;
coordinates are stored as signed 16-bit in 1/32 icon units.

Data per icon: a list of polylines { header, x0, y0, x1, y1, ... } terminated
by a header of 0. header = point count | 0x4000 when the polyline is closed
(the last point connects back to the first; the first point is not repeated).
"""
import math
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(ROOT, "assets", "icons")
OUT_H = os.path.join(ROOT, "src", "gui", "icons.h")
OUT_C = os.path.join(ROOT, "src", "gui", "icons_data.c")
SCALE = 32          # stored units per icon unit
TOL = 0.02          # flattening tolerance in icon units

# every icon the original LibreTune UI imports from lucide-react (React name)
REACT = """Activity AlertCircle AlertTriangle AlignCenterHorizontal AlignCenterVertical
AlignEndHorizontal AlignEndVertical AlignStartHorizontal AlignStartVertical ArrowDown ArrowLeft
ArrowLeftToLine ArrowRight ArrowUpDown BarChart3 BatteryCharging Book BookOpen Bot Box Cable
Calculator Car Check CheckCircle CheckCircle2 CheckSquare ChevronDown ChevronLeft ChevronRight
ChevronUp Circle CircleAlert CircleDot CircuitBoard Clipboard ClipboardPaste Clock Compass Copy
Cpu Crosshair Disc3 Download Droplet Droplets Equal ExternalLink Fan FileArchive FileDown
FilePlus FileText FileUp Flame Folder FolderOpen FolderPlus Fuel Gamepad2 Gauge GitBranch
GitCompare GitMerge Globe Grid3X3 HelpCircle Home Hourglass Info Key KeyRound Keyboard Layers
LayoutList Lightbulb LineChart ListOrdered Loader Loader2 Lock LockOpen MapPin Maximize2
MemoryStick Minimize2 Minus MoreHorizontal Move MoveHorizontal MoveVertical Network Package
Palette Pause Pencil Play PlayCircle Plug Plus Power Puzzle Radio Redo2 RefreshCw Rocket
RotateCcw RotateCw Route Save Scale Scaling ScrollText Search Settings Settings2 ShieldAlert
ShieldCheck Sliders Snowflake Sparkles Square Stethoscope Table2 Thermometer Timer ToggleLeft
Trash2 TrendingUp Undo2 Unplug Upload Usb Wand2 Waves Wifi WifiOff Wind Wrench X XCircle Zap
ZoomIn ZoomOut""".split()

# React names whose lucide file was renamed
RENAMED = {
    "AlertCircle": "circle-alert", "AlertTriangle": "triangle-alert",
    "BarChart3": "chart-column", "CheckCircle": "circle-check-big",
    "CheckCircle2": "circle-check", "CheckSquare": "square-check-big",
    "Grid3X3": "grid-3x3", "HelpCircle": "circle-help", "Home": "house",
    "LineChart": "chart-line", "Loader2": "loader-circle",
    "MoreHorizontal": "ellipsis", "PlayCircle": "circle-play",
    "Sliders": "sliders-vertical", "Wand2": "wand-sparkles", "XCircle": "circle-x",
}

# generic extras beyond what the original imports (lucide file names)
EXTRA = """file file-plus file-code folder-plus table chart-bar chart-no-axes-column settings-2
save undo-2 redo-2 undo redo search chevrons-up-down chevrons-down chevrons-up chevrons-left
chevrons-right play pause circle-stop circle lock-open lock refresh-cw box boxes cpu zap
info check x menu ellipsis ellipsis-vertical eye eye-off bell terminal database hard-drive
filter funnel star trash pin link unlink ruler binary code square-pen eraser percent
layout-dashboard panel-left panel-right maximize minimize sun moon arrow-up arrow-down
arrow-left arrow-right list sliders-horizontal scissors history bug rotate-ccw rotate-cw
thermometer-sun gauge flame battery plug-zap wrench bookmark tag target crosshair
chevron-first chevron-last circle-check circle-x circle-help circle-alert triangle-alert
square-check square-x grip-vertical grip-horizontal move-3d scan-line layers-2 image
printer share-2 upload download wand-sparkles sparkles""".split()


def kebab(n):
    s = re.sub(r"([a-z])([A-Z0-9])", r"\1-\2", n)
    s = re.sub(r"([0-9])([A-Z])", r"\1-\2", s)
    return s.lower()


def norm_key(n):
    return re.sub(r"[-_ ]", "", n).lower()


def wanted():
    """ordered {kebab name: set(alias keys)}"""
    names = {}
    for r in REACT:
        k = RENAMED.get(r, kebab(r))
        names.setdefault(k, set()).update([norm_key(r), norm_key(k)])
    for k in EXTRA:
        names.setdefault(k, set()).add(norm_key(k))
    return names


# ------------------------------------------------------------------ SVG
def fnum(s, default=0.0):
    return float(s) if s not in (None, "") else default


def attrs_of(tag):
    return dict(re.findall(r'([\w:-]+)\s*=\s*"([^"]*)"', tag))


def flatten_cubic(p0, p1, p2, p3, out):
    # control polygon length bound -> segment count
    d = (math.dist(p0, p1) + math.dist(p1, p2) + math.dist(p2, p3))
    dd = max(abs(p0[0] - 2 * p1[0] + p2[0]), abs(p0[1] - 2 * p1[1] + p2[1]),
             abs(p1[0] - 2 * p2[0] + p3[0]), abs(p1[1] - 2 * p2[1] + p3[1]))
    n = max(2, int(math.ceil(math.sqrt(3.0 * dd / (4.0 * TOL)))) + 1)
    n = min(n, 40)
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        out.append((mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t ** 3 * p3[0],
                    mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t ** 3 * p3[1]))


def flatten_quad(p0, p1, p2, out):
    dd = math.hypot(p0[0] - 2 * p1[0] + p2[0], p0[1] - 2 * p1[1] + p2[1])
    n = max(2, int(math.ceil(math.sqrt(dd / (4.0 * TOL)))) + 1)
    n = min(n, 40)
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        out.append((mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0],
                    mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]))


def flatten_arc(p0, rx, ry, rot, large, sweep, p1, out):
    """SVG endpoint arc (F.6.5) -> polyline appended to out (excluding p0)."""
    x1, y1 = p0
    x2, y2 = p1
    if (x1, y1) == (x2, y2):
        return
    rx, ry = abs(rx), abs(ry)
    if rx == 0 or ry == 0:
        out.append(p1)
        return
    phi = math.radians(rot)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2.0, (y1 - y2) / 2.0
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx *= s
        ry *= s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2.0
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2.0

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dth > 0:
        dth -= 2 * math.pi
    elif sweep and dth < 0:
        dth += 2 * math.pi
    r = max(rx, ry)
    step = 2.0 * math.acos(max(0.0, 1.0 - TOL / r)) if r > TOL else math.pi / 2
    n = max(2, int(math.ceil(abs(dth) / step)))
    n = min(n, 96)
    for i in range(1, n + 1):
        t = th1 + dth * i / n
        ex, ey = rx * math.cos(t), ry * math.sin(t)
        out.append((cp * ex - sp * ey + cx, sp * ex + cp * ey + cy))
    out[-1] = p1


NUM = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


class Scanner:
    """Tiny SVG path scanner (numbers may be glued: '1.5.5', '0-3', arc flags '011')."""

    def __init__(self, d):
        self.d = d
        self.i = 0

    def ws(self):
        while self.i < len(self.d) and self.d[self.i] in " \t\r\n,":
            self.i += 1

    def cmd(self):
        self.ws()
        if self.i < len(self.d) and self.d[self.i].isalpha():
            c = self.d[self.i]
            self.i += 1
            return c
        return None

    def more(self):
        self.ws()
        return self.i < len(self.d)

    def num(self):
        self.ws()
        m = NUM.match(self.d, self.i)
        if not m:
            raise ValueError("bad path near %r" % self.d[self.i:self.i + 12])
        self.i = m.end()
        return float(m.group(0))

    def flag(self):
        self.ws()
        c = self.d[self.i]
        if c not in "01":
            raise ValueError("bad arc flag")
        self.i += 1
        return int(c)


def parse_path(d):
    """-> list of (points, closed)"""
    sc = Scanner(d)
    subs = []
    cur = None
    pos = (0.0, 0.0)
    start = (0.0, 0.0)
    cmd = None
    last_c = None   # last cubic control (for S)
    last_q = None   # last quad control (for T)
    num = sc.num
    while sc.more():
        c0 = sc.cmd()
        if c0 is not None:
            cmd = c0
            if cmd in "Zz":
                if cur is not None:
                    subs.append((cur, True))
                cur = None
                pos = start
                last_c = last_q = None
                continue
        elif cmd is None or cmd in "Zz":
            raise ValueError("path data without command")
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            x, y = num(), num()
            if rel:
                x += pos[0]
                y += pos[1]
            if cur is not None:
                subs.append((cur, False))
            pos = start = (x, y)
            cur = [pos]
            cmd = "l" if rel else "L"
            last_c = last_q = None
            continue
        if cur is None:
            cur = [pos]
        if c == "L":
            x, y = num(), num()
            if rel:
                x += pos[0]
                y += pos[1]
            pos = (x, y)
            cur.append(pos)
            last_c = last_q = None
        elif c == "H":
            x = num()
            pos = (x + pos[0] if rel else x, pos[1])
            cur.append(pos)
            last_c = last_q = None
        elif c == "V":
            y = num()
            pos = (pos[0], y + pos[1] if rel else y)
            cur.append(pos)
            last_c = last_q = None
        elif c == "C":
            v = [num() for _ in range(6)]
            if rel:
                v = [v[k] + pos[k % 2] for k in range(6)]
            flatten_cubic(pos, (v[0], v[1]), (v[2], v[3]), (v[4], v[5]), cur)
            last_c = (v[2], v[3])
            last_q = None
            pos = (v[4], v[5])
        elif c == "S":
            v = [num() for _ in range(4)]
            if rel:
                v = [v[k] + pos[k % 2] for k in range(4)]
            c1 = (2 * pos[0] - last_c[0], 2 * pos[1] - last_c[1]) if last_c else pos
            flatten_cubic(pos, c1, (v[0], v[1]), (v[2], v[3]), cur)
            last_c = (v[0], v[1])
            last_q = None
            pos = (v[2], v[3])
        elif c == "Q":
            v = [num() for _ in range(4)]
            if rel:
                v = [v[k] + pos[k % 2] for k in range(4)]
            flatten_quad(pos, (v[0], v[1]), (v[2], v[3]), cur)
            last_q = (v[0], v[1])
            last_c = None
            pos = (v[2], v[3])
        elif c == "T":
            v = [num() for _ in range(2)]
            if rel:
                v = [v[k] + pos[k % 2] for k in range(2)]
            q1 = (2 * pos[0] - last_q[0], 2 * pos[1] - last_q[1]) if last_q else pos
            flatten_quad(pos, q1, (v[0], v[1]), cur)
            last_q = q1
            last_c = None
            pos = (v[0], v[1])
        elif c == "A":
            rx, ry, rot = num(), num(), num()
            large, sweep = sc.flag(), sc.flag()
            x, y = num(), num()
            if rel:
                x += pos[0]
                y += pos[1]
            flatten_arc(pos, rx, ry, rot, large, sweep, (x, y), cur)
            pos = (x, y)
            last_c = last_q = None
        else:
            raise ValueError("unsupported path command " + cmd)
    if cur is not None:
        subs.append((cur, False))
    return subs


def ellipse_poly(cx, cy, rx, ry):
    pts = []
    r = max(rx, ry)
    step = 2.0 * math.acos(max(0.0, 1.0 - TOL / r)) if r > TOL else math.pi / 2
    n = max(8, int(math.ceil(2 * math.pi / step)))
    n = min(n, 96)
    for i in range(n):
        t = 2 * math.pi * i / n
        pts.append((cx + rx * math.cos(t), cy + ry * math.sin(t)))
    return pts


def rect_poly(x, y, w, h, rx, ry):
    if rx <= 0 and ry <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    if rx <= 0:
        rx = ry
    if ry <= 0:
        ry = rx
    rx = min(rx, w / 2)
    ry = min(ry, h / 2)
    pts = [(x + rx, y)]
    pts.append((x + w - rx, y))
    flatten_arc((x + w - rx, y), rx, ry, 0, 0, 1, (x + w, y + ry), pts)
    pts.append((x + w, y + h - ry))
    flatten_arc((x + w, y + h - ry), rx, ry, 0, 0, 1, (x + w - rx, y + h), pts)
    pts.append((x + rx, y + h))
    flatten_arc((x + rx, y + h), rx, ry, 0, 0, 1, (x, y + h - ry), pts)
    pts.append((x, y + ry))
    flatten_arc((x, y + ry), rx, ry, 0, 0, 1, (x + rx, y), pts)
    pts.pop()   # last point equals the first
    return pts


def parse_svg(text, name):
    subs = []
    body = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    if re.search(r"<(g|use|defs|mask|clipPath)\b|transform=", body):
        raise ValueError("%s: unsupported SVG structure" % name)
    for m in re.finditer(r"<(path|line|polyline|polygon|rect|circle|ellipse)\b([^>]*?)/?>", body, flags=re.S):
        kind, a = m.group(1), attrs_of(m.group(2))
        if kind == "path":
            subs.extend(parse_path(a["d"]))
        elif kind == "line":
            subs.append(([(fnum(a.get("x1")), fnum(a.get("y1"))),
                          (fnum(a.get("x2")), fnum(a.get("y2")))], False))
        elif kind in ("polyline", "polygon"):
            nums = [float(v) for v in re.findall(r"-?(?:\d+\.?\d*|\.\d+)", a["points"])]
            pts = list(zip(nums[0::2], nums[1::2]))
            subs.append((pts, kind == "polygon"))
        elif kind == "rect":
            subs.append((rect_poly(fnum(a.get("x")), fnum(a.get("y")), fnum(a["width"]),
                                   fnum(a["height"]), fnum(a.get("rx")), fnum(a.get("ry"))), True))
        elif kind == "circle":
            r = fnum(a["r"])
            subs.append((ellipse_poly(fnum(a["cx"]), fnum(a["cy"]), r, r), True))
        elif kind == "ellipse":
            subs.append((ellipse_poly(fnum(a["cx"]), fnum(a["cy"]), fnum(a["rx"]), fnum(a["ry"])), True))
    return subs


def quantize(subs):
    out = []
    for pts, closed in subs:
        q = []
        for x, y in pts:
            p = (int(round(x * SCALE)), int(round(y * SCALE)))
            if not q or q[-1] != p:
                q.append(p)
        if closed and len(q) > 1 and q[0] == q[-1]:
            q.pop()
        if len(q) == 1:
            q.append(q[0])      # a dot: zero-length segment with round caps
            closed = False
        if len(q) < 2 and not closed:
            continue
        out.append((q, closed))
    return out


def macro_name(kebab_name):
    return "LT_ICON_" + kebab_name.upper().replace("-", "_")


def main():
    names = wanted()
    if "--fetch" in sys.argv:
        src = sys.argv[sys.argv.index("--fetch") + 1]
        os.makedirs(ICON_DIR, exist_ok=True)
        for k in sorted(names):
            p = os.path.join(src, "icons", k + ".svg")
            if not os.path.exists(p):
                sys.exit("lucide icon not found: " + k)
            shutil.copyfile(p, os.path.join(ICON_DIR, k + ".svg"))
        lic = os.path.join(src, "LICENSE")
        if os.path.exists(lic):
            shutil.copyfile(lic, os.path.join(ICON_DIR, "LICENSE"))
    order = sorted(names)
    icons = []
    for k in order:
        p = os.path.join(ICON_DIR, k + ".svg")
        if not os.path.exists(p):
            sys.exit("missing %s (run with --fetch)" % p)
        subs = quantize(parse_svg(open(p).read(), k))
        for pts, closed in subs:
            for x, y in pts:
                assert -64 <= x <= 24 * SCALE + 64 and -64 <= y <= 24 * SCALE + 64, k
        icons.append((k, subs))
    # ---- header
    h = ["/* icons.h - Lucide icon ids. GENERATED by tools/gen_icons.py, DO NOT EDIT.",
         " *",
         " * Icons are from Lucide (https://lucide.dev), ISC license, see",
         " * assets/icons/LICENSE. Ids are macros because C89 limits an enum to 127",
         " * enumerators. 0 = LT_ICON_NONE; lt_icon_find() maps lucide names",
         " * (\"folder-open\") and lucide-react names (\"FolderOpen\") to ids.",
         " * SPDX-License-Identifier: ISC",
         " */",
         "#ifndef LT_ICONS_H",
         "#define LT_ICONS_H",
         "",
         "#define LT_ICON_NONE 0"]
    seen = set()
    for i, (k, _s) in enumerate(icons, 1):
        m = macro_name(k)
        assert len(m) <= 31, m
        assert m[:31] not in seen
        seen.add(m[:31])
        h.append("#define %s %d" % (m, i))
    h.append("#define LT_ICON_COUNT %d   /* ids 1..LT_ICON_COUNT */" % (len(icons) + 1))
    h.append("")
    h.append("#endif /* LT_ICONS_H */")
    h.append("")
    open(OUT_H, "w").write("\n".join(h))
    # ---- data
    c = ["/* icons_data.c - Lucide icon polylines. GENERATED by tools/gen_icons.py, DO NOT EDIT.",
         " *",
         " * Source: Lucide (https://lucide.dev), ISC license (assets/icons/LICENSE).",
         " * Format: see tools/gen_icons.py. Coordinates are 1/%d icon units in a 24x24 box." % SCALE,
         " * SPDX-License-Identifier: ISC",
         " */",
         '#include "gui.h"',
         '#include "gfx_priv.h"',
         ""]
    total = 0
    for i, (k, subs) in enumerate(icons, 1):
        words = []
        for pts, closed in subs:
            words.append(len(pts) | (0x4000 if closed else 0))
            for x, y in pts:
                words += [x, y]
        words.append(0)
        total += 2 * len(words)
        assert 2 * len(words) < 32000, k
        lines = []
        for j in range(0, len(words), 16):
            lines.append("    " + ",".join(str(w) for w in words[j:j + 16]))
        c.append("/* %s */\nstatic const lt_s16 icd%03d[%d] = {\n%s\n};" % (k, i, len(words), ",\n".join(lines)))
    c.append("")
    c.append("static const lt_s16 *const icon_tab[LT_ICON_COUNT] = {\n    NULL,")
    c.append(",\n".join("    icd%03d" % i for i in range(1, len(icons) + 1)))
    c.append("};")
    c.append("")
    c.append("static const char *const icon_names[LT_ICON_COUNT] = {\n    \"\",")
    c.append(",\n".join('    "%s"' % k for k, _s in icons))
    c.append("};")
    c.append("")
    alias = []
    for i, (k, _s) in enumerate(icons, 1):
        for a in sorted(names[k]):
            alias.append((a, i))
    alias.sort()
    c.append("/* lookup keys: lower case, without '-' or '_'; sorted for bsearch */")
    c.append("static const char *const alias_key[%d] = {" % len(alias))
    c.append(",\n".join('    "%s"' % a for a, _i in alias))
    c.append("};")
    c.append("static const lt_u16 alias_id[%d] = {" % len(alias))
    for j in range(0, len(alias), 16):
        c.append("    " + ",".join(str(i) for _a, i in alias[j:j + 16]) + ("," if j + 16 < len(alias) else ""))
    c.append("};")
    c.append("""
const lt_s16 *lt_icon_data(int id)
{
    if (id <= 0 || id >= LT_ICON_COUNT) { return NULL; }
    return icon_tab[id];
}

const char *lt_icon_name(int id)
{
    if (id <= 0 || id >= LT_ICON_COUNT) { return NULL; }
    return icon_names[id];
}

int lt_icon_count(void)
{
    return LT_ICON_COUNT - 1;
}

int lt_icon_alias_count(void)
{
    return %d;
}

const char *lt_icon_alias_at(int i, int *id)
{
    if (i < 0 || i >= %d) { return NULL; }
    *id = (int)alias_id[i];
    return alias_key[i];
}
""" % (len(alias), len(alias)))
    open(OUT_C, "w").write("\n".join(c))
    print("%d icons, %d alias keys, %d bytes of coordinates" % (len(icons), len(alias), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
