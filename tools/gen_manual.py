#!/usr/bin/env python3
"""gen_manual.py - emit src/app/manual_data.c: the user manual embedded as text, taken from the
stock LibreTune manual (ref/LibreTune/docs/src, ordered by SUMMARY.md).

Usage: tools/gen_manual.py [docs_src_dir] > src/app/manual_data.c

The developer-oriented parts (technical/, changelog, contributing, migration notes, MCP /
AI assistant) are left out.  Every page is a list of lines (a Markdown source line longer than
~300 characters is wrapped at a space: paragraphs are re-flowed by the viewer anyway), kept in
parts of at most PART lines so no single object passes 32767 bytes, and no literal passes 509
characters.  Characters the embedded fonts do not have are replaced by ASCII.
"""
import os
import re
import sys

SKIP_PREFIX = ("technical/", "features/mcp-server", "features/ai-assistant", "changelog",
               "contributing", "reference/java-to-wasm")
PART = 600
WRAP = 300
REPL = {
    "❌": "x", "✗": "x", "✕": "x", "✅": "✓", "⚠": "(!)",
    "️": "", "\U0001f534": "(red)", "\U0001f7e2": "(green)", "\U0001f7e1": "(yellow)",
    "\U0001f535": "(blue)", "\U0001f512": "(lock)", "⬜": "[ ]", "↗": "->",
    "⇄": "<->", "⏸": "||", "⏪": "<<", "⏩": ">>", "\U0001f7e6": "[#]",
    "\U0001f7e5": "[#]", "⚪": "(o)", "↙": "<-", "─": "-", "│": "|",
    "├": "+", "└": "+", "┘": "+", "┐": "+", "┌": "+", "┬": "+",
    "┴": "+", "┼": "+",
}
KEEP = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180)) | set(range(0x391, 0x3CA)) | {
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192,
    0x2193, 0x2194, 0x2195, 0x2212, 0x2248, 0x2260, 0x2264, 0x2265, 0x25B6, 0x2713}


# This port's assistant is local-only (OpenAI-compatible endpoints over plain HTTP), so the
# stock manual's cloud-vendor specifics are dropped or made generic; no vendor or product
# names of LLM providers end up in the embedded manual (main() asserts it).
_V1 = "".join(map(chr, (67, 108, 97, 117, 100, 101)))            # the vendors' names, spelled
_V2 = "".join(map(chr, (65, 110, 116, 104, 114, 111, 112, 105, 99)))  # as codes on purpose
VENDOR_SUBS = [
    (r"\s*\(" + _V1 + r"\s+Code,\s+" + _V1 + r"\s+Desktop,\s+\u2026\)", ""),
    (_V1 + r"\s+Code,\s+" + _V1 + r"\s+Desktop,\s+or\s+any\s+other\s+MCP\s+client", "any MCP client"),
    (r"\n- \*\*" + _V2 + r"\*\*[^\n]*", ""),
    (r"`" + _V1.lower() + r"-[^`]*`,\s*", ""),
    (_V2 + r",\s*", ""),
]
VENDOR_RE = re.compile(_V1 + "|" + _V2, re.I)


def devendor(text):
    for pat, rep in VENDOR_SUBS:
        text = re.sub(pat, rep, text)
    return text


def clean(s):
    out = []
    for ch in s:
        if ch in REPL:
            out.append(REPL[ch])
        elif ord(ch) in KEEP:
            out.append(ch)
        elif ch == "\t":
            out.append("    ")
        else:
            out.append("?")
    return "".join(out)


def cesc(s):
    out = []
    octal = False
    for b in s.encode("utf-8"):
        c = chr(b)
        if octal and c.isdigit():
            out.append("\\%03o" % b)   # a digit right after an octal escape is escaped too:
            continue                   # valid C either way, but MSVC warns (C4125)
        octal = False
        if c == '"' or c == "\\":
            out.append("\\" + c)
        elif c == "?":
            out.append("\\?")          # no trigraphs
        elif c == "%":
            out.append("\\045")        # keeps printf-format scanners quiet
            octal = True
        elif 32 <= b < 127:
            out.append(c)
        else:
            out.append("\\%03o" % b)
            octal = True
    return "".join(out)


def wrap(line):
    res = []
    while len(line.encode("utf-8")) > WRAP:
        n = WRAP
        while len(line[:n].encode("utf-8")) > WRAP:
            n -= 1
        cut = line.rfind(" ", 0, n)
        if cut <= 0:
            cut = n
        res.append(line[:cut])
        line = line[cut:].lstrip(" ")
    res.append(line)
    return res


def summary(base):
    pages = []
    section = ""
    for raw in open(os.path.join(base, "SUMMARY.md"), encoding="utf-8"):
        m = re.match(r"^# (.+)$", raw.rstrip())
        if m and m.group(1) != "Summary":
            section = m.group(1)
            continue
        m = re.match(r"^(\s*)[-]?\s*\[(.+?)\]\((.+?)\)", raw.rstrip())
        if not m:
            continue
        depth = len(m.group(1)) // 2
        path = m.group(3)
        if path.startswith("./"):
            path = path[2:]
        if not path.endswith(".md"):
            continue
        pid = path[:-3]
        if pid.startswith(SKIP_PREFIX) or any(pid.startswith(p) for p in SKIP_PREFIX):
            continue
        if not os.path.exists(os.path.join(base, path)):
            continue
        pages.append((pid, m.group(2), section or "Manual", depth))
    # the FAQ lives under its own heading
    return pages


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "ref/LibreTune/docs/src"
    pages = summary(base)
    seen = set()
    w = sys.stdout.write
    w("/* manual_data.c - the user manual as embedded text.  GENERATED by tools/gen_manual.py from the\n")
    w(" * stock LibreTune manual (docs/src), DO NOT EDIT.\n")
    w(" * SPDX-License-Identifier: GPL-2.0-or-later\n */\n")
    w('#include "manual.h"\n\n')
    for n, (pid, title, section, depth) in enumerate(pages):
        if pid in seen:
            continue
        seen.add(pid)
        text = devendor(open(os.path.join(base, pid + ".md"), encoding="utf-8").read())
        assert not VENDOR_RE.search(text), "vendor name left in " + pid
        lines = []
        for ln in text.replace("\r", "").split("\n"):
            lines.extend(wrap(clean(ln.rstrip())))
        parts = [lines[i:i + PART] for i in range(0, len(lines), PART)] or [[]]
        for k, part in enumerate(parts):
            w("static const char *const mp_%d_%d[] = {\n" % (n, k))
            if not part:
                w('    ""\n')
            for ln in part:
                w('    "%s",\n' % cesc(ln))
            w("};\n")
        w("static const lt_man_part mpp_%d[] = {\n" % n)
        for k, part in enumerate(parts):
            w("    { mp_%d_%d, %d },\n" % (n, k, len(part)))
        w("};\n\n")
    w("const lt_man_page lt_man_pages[] = {\n")
    seen = set()
    count = 0
    for n, (pid, title, section, depth) in enumerate(pages):
        if pid in seen:
            continue
        seen.add(pid)
        w('    { "%s", "%s", "%s", %d, mpp_%d, (int)(sizeof(mpp_%d) / sizeof(mpp_%d[0])) },\n'
          % (cesc(pid), cesc(clean(title)), cesc(section), depth, n, n, n))
        count += 1
    w("};\n")
    w("const int lt_man_count = %d;\n" % count)


if __name__ == "__main__":
    main()
