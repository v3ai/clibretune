#!/usr/bin/env python3
"""gen_manual.py - emit src/app/manual_data.c: the user manual embedded as text, taken from the
stock LibreTune manual (ref/LibreTune/docs/src, ordered by SUMMARY.md).

Usage: tools/gen_manual.py [docs_src_dir] > src/app/manual_data.c

The developer-oriented parts (technical/, changelog, contributing, migration notes, MCP /
AI assistant) are left out, and so is what this port does not have: pop-out dashboard windows
and the self-updater (REWRITES below replace those passages with what clibretune here does).  Every page is a list of lines (a Markdown source line longer than
~300 characters is wrapped at a space: paragraphs are re-flowed by the viewer anyway), kept in
parts of at most PART lines so no single object passes 32767 bytes, and no literal passes 509
characters.  Characters the embedded fonts do not have are replaced by ASCII.
"""
import os
import re
import sys

SKIP_PREFIX = ("technical/", "features/mcp-server", "features/ai-assistant", "changelog",
               "contributing", "reference/java-to-wasm", "features/popout-windows")
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


# Passages of the stock manual describing features this port does not have, per page:
# (page id, regex, replacement).  Every pattern must match (a changed upstream page fails the
# generator instead of silently keeping the old promise); FORBIDDEN_RE is checked on the result.
RELEASES = "https://github.com/v3ai/clibretune/releases"
REWRITES = [
    ("getting-started/installation", r"## Updating\n.*?(?=## Building from Source)",
     "## Updating\n\n"
     "clibretune does not download or install updates by itself. **Help \u2192 Check for Updates**"
     " shows the version and build you are running and opens the releases page (" + RELEASES +
     ") in your web browser. Download a newer build from there and install it the same way as"
     " the first one.\n\n"),
    ("getting-started/settings", r"\*\*Check for Updates:\*\*\n(- [^\n]*\n)+\n",
     "**Updates:** there is no automatic update check; **Help \u2192 Check for Updates** shows"
     " your version and opens the releases page.\n\n"),
    ("features/dashboards/using", r"## Multi-Monitor\n.*?(?=## Performance)",
     "## Multi-Monitor\n\n"
     "Dashboards are tabs of the main window; there are no separate pop-out windows. For a"
     " second monitor, move the clibretune window there and use **Full screen (F11)** on the"
     " dashboard.\n\n"),
    ("introduction", r",\s*keyboard navigation,\s*and multi-monitor support\.",
     " and keyboard navigation."),
    ("features/autotune", r"3\. Or click \*\*Apply & Save\*\* to update the tune file\n"
     r"4\. \*\*Burn\*\* to make changes permanent in ECU flash\n",
     "3. **Undo** (Ctrl+Z) writes the table back as it was before the last Send to ECU\n"
     "4. **Burn** to make changes permanent in ECU flash; File > Save Tune keeps them in the"
     " project\n"),
    ("features/table-editing/shortcuts", r"\| Escape \| Clear selection \|\n",
     "| Escape | Cancel the value being typed; otherwise keep only the cursor cell selected |\n"),
    ("features/table-editing/shortcuts", r"\| Enter \| Edit selected cell \|\n.*?(?=\n## Clipboard)",
     "| 0-9 or . | Start typing a value for the selected cells |\n"
     "| Enter or F2 | Edit the cell, starting from its value |\n"
     "| Delete or Backspace | Clear the selected cells |\n"
     "| + or > | Increase value by step |\n"
     "| - or < | Decrease value by step |\n"
     "| q / w | Increase / decrease by a small step |\n"
     "| Ctrl + (+/-) | Increase/decrease by the step times the multiplier |\n"
     "| = | Set equal (average) |\n"
     "| * | Scale by factor |\n"
     "| / | Interpolate |\n"
     "| S | Smooth selection |\n\n"
     "While a value is being typed, `-` is part of the number. Without an edit open, `-`"
     " steps the selection down (like TunerStudio): to type a negative value press Enter or F2"
     " first, then type it (for example `-5`).\n"),
    ("features/table-editing/2d-tables", r"### Single Cell Edit\n1\. Select a cell\n2\. Type a number\n"
     r"3\. Press Enter\n",
     "### Single Cell Edit\n1. Select a cell\n2. Type a number\n3. Press Enter (or click another"
     " cell)\n\nEsc while typing cancels that edit and the cell keeps its value. Esc otherwise"
     " only reduces the selection to the cursor cell: it never changes values. To go back to the"
     " values the table had when it was opened use **Revert** in the toolbar (whole table) or"
     " **Reset to original** in the right-click menu (selected cells).\n\n"
     "A typed, pasted or calculated value outside the limits of the ECU definition is set to"
     " the nearest allowed value and a message says so (\"Limited to 255 (allowed 0 - 255)\","
     " or how many values were limited). An operation that changes nothing (for example"
     " pasting the values a cell already has) adds no undo step and leaves the tune"
     " unmodified.\n"),
    ("features/table-editing/2d-tables", r"\| `S` \| Smooth \| Gaussian blur \|\n",
     "| `S` | Smooth | Gaussian blur |\n\n"
     "Every operation is also a toolbar button. When the window is too narrow for the whole"
     " toolbar, the buttons that do not fit move into the **\u00bb** menu at its right end;"
     " **Burn** and **3D** always stay on the bar.\n\n"
     "A Target AFR table has a read-only **lambda preview** beside it: the same cells as lambda"
     " for the fuel you pick (or as AFR when the table is shown in lambda), with live readouts."
     " It scrolls with the table and shows its selection. Its **\u00bb** button collapses it to a"
     " narrow strip, and the strip's button opens it again; clibretune remembers the choice.\n"),
    ("features/table-editing/2d-tables", r"- Hold \*\*Shift\*\* for 10x step\n",
     "- `q` / `w`: small step; Ctrl with `+` / `-`: the step times the multiplier\n\n"
     "The step is one unit of the ECU's own unit (1 AFR, 1 \u00b0C, 1 kPa) shown in the unit you"
     " chose, so +1 AFR is about +0.07 on a table shown in lambda; when the ECU stores the value"
     " in coarser steps (a pressure kept in 10 kPa steps) it is one of those. Curves step the"
     " same way. Right-click > **Set increments...** changes the table's step.\n"),
    ("features/autotune/usage-guide", r"This sends changes to ECU RAM\. They take effect immediately\.\n",
     "This sends changes to ECU RAM. They take effect immediately. **Undo** (Ctrl+Z) writes the\n"
     "table back as it was before the last Send to ECU.\n"),
]
FORBIDDEN_RE = re.compile(r"Install and restart|pop-out icon|Pop out dashboards|popped out|"
                          r"multi-monitor support|cryptographically signed", re.I)


