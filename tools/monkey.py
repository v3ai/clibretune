#!/usr/bin/env python3
"""tools/monkey.py - seeded random ("monkey") input to the whole app under ASan+UBSan.

    tools/monkey.py [options]                 run N random sessions, report unique failures
    tools/monkey.py --repro FILE.json         rerun one recorded failure, print its log
    tools/monkey.py --minimize FILE.json      delta-debug its token stream, keeping the failure

Every run is generated from one integer seed: the starting point (start screen, --demo with a
random bundled or fixture INI, an offline INI, a project with hostile files, an INI menu target
or dlg:<kind> opened first), window size and LT_SCALE, hostile config files, and a token stream
for LT_SHOT_SCRIPT (pixel UI) or LT_SHOT_KEYS (terminal UI) biased toward clicks, menu
accelerators, Enter/Esc and odd text. Each run gets a fresh directory (config, HOME, projects,
definitions, downloads) under --out, so the real configuration is never touched; a fake xdg-open
logs URLs instead of opening a browser.

A run fails on a sanitizer report (ASan, UBSan, LeakSanitizer), a signal, a nonzero exit or a
timeout (a hang). Failures are grouped by a signature (kind + first app frames); each group keeps
its first runs as FILE.json (seed, argv, env, tokens) + FILE.log + the generated input files.
Options:
  --runs N        sessions to run (default 200)
  --jobs J        parallel runs (default 6)
  --seed S        first seed (default 1); run i uses seed S+i
  --mode M        gui | tty | files | mcp | mix (default mix: all of them; mcp = JSON-RPC lines
                  on stdin of --mcp instead of UI input)
  --timeout T     seconds per run (default 120)
  --bin PATH      the sanitizer build (default build/monkey/clibretune-asan; --build makes it)
  --build         build the ASan+UBSan app first (like tools/ini_corpus_check.sh)
  --out DIR       results (default build/monkey/out)
  --no-leaks      ASAN_OPTIONS detect_leaks=0
Needs: python3 tools/gen_definitions.py --extract (bundled INIs in build/definitions).
SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FIX = os.path.join(ROOT, "tests", "fixtures")

# ------------------------------------------------------------------ inputs
GUI_SIZES = [(1280, 800), (1920, 1080), (1024, 600), (800, 600), (640, 480), (480, 320),
             (320, 240), (200, 150), (2560, 1440), (1366, 768), (900, 1400)]
TTY_SIZES = [(120, 40), (80, 24), (40, 12), (20, 6), (200, 60), (132, 50), (60, 30), (10, 3)]
SCALES = [None, None, "100", "125", "150", "200", "300", "auto", "1", "2"]
KEYS = ["enter", "esc", "tab", "shift+tab", "up", "down", "left", "right", "home", "end",
        "pgup", "pgdn", "del", "bksp", "ins", "space", "f1", "f2", "f3", "f4", "f5", "f6",
        "f7", "f8", "f9", "f10", "f11", "f12", "ctrl+z", "ctrl+y", "ctrl+c", "ctrl+v",
        "ctrl+x", "ctrl+a", "ctrl+s", "ctrl+o", "ctrl+n", "ctrl+w", "ctrl+l", "ctrl+k",
        "ctrl+f", "ctrl+tab", "ctrl+shift+tab", "ctrl+b", "ctrl+r", "ctrl+e", "ctrl+d",
        "ctrl+g", "ctrl+h", "ctrl+i", "ctrl+m", "ctrl+p", "ctrl+t", "ctrl+u", "ctrl+q",
        "shift+up", "shift+down", "shift+left", "shift+right", "shift+home", "shift+end",
        "ctrl+up", "ctrl+down", "ctrl+left", "ctrl+right", "ctrl+home", "ctrl+end",
        "alt+left", "alt+right", "alt+up", "alt+down", "shift+enter", "ctrl+enter",
        "+", "-", "*", "/", "=", ".", "0", "1", "5", "9", "comma", "<", ">", "[", "]",
        "ctrl+plus", "ctrl+=", "ctrl+-", "ctrl+0", "shift+f10", "ctrl+space"]
ALT = ["alt+" + c for c in "fevtdhcwlsoparmgi"]
TEXTS = ["0", "1", "12", "-1", "-0", "100", "255", "256", "65535", "65536", "4294967295",
         "-2147483649", "99999999999999999999", "1e308", "1e309", "-1e309", "1e-320", "nan",
         "inf", "-inf", "0x7fffffff", "0.5", ".5", "5.", "1,5", "+", "-", "e", "1e", "--1",
         "%s%s%s%n", "%x%x", "%99999d", "../../../../etc/passwd", "..", ".", "/", "\\\\",
         "C:\\\\x", "a" * 90, "9" * 90, "-" + "9" * 60, "\xc3\xa9t\xc3\xa9", "\xe2\x82\xac",
         "\xf0\x9f\x98\x80", "\xff\xfe", "\x7f", "\x01\x02", "\x1b[2J", "x;y=z", "\"'`",
         "<b>", "&amp;", "rpm", "map", "AFR", "VE", "fuel", "ign", "boost", "test", "Tune",
         "new", "*.msq", "?", "[", "{", "}", "$(id)", "a/b", "CON", "nul"]
JUNK_CHARS = [chr(c) for c in range(33, 127) if chr(c) not in ", "] + ["\xc3", "\xa9", "\xff",
                                                                       "\x80", "\x01"]


def rand_text(r):
    if r.random() < 0.75:
        return r.choice(TEXTS)
    return "".join(r.choice(JUNK_CHARS) for _ in range(r.randint(1, 40)))


def gui_tokens(r, w, h, n):
    """pixel-coordinate tokens for LT_SHOT_SCRIPT"""
    out = []

    def pt(region=None):
        if region == "menu":
            return r.randrange(0, max(1, w)), r.randrange(0, max(1, min(h, 40)))
        if region == "bottom":
            return r.randrange(0, max(1, w)), r.randrange(max(0, h - 60), max(1, h))
        if region == "center":
            return (r.randrange(w // 4, max(w // 4 + 1, 3 * w // 4)),
                    r.randrange(h // 4, max(h // 4 + 1, 3 * h // 4)))
        if region == "left":
            return r.randrange(0, max(1, min(w, 260))), r.randrange(0, max(1, h))
        if r.random() < 0.03:   # outside the window
            return r.randint(-50, w + 50), r.randint(-50, h + 50)
        return r.randrange(0, max(1, w)), r.randrange(0, max(1, h))

    while len(out) < n:
        k = r.random()
        if k < 0.38:
            x, y = pt(r.choice([None, None, "menu", "center", "center", "left", "bottom"]))
            out += ["m:%d/%d" % (x, y), "d:%d/%d" % (x, y), "u:%d/%d" % (x, y)]
        elif k < 0.42:
            x, y = pt("center")
            out += ["m:%d/%d" % (x, y), "d:%d/%d" % (x, y), "u:%d/%d" % (x, y),
                    "d:%d/%d" % (x, y), "u:%d/%d" % (x, y)]
        elif k < 0.47:
            x, y = pt()
            x2, y2 = pt()
            out += ["m:%d/%d" % (x, y), "d:%d/%d" % (x, y)]
            for i in range(1, r.randint(2, 5)):
                out.append("m:%d/%d" % (x + (x2 - x) * i // 4, y + (y2 - y) * i // 4))
            out.append("u:%d/%d" % (x2, y2))
        elif k < 0.50:
            x, y = pt()
            out.append("rclick:%d/%d" % (x // 8, y // 16))
        elif k < 0.54:
            x, y = pt()
            out.append("w:%d/%d/%d" % (x, y, r.choice([-1, 1, -3, 3, -10, 10, -120, 120])))
        elif k < 0.57:
            x, y = pt()
            out.append("m:%d/%d" % (x, y))
        elif k < 0.63:
            out.append(r.choice(ALT))
        elif k < 0.71:
            out.append(r.choice(["enter", "esc", "esc", "tab", "enter"]))
        elif k < 0.85:
            out.append(r.choice(KEYS))
        else:
            out.append("text:" + rand_text(r))
    return out[:n]


def tty_tokens(r, cols, rows, n):
    """cell-coordinate tokens for LT_SHOT_KEYS (the parser caps a script at ~248 events)"""
    out = []
    while len(out) < n:
        k = r.random()
        c, rw = r.randrange(0, max(1, cols)), r.randrange(0, max(1, rows))
        if k < 0.30:
            out.append("click:%d/%d" % (c, rw))
        elif k < 0.33:
            out.append("rclick:%d/%d" % (c, rw))
        elif k < 0.36:
            out.append("drag:%d/%d/%d/%d" % (c, rw, r.randrange(0, max(1, cols)),
                                              r.randrange(0, max(1, rows))))
        elif k < 0.40:
            out.append("wheel:%d/%d/%d" % (c, rw, r.choice([-1, 1, -5, 5])))
        elif k < 0.48:
            out.append(r.choice(ALT))
        elif k < 0.58:
            out.append(r.choice(["enter", "esc", "tab", "down", "up"]))
        elif k < 0.90:
            out.append(r.choice(KEYS))
        else:
            out.append("text:" + rand_text(r)[:12])
    return out[:n]


def mcp_lines(r, ini, n):
    """JSON-RPC lines for --mcp (stdin): tool calls with odd arguments, broken JSON, batches"""
    names = []
    try:
        txt = open(ini, encoding="latin-1").read()
        names = re.findall(r"^\s*(\w+)\s*=\s*(?:scalar|array|bits|string)", txt, re.M)
        names += re.findall(r"^\s*table\s*=\s*(\w+)", txt, re.M)
    except OSError:
        pass
    names = names or ["veTable"]
    odd = ["", "../x", "/etc/passwd", "a" * 5000, "\u0000", "%s%n", "log0.csv", "log1.mlg",
           "log0.msl", "..", ".", "log2.csv", "\u00e9"]

    def val():
        return r.choice([r.choice(names), r.choice(odd), 1, -1, 1e308, None, True, [], {},
                         [r.choice(names)] * r.randint(0, 300), {"x": [1, {"y": None}]},
                         [[r.randint(-5, 40), r.randint(-5, 40)] for _ in range(r.randint(0, 50))]])
    tools = ["list_tables", "read_table", "read_constant", "list_features", "tune_health_check",
             "get_realtime_snapshot", "query_datalog", "propose_table_edit",
             "propose_bulk_operation", "propose_constant_change", "nope", ""]
    keys = ["table_name", "name", "log", "channels", "mode", "row", "col", "value", "op", "cells",
            "param", "x", "y"]
    out = ['{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"x"}}']
    while len(out) < n:
        k = r.random()
        if k < 0.70:
            args = {}
            for _ in range(r.randint(0, 4)):
                args[r.choice(keys)] = val()
            if r.random() < 0.3:
                args["mode"] = r.choice(["summary", "tail", "x"])
            req = {"jsonrpc": "2.0", "id": r.choice([1, "a", None, 1e300, -1, [], {}]),
                   "method": "tools/call",
                   "params": {"name": r.choice(tools), "arguments": args
                              if r.random() < 0.9 else r.choice([None, 1, "x", []])}}
            line = json.dumps(req)
        elif k < 0.80:
            line = json.dumps([{"jsonrpc": "2.0", "id": i, "method": r.choice(
                ["ping", "tools/list", "tools/call", "x"])} for i in range(r.randint(0, 40))])
        elif k < 0.90:
            line = mutate(r, json.dumps({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                                         "params": {"name": r.choice(tools),
                                                    "arguments": {"table_name": r.choice(names)}}}
                                        ).encode(), 2).decode("latin-1").replace("\n", " ")
        else:
            line = r.choice(["[" * r.randint(1, 100000), "{" * 5000, '"' + "x" * 100000, "[]",
                             "{}", "null", "1", '{"id":1}', '{"id":1,"method":1}',
                             '{"id":{},"method":"ping"}', "\x00\x01", "[1,2,3]"])
        out.append(line)
    return out


# ------------------------------------------------------------------ hostile files
EXTREME = [b"0", b"-0", b"1e309", b"-1e309", b"nan", b"NaN", b"inf", b"-2147483649",
           b"4294967296", b"99999999999999999999999", b"1e-320", b"0x10", b"-", b"", b"1.5.5",
           b"18446744073709551616", b"-9223372036854775809", b"65536", b"-1"]


def mutate(r, data, rounds=None):
    d = bytearray(data)
    for _ in range(rounds if rounds is not None else r.randint(1, 6)):
        k = r.random()
        if not d:
            d += bytes(r.randrange(256) for _ in range(r.randint(1, 64)))
            continue
        i = r.randrange(len(d))
        if k < 0.15:
            del d[r.randrange(len(d)):]                                 # truncate
        elif k < 0.30:
            for _ in range(r.randint(1, 16)):                           # flip bytes
                d[r.randrange(len(d))] = r.randrange(256)
        elif k < 0.50:                                                  # extreme numbers
            nums = [m for m in re.finditer(rb"-?\d+(\.\d+)?(e-?\d+)?", bytes(d))]
            if nums:
                ms =sorted(r.sample(nums, min(len(nums), r.randint(1, 12))),
                            key=lambda m: -m.start())
                for m in ms:
                    d[m.start():m.end()] = r.choice(EXTREME)
        elif k < 0.60:                                                  # delete lines
            lines = bytes(d).split(b"\n")
            for _ in range(r.randint(1, max(1, len(lines) // 4))):
                if lines:
                    del lines[r.randrange(len(lines))]
            d = bytearray(b"\n".join(lines))
        elif k < 0.68:                                                  # repeat a chunk
            j = min(len(d), i + r.randint(1, 400))
            d[i:i] = bytes(d[i:j]) * r.randint(2, r.choice([10, 100, 3000]))
        elif k < 0.75:                                                  # insert junk
            d[i:i] = bytes(r.randrange(256) for _ in range(r.randint(1, 32)))
        elif k < 0.80:
            d[i:i] = b"\x00" * r.randint(1, 8)
        elif k < 0.86:                                                  # swap lines
            lines = bytes(d).split(b"\n")
            if len(lines) > 2:
                a, b = r.randrange(len(lines)), r.randrange(len(lines))
                lines[a], lines[b] = lines[b], lines[a]
            d = bytearray(b"\n".join(lines))
        elif k < 0.90:                                                  # long token
            d[i:i] = r.choice([b"A", b"9", b"<", b"{", b"[", b"\"", b","]) * r.randint(100, 70000)
        elif k < 0.95:                          # one CSV column constant / huge (ranges, ticks)
            lines = bytes(d).split(b"\n")
            col = r.randrange(6)
            val = r.choice(EXTREME + [b"1e17", b"-1e17", b"1e300", b"5e15"])
            step = r.choice([0, 0, 1, 16, 1e-300])
            for j in range(2, len(lines)):
                cells = lines[j].split(b",")
                if col < len(cells):
                    try:
                        cells[col] = (repr(float(val) + step * j).encode() if step else val)
                    except ValueError:
                        cells[col] = val
                    lines[j] = b",".join(cells)
            d = bytearray(b"\n".join(lines))
        else:                                                           # duplicate a line
            lines = bytes(d).split(b"\n")
            if lines:
                ln = lines[r.randrange(len(lines))]
                lines.insert(r.randrange(len(lines)), ln)
            d = bytearray(b"\n".join(lines))
    return bytes(d)


def rd(p):
    with open(p, "rb") as f:
        return f.read()


def wr(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)


_PROPKEYS = None


def prop_keys():
    global _PROPKEYS
    if _PROPKEYS is None:
        keys = set()
        for d, _, fs in os.walk(os.path.join(ROOT, "src", "app")):
            for f in fs:
                if f.endswith(".c"):
                    for line in open(os.path.join(d, f), encoding="utf-8", errors="replace"):
                        if "props" in line or "get_int" in line:
                            keys.update(re.findall(r'"([a-z][A-Za-z0-9_.]{1,30})"', line))
        for i in range(10):
            for s in ("path", "name", "signature", "opened"):
                keys.add("p%d.%s" % (i, s))
            keys.add("dev%d.key" % i)
            keys.add("dev%d.val" % i)
            keys.add("recent%d" % i)
            keys.add("heat%d" % i)
            keys.add("heat%d_stops" % i)
        keys -= {"mcp", "mcp_port", "base_url", "api_key", "model", "tier"}
        _PROPKEYS = sorted(keys)
    return _PROPKEYS


def hostile_props(r, T):
    vals = [b"", b"0", b"1", b"-1", b"2", b"9999999", b"-2147483648", b"2147483648", b"auto",
            b"nan", b"1e309", b"x" * 600, b"|||||", b"a|b|c|d|e|f", b"a;b;c;;;", b"Dark",
            b"Light", b"nonexistent", b"0,0,0", b"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17",
            b",,,,,", T.encode() + b"/p/none", b"../../..", b"/", b"\xff\xfe", b"\\u0000",
            b"RPM,MAP,CLT,TPS,AFR,IAT,BATT", b"#", b"=", b"x=y=z", b"0x10", b"25.5"]
    lines = []
    keys = prop_keys()
    for _ in range(r.randint(1, 60)):
        lines.append(r.choice(keys).encode() + b"=" + r.choice(vals))
    if r.random() < 0.2:
        lines.append(b"=")
        lines.append(b"\\")
        lines.append(b"key\\")
    return b"\n".join(lines) + (b"\n" if r.random() < 0.8 else b"")


def all_inis():
    out = []
    for base in (os.path.join(ROOT, "build", "definitions"), os.path.join(FIX)):
        for d, _, fs in os.walk(base):
            for f in fs:
                if f.endswith(".ini"):
                    out.append(os.path.join(d, f))
    out.sort()
    return out


_TARGETS = None


def menu_targets():
    """INI -> [menu targets], from tools/ini_check_parse (built by --build)"""
    global _TARGETS
    if _TARGETS is None:
        _TARGETS = {}
        p = os.path.join(ROOT, "build", "monkey", "parse.txt")
        if os.path.exists(p):
            for line in open(p, encoding="utf-8", errors="replace"):
                f = line.rstrip("\n").split("\t")
                if len(f) == 3 and f[0] == "T":
                    _TARGETS.setdefault(os.path.abspath(f[1]), []).append(f[2])
    return _TARGETS


DLG_KINDS = None


def dlg_kinds():
    global DLG_KINDS
    if DLG_KINDS is None:
        ks = set()
        for d, _, fs in os.walk(os.path.join(ROOT, "src", "app")):
            for f in fs:
                if f.endswith(".c"):
                    ks.update(re.findall(r'dk\.kind = "([^"]+)"',
                                         open(os.path.join(d, f), encoding="utf-8",
                                              errors="replace").read()))
        DLG_KINDS = sorted(ks)
    return DLG_KINDS


SEEDS = os.path.join(ROOT, "build", "monkey", "seeds")


def seed_files(kind):
    d = os.path.join(SEEDS, kind)
    fs = sorted(os.path.join(d, f) for f in os.listdir(d)) if os.path.isdir(d) else []
    return fs


PREFIXED = {"msq/mini_defaults.msq", "msq/a.csv"}   # seedgen writes a selector byte first


def seed_bytes(p):
    data = rd(p)
    rel = os.path.relpath(p, SEEDS)
    if rel in PREFIXED or rel.startswith("fw/") or rel.startswith("proto/"):
        data = data[1:]
    return data


def log_seeds():
    fs = seed_files("mlg") + seed_files("msl") + seed_files("csv")
    fs.append(os.path.join(FIX, "datalog", "ts_style.csv"))
    return fs


def msq_seeds():
    fs = [p for p in seed_files("msq") if p.endswith(".msq")]
    fs += [os.path.join(FIX, "tune", "sample.msq"),
           os.path.join(FIX, "project", "ts_proj", "CurrentTune.msq")]
    bt = os.path.join(ROOT, "assets", "basetunes", "speeduino")
    for d, _, f2 in os.walk(bt):
        fs += [os.path.join(d, f) for f in f2 if f.endswith(".msq")][:2]
    return sorted(fs)


def make_project(r, T, name, ini, files):
    """a clibretune project directory with (maybe) hostile files; returns its path"""
    pd = os.path.join(T, "p", name)
    for sub in ("projectCfg", "datalogs", "dashboards"):
        os.makedirs(os.path.join(pd, sub), exist_ok=True)
    ini_data = rd(ini)
    if r.random() < 0.25:
        ini_data = mutate(r, ini_data, r.randint(1, 3))
    wr(os.path.join(pd, "projectCfg", "definition.ini"), ini_data)
    cfg = {"version": "1.0", "name": name, "created": "2026-01-01T00:00:00Z",
           "modified": "2026-01-01T00:00:00Z", "ecu_definition": "projectCfg/definition.ini",
           "signature": "", "connection": {"port": None, "baud_rate": 115200, "timeout_ms": 1000},
           "settings": {"auto_load_tune": True, "auto_save_tune": True, "auto_connect": False,
                        "max_restore_points": 20}, "dashboard": None}
    if r.random() < 0.2:
        cfg["ecu_definition"] = r.choice(["../../../nonexistent.ini", "", "/", "projectCfg",
                                          "projectCfg/definition.ini" * 40])
    if r.random() < 0.15:
        cfg["settings"]["max_restore_points"] = r.choice([0, -1, 1e12, 2 ** 40])
    if r.random() < 0.1:
        cfg["dashboard"] = r.choice(["../x.dash", "", "dashboards/none.dash", "a" * 3000])
    pj = json.dumps(cfg, indent=2).encode()
    if r.random() < 0.3:
        pj = mutate(r, pj, r.randint(1, 3))
    wr(os.path.join(pd, "project.json"), pj)
    if r.random() < 0.7:
        wr(os.path.join(pd, "CurrentTune.msq"), mutate(r, rd(r.choice(msq_seeds())))
           if r.random() < 0.7 else rd(r.choice(msq_seeds())))
    if r.random() < 0.5:
        wr(os.path.join(pd, "projectCfg", "pcVariableValues.msq"),
           mutate(r, rd(r.choice(msq_seeds()))))
    for i in range(r.randint(0, 4)):
        src = r.choice(log_seeds())
        ext = os.path.splitext(src)[1]
        wr(os.path.join(pd, "datalogs", "log%d%s" % (i, ext)), mutate(r, seed_bytes(src)))
    for i in range(r.randint(0, 2)):
        src = r.choice(seed_files("dash") or [os.path.join(FIX, "dash", "basic.dash")])
        wr(os.path.join(pd, "dashboards", "d%d%s" % (i, os.path.splitext(src)[1])),
           mutate(r, seed_bytes(src)))
    if r.random() < 0.3:
        os.makedirs(os.path.join(pd, "restorePoints"), exist_ok=True)
        for i in range(r.randint(1, 3)):
            wr(os.path.join(pd, "restorePoints", "rp_2026-01-0%d_00.00.0%d.msq" % (i + 1, i)),
               mutate(r, rd(r.choice(msq_seeds()))))
    for fn in ("triggers.properties", "math_channels.json", "graph_setup.json", "setup.conf",
               "prefs.conf", "vehicle_specs.properties", "autotune_proposal.json"):
        if r.random() < 0.12:
            if fn.endswith(".json"):
                data = mutate(r, b'{"channels":[{"name":"x","expr":"rpm*2","units":"u"}],'
                                 b'"a":[1,2,3],"b":{"c":null}}')
            else:
                data = hostile_props(r, T)
            wr(os.path.join(pd, "projectCfg", fn), data)
    return pd


def gen_files(r, T):
    """loose hostile files for the file dialogs (cwd/PWD) and the env hooks"""
    fd = os.path.join(T, "files")
    out = {}
    os.makedirs(fd, exist_ok=True)
    for i, src in enumerate(r.sample(log_seeds(), min(3, len(log_seeds())))):
        p = os.path.join(fd, "log%d%s" % (i, os.path.splitext(src)[1]))
        wr(p, mutate(r, seed_bytes(src)))
        out.setdefault("log", []).append(p)
    for i, src in enumerate(r.sample(msq_seeds(), 2)):
        p = os.path.join(fd, "tune%d.msq" % i)
        wr(p, mutate(r, rd(src)))
        out.setdefault("msq", []).append(p)
    ds = seed_files("dash")
    if ds:
        src = r.choice(ds)
        p = os.path.join(fd, "dash" + os.path.splitext(src)[1])
        wr(p, mutate(r, seed_bytes(src)))
        out["dash"] = [p]
    mp = seed_files("msqpart")
    if mp:
        p = os.path.join(fd, "part.msqpart")
        wr(p, mutate(r, seed_bytes(r.choice(mp))))
        out["msqpart"] = [p]
    p = os.path.join(fd, "trig.properties")
    wr(p, hostile_props(r, T))
    out["trig"] = [p]
    for i, src in enumerate([os.path.join(FIX, "dyno", "run_before.csv"),
                             os.path.join(FIX, "dyno", "run_after.csv")]):
        p = os.path.join(fd, "dyno%d.csv" % i)
        wr(p, mutate(r, rd(src)) if r.random() < 0.7 else rd(src))
        out.setdefault("dyno", []).append(p)
    return out


# ------------------------------------------------------------------ scenario
def scenario(seed, mode):
    r = random.Random(seed)
    if mode == "mix":
        mode = r.choices(["gui", "tty", "files", "mcp"], [6, 2, 3, 1])[0]
    sc = {"seed": seed, "mode": mode, "args": [], "env": {}, "prep": {}}
    inis = all_inis()
    start = r.choices(["start", "demo", "demo_ini", "ini", "project"],
                      [2, 3, 5, 1, 3 if mode == "files" else 1])[0]
    ini = r.choice(inis)
    if r.random() < 0.15:
        ini = r.choice([p for p in inis if "/fixtures/" in p])
    sc["start"] = start
    if start == "demo":
        sc["args"] = ["--demo"]
    elif start == "demo_ini":
        sc["args"] = ["--demo", "--ini", ini]
    elif start == "ini":
        sc["args"] = ["--ini", ini]
    elif start == "project":
        sc["args"] = ["--project", "@PROJECT@"]
        sc["prep"]["project_ini"] = ini
    if mode == "mcp":
        start = r.choice(["demo_ini", "project", "project", "ini"])
        sc["prep"] = {}
        sc["start"] = start
        sc["args"] = {"demo_ini": ["--mcp", "--demo", "--ini", ini], "ini": ["--mcp", "--ini", ini],
                      "project": ["--mcp", "--project", "@PROJECT@"]}[start]
        if start == "project":
            sc["prep"]["project_ini"] = ini
        sc["tokens"] = mcp_lines(r, ini, r.randint(5, 60))
        sc["tokvar"] = None
        return sc
    if mode != "files" and r.random() < 0.10 and start in ("demo_ini", "ini"):
        sc["prep"]["mutate_ini"] = True
    if r.random() < 0.35 and start != "start":
        tg = menu_targets().get(os.path.abspath(ini), [])
        if start != "demo" and tg and r.random() < 0.6:
            sc["env"]["LT_SHOT_OPEN"] = r.choice(tg)
        else:
            sc["env"]["LT_SHOT_OPEN"] = "dlg:" + r.choice(
            [k for k in dlg_kinds() if start == "project" or not k.startswith("history")])
    elif r.random() < 0.08:
        # only dialogs the app itself can open here: calibration dialogs come from INI menus and
        # assume a definition, the history dialogs a project (dlg:<kind> otherwise is harness-only)
        kinds = [k for k in dlg_kinds() if not k.startswith("history") and
                 (start != "start" or not k.startswith("calib"))]
        sc["env"]["LT_SHOT_OPEN"] = "dlg:" + r.choice(kinds)
    if mode == "tty":
        c, rw = r.choice(TTY_SIZES)
        sc["env"]["LT_SHOT_TEXT"] = "@T@/out.txt"
        sc["env"]["LT_SHOT_SIZE"] = "%dx%d" % (c, rw)
        sc["env"]["LT_SHOT_FRAMES"] = str(r.choice([1, 2, 4]))
        sc["tokens"] = tty_tokens(r, c, rw, r.randint(10, 90))
        sc["tokvar"] = "LT_SHOT_KEYS"
    else:
        w, h = r.choice(GUI_SIZES)
        sc["env"]["LT_SCREENSHOT"] = "@T@/out.ppm"
        sc["env"]["LT_SHOT_SIZE"] = "%dx%d" % (w, h)
        sc["env"]["LT_SHOT_FRAMES"] = str(r.choice([1, 2, 3]))
        sc["tokens"] = gui_tokens(r, w, h, r.randint(20, 220))
        sc["tokvar"] = "LT_SHOT_SCRIPT"
    s = r.choice(SCALES)
    if s is not None:
        sc["env"]["LT_SCALE"] = s
    sc["env"]["LT_SHOT_SLEEP"] = str(r.choice([0, 16, 60, 60, 300, 2500]))
    if mode == "files" or r.random() < 0.15:
        sc["prep"]["hostile_config"] = [f for f in ("settings.conf", "recent.conf",
                                                    "devices.conf", "prefs.conf",
                                                    "logging.conf", "dashboard.conf")
                                        if r.random() < 0.4]
    if mode == "files":
        sc["prep"]["files"] = True
        hooks = r.sample(["LT_SHOT_LOG", "LT_TUNEDIFF_B", "LT_MSQPART_FILE", "LT_DASH_IMPORT",
                          "LT_TRIGGERS_FILE", "LT_DYNO_RUNS", "LT_AT_REPLAY"], r.randint(0, 3))
        sc["prep"]["hooks"] = hooks
        if "LT_TUNEDIFF_B" in hooks and r.random() < 0.7:
            sc["env"]["LT_SHOT_OPEN"] = "dlg:tunediff"
        if "LT_MSQPART_FILE" in hooks and r.random() < 0.7:
            sc["env"]["LT_SHOT_OPEN"] = "dlg:msqpart_load"
        if "LT_SHOT_LOG" in hooks and start != "start" and r.random() < 0.7:
            sc["env"]["LT_SHOT_OPEN"] = "datalog-viewer"
            sc["env"]["LT_SHOT_LOGVIEW"] = r.choice(["graph", "scatter", "hist"])
        if "LT_DYNO_RUNS" in hooks and r.random() < 0.7:
            sc["env"]["LT_SHOT_OPEN"] = "dlg:dyno"
    return sc


def materialise(sc, T):
    """create the run directory and the inputs; returns (argv, env)"""
    r = random.Random(sc["seed"] * 7919 + 17)
    for d in ("c", "h", "p", "defs", "dl", "bin", "files"):
        os.makedirs(os.path.join(T, d), exist_ok=True)
    xo = os.path.join(T, "bin", "xdg-open")
    with open(xo, "w") as f:
        f.write('#!/bin/sh\necho "$@" >> "%s/urls.txt"\n' % T)
    os.chmod(xo, 0o755)
    prep = sc["prep"]
    args = list(sc["args"])
    if prep.get("mutate_ini"):
        i = args.index("--ini") + 1
        p = os.path.join(T, "files", "mut.ini")
        wr(p, mutate(r, rd(args[i]), r.randint(1, 4)))
        args[i] = p
    for fn in prep.get("hostile_config", []):
        wr(os.path.join(T, "c", fn), hostile_props(r, T))
    if "project_ini" in prep:
        pd = make_project(r, T, "Proj", prep["project_ini"], None)
        args[args.index("@PROJECT@")] = pd
        if r.random() < 0.5:   # a second one for the start screen / recent list
            make_project(r, T, "Other", r.choice(all_inis()), None)
    elif sc["start"] == "start" and r.random() < 0.5:
        make_project(r, T, "Proj", r.choice(all_inis()), None)
    env = {k: v.replace("@T@", T) for k, v in sc["env"].items()}
    if prep.get("files"):
        fs = gen_files(r, T)
        for h in prep.get("hooks", []):
            if h in ("LT_SHOT_LOG", "LT_AT_REPLAY"):
                env[h] = r.choice(fs["log"])
            elif h == "LT_TUNEDIFF_B":
                env[h] = r.choice(fs["msq"])
            elif h == "LT_MSQPART_FILE":
                env[h] = fs.get("msqpart", fs["msq"])[0]
            elif h == "LT_DASH_IMPORT":
                env[h] = fs.get("dash", fs["msq"])[0]
            elif h == "LT_TRIGGERS_FILE":
                env[h] = fs["trig"][0]
            elif h == "LT_DYNO_RUNS":
                env[h] = "|".join(fs["dyno"])
    if sc["tokvar"] is not None:
        env[sc["tokvar"]] = " ".join(sc["tokens"])
    base = {k: v for k, v in os.environ.items()
            if k in ("PATH", "LANG", "LC_ALL", "TERM", "USER", "LOGNAME", "TMPDIR")}
    base.update({
        "HOME": os.path.join(T, "h"), "CLIBRETUNE_CONFIG_DIR": os.path.join(T, "c"),
        "XDG_CONFIG_HOME": os.path.join(T, "h", ".config"),
        "XDG_DATA_HOME": os.path.join(T, "h", ".local", "share"),
        "CLIBRETUNE_PROJECTS": os.path.join(T, "p"),
        "CLIBRETUNE_DEFINITIONS": os.path.join(T, "defs"),
        "CLIBRETUNE_VOLUMES": os.path.join(T, "files"),
        "LT_DOWNLOADS_DIR": os.path.join(T, "dl"),
        "LT_DEMO_INI": os.path.join(FIX, "ini", "demo.ini"),
        "PWD": os.path.join(T, "files"),
        "PATH": os.path.join(T, "bin") + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
        "LT_NO_BROWSER": "1",
        "LT_USB_SYSFS_ROOT": os.path.join(T, "files", "nosys"),
        "ASAN_OPTIONS": "detect_leaks=%d:abort_on_error=0:allocator_may_return_null=1"
                        ":detect_stack_use_after_return=1:handle_abort=1" % (0 if NO_LEAKS else 1),
        "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1",
    })
    base.update(env)
    return args, base


NO_LEAKS = False


# ------------------------------------------------------------------ run + classify
FRAME_RE = re.compile(r"^\s*#(\d+) 0x[0-9a-f]+ in (\S+) (\S+)")


def is_app_file(path):
    return path.startswith("src/") or path.startswith(os.path.join(ROOT, "src") + os.sep)


def signature(log, rc, timed_out):
    if timed_out:
        frames = []
        for line in log.splitlines():
            fm = FRAME_RE.match(line)
            if fm and is_app_file(fm.group(3)):
                frames.append("%s@%s" % (fm.group(2), os.path.basename(fm.group(3).split(":")[0])))
                if len(frames) >= 2:
                    break
        return "HANG", " ".join(frames) or "timeout"
    kind = None
    m = re.search(r"ERROR: AddressSanitizer: ([\w-]+)", log)
    if m:
        kind = "ASAN-" + m.group(1)
    elif "runtime error:" in log:
        m = re.search(r"(\S+?):(\d+):\d+: runtime error: ([^\n]{0,60})", log)
        if m:
            return "UBSAN", "%s:%s %s" % (os.path.basename(m.group(1)), m.group(2),
                                          re.sub(r"\d+(\.\d+)?", "N", m.group(3))[:40])
        kind = "UBSAN"
    elif "LeakSanitizer" in log:
        kind = "LEAK"
    elif "AddressSanitizer" in log:
        kind = "ASAN"
    elif rc < 0:
        kind = "SIG%d" % -rc
    elif rc != 0:
        kind = "EXIT%d" % rc
    else:
        return None, None
    frames = []
    for line in log.splitlines():
        fm = FRAME_RE.match(line)
        if fm and is_app_file(fm.group(3)) and not fm.group(2).startswith("lt_malloc") and \
                fm.group(2) not in ("lt_calloc", "lt_realloc", "lt_strdup", "lt_strndup"):
            frames.append("%s@%s" % (fm.group(2), os.path.basename(fm.group(3).split(":")[0])))
            if len(frames) >= 3:
                break
        if kind == "LEAK" and frames and line.strip() == "":
            break
    return kind, " ".join(frames) or "noframes"


def run_one(sc, outdir, binpath, timeout, keep=False, T=None):
    T = T or os.path.join(outdir, "run", "s%d" % sc["seed"])
    shutil.rmtree(T, ignore_errors=True)
    os.makedirs(T)
    args, env = materialise(sc, T)
    t0 = time.time()
    timed_out = False
    stdin = None
    if sc["tokvar"] is None:
        stdin = ("\n".join(sc["tokens"]) + "\n").encode("utf-8", "surrogateescape")
    p = subprocess.Popen([binpath] + args, env=env, cwd=os.path.join(T, "files"),
                         stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True)
    try:
        log, _ = p.communicate(input=stdin, timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:                         # handle_abort=1: ASan prints where it was stuck
            os.killpg(p.pid, signal.SIGABRT)
            log, _ = p.communicate(timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            log, _ = p.communicate()
    dt = time.time() - t0
    log = log.decode("utf-8", "replace")
    kind, sig = signature(log, p.returncode, timed_out)
    res = {"seed": sc["seed"], "kind": kind, "sig": sig, "dt": round(dt, 2), "rc": p.returncode}
    if kind is not None:
        res["log"] = log
        res["T"] = T
    elif not keep:
        shutil.rmtree(T, ignore_errors=True)
    return res


def save_failure(outdir, sc, res):
    h = hashlib.sha1(("%s|%s" % (res["kind"], res["sig"])).encode()).hexdigest()[:10]
    d = os.path.join(outdir, "fail", "%s-%s" % (res["kind"], h))
    os.makedirs(d, exist_ok=True)
    n = len([f for f in os.listdir(d) if f.endswith(".json")])
    if n < 5:
        rec = dict(sc)
        rec.update({"kind": res["kind"], "sig": res["sig"], "dt": res["dt"], "rc": res["rc"]})
        with open(os.path.join(d, "s%d.json" % sc["seed"]), "w") as f:
            json.dump(rec, f, indent=1)
        with open(os.path.join(d, "s%d.log" % sc["seed"]), "w") as f:
            f.write(res["log"])
        inp = os.path.join(d, "s%d.inputs" % sc["seed"])
        shutil.rmtree(inp, ignore_errors=True)
        try:
            shutil.copytree(res["T"], inp, ignore=shutil.ignore_patterns("*.ppm", "bin"))
        except (OSError, shutil.Error):
            pass
    shutil.rmtree(res["T"], ignore_errors=True)
    return d


def cmd_runs(o):
    outdir = o["out"]
    os.makedirs(outdir, exist_ok=True)
    seeds = list(range(o["seed"], o["seed"] + o["runs"]))
    groups = {}
    n_done = 0
    t0 = time.time()
    stats = open(os.path.join(outdir, "runs.tsv"), "a")

    def work(s):
        sc = scenario(s, o["mode"])
        return sc, run_one(sc, outdir, o["bin"], o["timeout"])

    with ThreadPoolExecutor(max_workers=o["jobs"]) as ex:
        for sc, res in ex.map(work, seeds):
            n_done += 1
            stats.write("%d\t%s\t%s\t%s\t%.2f\t%d\t%s\n" % (
                sc["seed"], sc["mode"], sc["start"], res["kind"] or "OK", res["dt"],
                len(sc["tokens"]), sc["env"].get("LT_SHOT_OPEN", "-")))
            stats.flush()
            if res["kind"] is not None:
                d = save_failure(outdir, sc, res)
                key = (res["kind"], res["sig"])
                if key not in groups:
                    print("NEW %s %s  (seed %d) -> %s" % (res["kind"], res["sig"], sc["seed"], d),
                          flush=True)
                groups.setdefault(key, []).append(sc["seed"])
            if n_done % 100 == 0:
                print("... %d/%d runs, %d failure groups, %.0f s" % (
                    n_done, len(seeds), len(groups), time.time() - t0), flush=True)
    print("monkey: %d runs in %.0f s, %d failure groups" % (n_done, time.time() - t0, len(groups)))
    for (k, s), v in sorted(groups.items()):
        print("  %-22s %4d  %s  seeds %s" % (k, len(v), s, " ".join(map(str, v[:6]))))
    return 1 if groups else 0


def load_rec(path):
    with open(path) as f:
        return json.load(f)


def cmd_repro(o, path):
    sc = load_rec(path)
    T = os.path.join(o["out"], "repro")
    res = run_one(sc, o["out"], o["bin"], o["timeout"], keep=True, T=T)
    args, env = materialise(sc, os.path.join(o["out"], "repro-show"))
    shown = {k: v for k, v in env.items() if k.startswith(("LT_", "CLIBRETUNE", "LIBRETUNE"))}
    print("argv: %s" % " ".join(args))
    for k in sorted(shown):
        print("%s=%s" % (k, shown[k]))
    print("result: %s %s (%.1f s, rc %s); run dir %s" % (res["kind"] or "OK", res["sig"] or "",
                                                         res["dt"], res["rc"], T))
    if res.get("log"):
        print(res["log"][-6000:])
    return 0 if res["kind"] is None else 1


def cmd_minimize(o, path):
    sc = load_rec(path)
    T = os.path.join(o["out"], "min")
    cache = {}
    first = run_one(sc, o["out"], o["bin"], o["timeout"], T=T)   # signature of this build
    shutil.rmtree(T, ignore_errors=True)
    want = (first["kind"], first["sig"])
    if first["kind"] is None or first["kind"] != sc["kind"]:
        print("does not reproduce: %s %s (now %s)" % (sc["kind"], sc["sig"], first["kind"]))
        return 1

    def fails(toks):
        key = "\x1f".join(toks)
        if key in cache:
            return cache[key]
        t = dict(sc)
        t["tokens"] = toks
        res = run_one(t, o["out"], o["bin"], o["timeout"], T=T)
        ok = (res["kind"], res["sig"]) == want
        shutil.rmtree(T, ignore_errors=True)
        cache[key] = ok
        return ok

    toks = list(sc["tokens"])
    if not fails(toks):
        print("does not reproduce: %s %s" % want)
        return 1
    n = 2
    while len(toks) >= 2:
        chunk = max(1, len(toks) // n)
        reduced = False
        for i in range(0, len(toks), chunk):
            cand = toks[:i] + toks[i + chunk:]
            if cand and fails(cand):
                toks = cand
                n = max(n - 1, 2)
                reduced = True
                print("  %d tokens" % len(toks), flush=True)
                break
        if not reduced:
            if chunk == 1:
                break
            n = min(len(toks), n * 2)
    if fails([]):
        toks = []
    sc["tokens"] = toks
    outp = path.replace(".json", ".min.json")
    with open(outp, "w") as f:
        json.dump(sc, f, indent=1)
    print("minimal (%d tokens): %s" % (len(toks), " ".join(toks)))
    print("written %s" % outp)
    return 0


def build():
    sanf = "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
    d = os.path.join(ROOT, "build", "monkey")
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, CFLAGS=sanf, LDFLAGS=sanf)
    with open(os.path.join(d, "build.log"), "w") as lg:
        subprocess.check_call(["./build.sh", "app"], cwd=ROOT, env=env, stdout=lg, stderr=lg)
        shutil.copy(os.path.join(ROOT, "build", "clibretune"), os.path.join(d, "clibretune-asan"))
        subprocess.check_call(["./build.sh", "app"], cwd=ROOT, stdout=lg, stderr=lg)
    sh = ("set -e; . tools/target.sh; srcs=''; for s in $(lt_sources); do srcs=\"$srcs $s\"; done;"
          "${CC:-cc} -O2 $PLAT_CPPFLAGS -Isrc tools/ini_check_parse.c $srcs -o build/monkey/"
          "ini_check_parse $TARGET_LIBS;"
          "${CC:-cc} -O1 $PLAT_CPPFLAGS -Isrc tools/fuzz/seedgen.c $srcs -o build/monkey/seedgen"
          " $TARGET_LIBS; rm -rf build/monkey/seeds; mkdir -p build/monkey/seeds;"
          "build/monkey/seedgen build/monkey/seeds; rm -f build/monkey/seeds/seed.*")
    subprocess.check_call(["sh", "-c", sh], cwd=ROOT)
    with open(os.path.join(d, "parse.txt"), "w") as f:
        for ini in all_inis():
            subprocess.call([os.path.join(d, "ini_check_parse"), ini], stdout=f,
                            stderr=subprocess.DEVNULL, cwd=ROOT)


def main(argv):
    global NO_LEAKS
    o = {"runs": 200, "jobs": 6, "seed": 1, "mode": "mix", "timeout": 120,
         "bin": os.path.join(ROOT, "build", "monkey", "clibretune-asan"),
         "out": os.path.join(ROOT, "build", "monkey", "out")}
    repro = minimize = None
    do_build = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--runs", "--jobs", "--seed", "--timeout"):
            i += 1
            o[a[2:]] = int(argv[i])
        elif a in ("--mode", "--bin", "--out"):
            i += 1
            o[a[2:]] = os.path.abspath(argv[i]) if a != "--mode" else argv[i]
        elif a == "--repro":
            i += 1
            repro = argv[i]
        elif a == "--minimize":
            i += 1
            minimize = argv[i]
        elif a == "--build":
            do_build = True
        elif a == "--no-leaks":
            NO_LEAKS = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            print("unknown option %s" % a, file=sys.stderr)
            return 2
        i += 1
    if do_build:
        build()
    if not os.path.isdir(os.path.join(ROOT, "build", "definitions")):
        print("run python3 tools/gen_definitions.py --extract first", file=sys.stderr)
        return 2
    if repro:
        return cmd_repro(o, repro)
    if minimize:
        return cmd_minimize(o, minimize)
    return cmd_runs(o)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
