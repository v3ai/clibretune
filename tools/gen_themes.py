#!/usr/bin/env python3
"""gen_themes.py - emit src/gui/ui_theme_data.c from the stock LibreTune theme
tokens (ref/LibreTune/crates/libretune-app/src/themes/variables.css).

Usage: tools/gen_themes.py [variables.css] > src/gui/ui_theme_data.c
The token order below MUST match field_off[] in src/gui/ui_theme.c.

The stock palettes are then made to meet WCAG 2.1 AA: every colour pair listed in
tests/test_theme_contrast.c (the spec between its markers: which tokens the UI draws on
which backgrounds, and the ratio each pair needs) is checked, and a failing pair is fixed
by moving one of its colours in OKLCH lightness only: the hue stays, chroma only drops
where the lighter or darker colour would leave the sRGB gamut. The foreground moves
first; the surfaces text sits on never move (FIXED below), only fills such as button
colours and banner backgrounds may when no foreground can reach the target. Each colour
moves to the nearest lightness that passes, so a theme keeps its character. The
adjustments are listed on stderr.
"""
import math
import os
import re
import sys

TOKENS = [
    "bg-app", "bg-surface", "bg-elevated", "bg-input", "bg-hover", "bg-active",
    "bg-selected", "bg-dashboard", "bg-menubar", "bg-toolbar", "bg-statusbar",
    "bg-sidebar", "bg-tab", "bg-tab-active", "text-primary", "text-secondary",
    "text-muted", "text-disabled", "text-inverse", "text-link", "border-default",
    "border-subtle", "border-strong", "border-focus", "primary", "primary-hover",
    "primary-active", "primary-text", "accent", "accent-hover", "accent-text",
    "success", "success-bg", "warning", "warning-bg", "error", "error-bg", "info",
    "info-bg", "scrollbar-track", "scrollbar-thumb", "scrollbar-thumb-hover",
    "table-bg", "table-header-bg", "table-cell-bg", "table-cell-hover",
    "table-cell-selected", "table-cell-editing", "table-axis-bg", "table-grid-line",
    "value-richer", "value-leaner", "value-unchanged", "cursor-live", "cursor-trail",
    "cursor-selected", "cursor-active",
]
# the lt_theme field of each token, same order (the names the contrast spec uses)
FIELDS = [
    "bg_app", "surface", "elevated", "input", "hover", "active", "selected", "dash_bg",
    "menubar", "bg_toolbar", "statusbar", "sidebar", "tab", "tab_active", "text", "text2",
    "muted", "disabled", "inverse", "link", "border", "border_subtle", "border_strong",
    "focus", "primary", "primary_hover", "primary_active", "primary_text", "accent",
    "accent_hover", "accent_text", "success", "success_bg", "warning", "warning_bg", "error",
    "error_bg", "info", "info_bg", "scroll_track", "scroll_thumb", "scroll_hover", "table_bg",
    "table_header", "table_cell", "table_hover", "table_selected", "table_editing",
    "table_axis", "table_grid", "change_richer", "change_leaner", "change_same", "cursor_live",
    "cursor_trail", "cursor_selected", "cursor_active",
]
THEMES = ["dark", "light", "midnight", "carbon", "synthwave", "solarized", "nord",
          "dracula", "highcontrast"]

# surfaces that carry text: never adjusted
FIXED = {"bg_app", "surface", "elevated", "input", "hover", "active", "selected", "dash_bg",
         "menubar", "bg_toolbar", "statusbar", "sidebar", "tab", "tab_active", "scroll_track",
         "table_bg", "table_header", "table_cell", "table_axis"}
# hover/pressed fills stay visibly different from the colour they replace (OKLab L)
DISTINCT = {"primary_hover": "primary", "primary_active": "primary", "accent_hover": "accent",
            "scroll_hover": "scroll_thumb"}
MIN_DL = 0.03
# secondary text stays clearly stronger than muted text on the main surfaces (by this
# factor, or by the stock theme's own margin where that is smaller)
HIER = [("text2", "muted", 1.15), ("text", "text2", 1.15)]
HIER_BG = ["bg_app", "surface", "elevated"]
MARGIN = 0.03      # aim a little above the target so the C check never sits on the edge


def parse(path):
    css = open(path).read()
    out = {}
    for m in re.finditer(r'\[data-theme="([a-z]+)"\]\s*\{(.*?)\n\}', css, re.S):
        name, body = m.group(1), m.group(2)
        vals = {}
        for v in re.finditer(r'--([a-z0-9-]+)\s*:\s*#([0-9a-fA-F]{3,6})\s*;', body):
            h = v.group(2)
            if len(h) == 3:
                h = "".join(c * 2 for c in h)
            if len(h) == 6:
                vals[v.group(1)] = int(h, 16)
        out[name] = (("color-scheme: dark" in body), vals)
    return out