def rewrite(pid, text):
    for page, pat, rep in REWRITES:
        if page != pid:
            continue
        text, n = re.subn(pat, lambda m: rep, text, flags=re.S)
        assert n > 0, "rewrite did not match in " + pid + ": " + pat
    return text


# This program is clibretune; the stock text calls it LibreTune.  Links to the original project
# and the original's own source paths keep that name.
_BRAND_KEEP = re.compile(r"RallyPat/LibreTune|libretune_core|libretune-app|crates/libretune[-\w]*")


def brand(text):
    keep = []

    def hold(m):
        keep.append(m.group(0))
        return "\x00%d\x00" % (len(keep) - 1)
    text = _BRAND_KEEP.sub(hold, text)
    text = re.sub(r"(?<![A-Za-z_])(?:LibreTune|libretune)", "clibretune", text)
    return re.sub("\x00(\\d+)\x00", lambda m: keep[int(m.group(1))], text)


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
        text = open(os.path.join(base, pid + ".md"), encoding="utf-8").read().replace("\r", "")
        text = brand(rewrite(pid, devendor(text)))
        assert not VENDOR_RE.search(text), "vendor name left in " + pid
        assert not FORBIDDEN_RE.search(text), "unsupported feature promised in " + pid
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
          % (cesc(pid), cesc(clean(brand(title))), cesc(brand(section)), depth, n, n, n))
        count += 1
    w("};\n")
    w("const int lt_man_count = %d;\n" % count)


if __name__ == "__main__":
    main()