def parse_spec(path):
    """[(fg alternatives, target, bg)] from the spec table of tests/test_theme_contrast.c"""
    src = open(path).read()
    body = src.split("theme-contrast-spec-begin")[1].split("theme-contrast-spec-end")[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    pairs = []
    for m in re.finditer(r'\{\s*((?:"[^"]*"\s*)+),\s*(\d+),\s*((?:"[^"]*"\s*)+)\}', body):
        fg = "".join(re.findall(r'"([^"]*)"', m.group(1))).split()
        bg = "".join(re.findall(r'"([^"]*)"', m.group(3))).split()
        for f in fg:
            for b in bg:
                pairs.append((tuple(f.split("|")), int(m.group(2)) / 10.0, b))
    if not pairs:
        raise SystemExit("gen_themes: no contrast spec found in " + path)
    return pairs


# ---------------------------------------------------------------- colour math
def lin(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def lum(c):
    return 0.2126 * lin(c >> 16) + 0.7152 * lin((c >> 8) & 255) + 0.0722 * lin(c & 255)


def ratio(a, b):
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def bw_pick(c):
    """lt_color_contrast() of src/gui/ui_theme.c"""
    y = ((c >> 16) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) // 1000
    return 0x101010 if y > 140 else 0xFFFFFF


def to_oklch(c):
    r, g, b = lin(c >> 16), lin((c >> 8) & 255), lin(c & 255)
    l_ = (0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b) ** (1.0 / 3)
    m_ = (0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b) ** (1.0 / 3)
    s_ = (0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b) ** (1.0 / 3)
    L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_
    A = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_
    B = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
    return L, math.hypot(A, B), math.atan2(B, A)


def from_oklch(L, C, h):
    """linear-light sRGB triple (may be out of gamut)"""
    A, B = C * math.cos(h), C * math.sin(h)
    l_ = (L + 0.3963377774 * A + 0.2158037573 * B) ** 3
    m_ = (L - 0.1055613458 * A - 0.0638541728 * B) ** 3
    s_ = (L - 0.0894841775 * A - 1.2914855480 * B) ** 3
    return (4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_,
            -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_,
            -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_)


def enc(v):
    v = min(max(v, 0.0), 1.0)
    v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
    return int(round(v * 255.0))


def with_lightness(c, L):
    """c moved to OKLab lightness L, same hue; chroma reduced only to stay in gamut"""
    _, C, h = to_oklch(c)
    lo, hi = 0.0, C
    rgb = from_oklch(L, C, h)
    if min(rgb) < -1e-6 or max(rgb) > 1 + 1e-6:
        for _ in range(30):
            mid = (lo + hi) / 2
            rgb = from_oklch(L, mid, h)
            if min(rgb) < -1e-6 or max(rgb) > 1 + 1e-6:
                hi = mid
            else:
                lo = mid
        rgb = from_oklch(L, lo, h)
    return (enc(rgb[0]) << 16) | (enc(rgb[1]) << 8) | enc(rgb[2])


# ---------------------------------------------------------------- solver
def pair_ratio(pal, p):
    fgs, _, bg = p
    best = 0.0
    for f in fgs:
        c = bw_pick(pal[bg]) if f == "@contrast" else pal[f]
        best = max(best, ratio(c, pal[bg]))
    return best


def passes(pal, p, margin=0.0):
    return pair_ratio(pal, p) >= p[1] + margin


def hier_ok(pal, tok, stock):
    """tok, as the stronger of a HIER rule, stays clearly stronger"""
    for hi, lo, f in HIER:
        if tok == hi:
            for b in HIER_BG:
                need = min(f, ratio(stock[hi], stock[b]) / ratio(stock[lo], stock[b]))
                if ratio(pal[hi], pal[b]) < need * ratio(pal[lo], pal[b]) - 1e-9:
                    return False
    return True


def adjustable(tok):
    return tok not in FIXED and not tok.startswith("@")


def involved(pairs, tok):
    return [p for p in pairs if p[2] == tok or tok in p[0]]


def others_adjustable(p, tok):
    sides = [f for f in p[0] if f != tok] + ([p[2]] if p[2] != tok else [])
    return any(adjustable(s) for s in sides)


def nearest(pal, stock, pairs, tok, target, strict, dark):
    """the colour for tok nearest its stock lightness that fixes pair target, keeps pairs
    against fixed surfaces passing and (strict) every pair that passes now; None if none"""
    inv = involved(pairs, tok)
    keep = [p for p in inv if p is not target and
            (not others_adjustable(p, tok) or (strict and passes(pal, p)))]
    # a pair that fails now gets the same safety margin as the target
    margin = [0.0 if passes(pal, p) else MARGIN for p in keep]
    L0 = to_oklch(stock[tok])[0]
    base = DISTINCT.get(tok)
    old = pal[tok]
    seen = set()
    step = 0
    while step <= 1000:
        d = step * 0.001
        for L in ((L0 + d, L0 - d) if dark else (L0 - d, L0 + d)):
            if L < 0.0 or L > 1.0:
                continue
            c = with_lightness(stock[tok], L)
            if c in seen:
                continue
            seen.add(c)
            pal[tok] = c
            ok = passes(pal, target, MARGIN) and all(passes(pal, p, m) for p, m in zip(keep, margin))
            if ok and base is not None and abs(to_oklch(c)[0] - to_oklch(pal[base])[0]) < MIN_DL:
                ok = False
            if ok and not hier_ok(pal, tok, stock):
                ok = False
            pal[tok] = old
            if ok:
                return c
        step += 1
    return None


def fix_theme(name, pal, pairs, dark):
    stock = dict(pal)
    for _ in range(400):
        fails = [p for p in pairs if not passes(pal, p)]
        bad_h = [hi for hi, lo, f in HIER if not hier_ok(pal, hi, stock)]
        if not fails and not bad_h:
            return
        if fails:
            p = fails[0]
            cands = [f for f in p[0] if adjustable(f)] + ([p[2]] if adjustable(p[2]) else [])
        else:
            p = None
            cands = [bad_h[0]]
        # of the colours that could fix the pair, move the one that needs the smallest change;
        # first without breaking any pair that passes, then allowing pairs with another
        # adjustable colour to break (they are fixed in a later round)
        best = None
        for strict in (True, False):
            for tok in cands:
                c = nearest(pal, stock, pairs, tok, p if p else ((tok,), 0.0, "bg_app"), strict, dark)
                if c is not None and c != pal[tok]:
                    dl = abs(to_oklch(c)[0] - to_oklch(pal[tok])[0])
                    if best is None or dl < best[0]:
                        best = (dl, tok, c)
            if best is not None:
                break
        if best is not None:
            pal[best[1]] = best[2]
            if os.environ.get("GEN_THEMES_TRACE"):
                sys.stderr.write("  %s: %s -> #%06X for %s\n" % (name, best[1], best[2], p))
        else:
            raise SystemExit("gen_themes: %s: cannot fix %s" % (name, p if p else bad_h[0]))
    raise SystemExit("gen_themes: %s: no convergence" % name)


def report(name, before, after, pairs):
    w = sys.stderr.write
    changed = [f for f in FIELDS if before[f] != after[f]]
    w("%s:\n" % name)
    for f in changed:
        lb, la = to_oklch(before[f])[0], to_oklch(after[f])[0]
        w("  %-14s #%06X -> #%06X  (OKLab L %.3f -> %.3f)\n" % (f, before[f], after[f], lb, la))
    for tgt in (4.5, 3.0):
        sel = [p for p in pairs if p[1] == tgt]
        if sel:
            wb = min(sel, key=lambda p: pair_ratio(before, p))
            wa = min(sel, key=lambda p: pair_ratio(after, p))
            w("  worst %.1f pair: %s/%s %.2f -> %s/%s %.2f\n" % (
                tgt, "|".join(wb[0]), wb[2], pair_ratio(before, wb),
                "|".join(wa[0]), wa[2], pair_ratio(after, wa)))


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(root, "ref/LibreTune/crates/libretune-app/src/themes/variables.css")
    data = parse(path)
    pairs = parse_spec(os.path.join(root, "tests/test_theme_contrast.c"))
    for p in pairs:
        for t in list(p[0]) + [p[2]]:
            if t != "@contrast" and t not in FIELDS:
                raise SystemExit("gen_themes: unknown token %s in the contrast spec" % t)
    fixed = {}
    for name in THEMES:
        dark, vals = data[name]
        if name == "dark" or not dark:
            missing = [t for t in TOKENS if t not in vals]
            if missing:
                raise SystemExit("gen_themes: %s lacks %s" % (name, ", ".join(missing)))
        # unset tokens of dark themes fall back to the (adjusted) dark row, as in ui_theme.c
        pal = {}
        for t, f in zip(TOKENS, FIELDS):
            pal[f] = vals[t] if t in vals else fixed["dark"][f]
        before = dict(pal)
        fix_theme(name, pal, pairs, dark)
        report(name, before, pal, pairs)
        fixed[name] = pal

    w = sys.stdout.write
    w("/* ui_theme_data.c - stock LibreTune theme tokens. GENERATED by\n")
    w(" * tools/gen_themes.py from themes/variables.css, DO NOT EDIT.\n")
    w(" * Lightness adjusted where needed to meet the WCAG 2.1 AA pairs of\n")
    w(" * tests/test_theme_contrast.c.\n")
    w(" * Each row lists LT_THEME_NTOK colors as 0xRRGGBB; 0xFFFFFFFF = not set by the theme.\n")
    w(" * SPDX-License-Identifier: GPL-2.0-or-later\n */\n")
    w('#include "ui_priv.h"\n\n')
    w("const lt_theme_row lt_theme_rows[LT_THEME_NROWS] = {\n")
    for name in THEMES:
        dark, vals = data[name]
        pal = fixed[name]
        out = []
        for t, f in zip(TOKENS, FIELDS):
            if t in vals or name == "dark" or pal[f] != fixed["dark"][f]:
                out.append("0x%06XUL" % pal[f])
            else:
                out.append("0xFFFFFFFFUL")
        w('    { "%s", %d, {\n' % (name, 1 if dark else 0))
        for i in range(0, len(out), 6):
            w("        " + ", ".join(out[i:i + 6]))
            w(",\n" if i + 6 < len(out) else "\n")
        w("    } }%s\n" % ("," if name != THEMES[-1] else ""))
    w("};\n")


main()
