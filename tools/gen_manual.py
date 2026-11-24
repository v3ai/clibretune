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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import manual_fixes  # noqa: E402  (the per-page corrections)

SKIP_PREFIX = ("technical/", "features/mcp-server", "features/ai-assistant", "changelog",
               "contributing", "reference/java-to-wasm", "features/popout-windows",
               "reference/action-scripting")   # the Action Recorder is described in features/tools
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
ISSUES = "https://github.com/v3ai/clibretune/issues"

# The stock Installation page describes installers (.exe setup, .dmg, .deb, AppImage); this
# program is one native binary per OS (README.md, docs/GETTING_STARTED.md, .github/release-notes.md).
INSTALL = (
    "## Download\n\n"
    "clibretune is a single program file per operating system: no installer and nothing else to"
    " install. Download it from the [GitHub Releases](" + RELEASES + ") page:\n\n"
    "| Platform | File |\n"
    "|----------|------|\n"
    "| Windows (x86-64) | `clibretune-windows-x86_64-gui.exe` (or `clibretune-windows-x86_64.exe`,"
    " which also opens a console window) |\n"
    "| macOS (Apple Silicon and Intel) | `clibretune-macos-universal` |\n"
    "| Linux (x86-64) | `clibretune-linux-x86_64` |\n"
    "| Linux (ARM64, e.g. Raspberry Pi 4/5 with a 64-bit OS) | `clibretune-linux-aarch64` |\n\n"
    "Each release also has `SHA256SUMS` to check the download against and, for Linux,"
    " `50-clibretune.rules` (see below). Put the file anywhere you like, for example in your"
    " home folder or on a USB stick: settings are kept in your user configuration folder, not"
    " next to the program.\n\n"
    "## Windows\n\n"
    "1. Download `clibretune-windows-x86_64-gui.exe`\n"
    "2. Double-click it. The program is not code-signed yet, so SmartScreen may warn about it:"
    " click **More info**, then **Run anyway**\n"
    "3. To have it in the Start menu, make a shortcut to the file yourself\n\n"
    "### USB Drivers (Windows)\n\n"
    "rusEFI, FOME, epicEFI and genuine Arduino boards need no driver on current Windows: they"
    " show up as a `COM` port. Some Arduino clones use a CH340 USB chip; if no new COM port"
    " appears in Device Manager when you plug one in, install the"
    " [CH340 driver](https://sparks.gogo.co.nz/ch340.html) (or the"
    " [FTDI driver](https://ftdichip.com/drivers/vcp-drivers/) for FTDI adapters).\n\n"
    "Firmware flashing (**Tools \u2192 Firmware Update**) talks to the bootloader through"
    " Windows' WinUSB driver, which has to be assigned once with Zadig; the Firmware Update"
    " screen explains how.\n\n"
    "## macOS\n\n"
    "One file runs natively on both Apple Silicon and Intel Macs.\n\n"
    "1. Download `clibretune-macos-universal`\n"
    "2. Make it executable and start it from Terminal:\n\n"
    "```bash\n"
    "chmod +x clibretune-macos-universal\n"
    "./clibretune-macos-universal\n"
    "```\n\n"
    "### First Launch on macOS (Gatekeeper)\n\n"
    "The program is not signed with an Apple Developer ID or notarized, so macOS quarantines"
    " it after download and refuses to open it the first time. It is not damaged. Either"
    " right-click (Control-click) the file in Finder, choose **Open** and confirm, or remove"
    " the quarantine flag in Terminal:\n\n"
    "```bash\n"
    "xattr -d com.apple.quarantine clibretune-macos-universal\n"
    "```\n\n"
    "Only do this with downloads you trust.\n\n"
    "### Serial Ports (macOS)\n\n"
    "USB serial ECUs appear as `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`; no driver or"
    " permission is needed for them.\n\n"
    "## Linux\n\n"
    "```bash\n"
    "chmod +x clibretune-linux-x86_64\n"
    "./clibretune-linux-x86_64\n"
    "```\n\n"
    "(`clibretune-linux-aarch64` on ARM64.) The program needs no libraries beyond those of"
    " any desktop: it opens an X11 window (XWayland on Wayland desktops).\n\n"
    "### Serial Port Access (Linux)\n\n"
    "To use serial ports without root, add your user to the `dialout` group (`uucp` on Arch"
    " and some others):\n\n"
    "```bash\n"
    "sudo usermod -a -G dialout $USER\n"
    "```\n\n"
    "Log out and back in for the change to take effect. clibretune shows this command itself"
    " when a port cannot be opened for lack of permission.\n\n"
    "### Firmware Flashing (Linux)\n\n"
    "Flashing firmware talks to the ECU's USB bootloader directly, which needs a udev rule."
    " Install `50-clibretune.rules` from the release (the Firmware Update screen can also"
    " save it for you):\n\n"
    "```bash\n"
    "sudo cp 50-clibretune.rules /etc/udev/rules.d/\n"
    "sudo udevadm control --reload-rules && sudo udevadm trigger\n"
    "```\n\n"
    "then unplug and replug the device. clibretune never runs `sudo` itself.\n\n")

# The stock Troubleshooting page has a Wayland/AppImage section, React-era fixes, RUST_LOG and a
# plugin system; this is what applies to clibretune.  The connection, table and AutoTune advice
# is kept where it holds.
TROUBLE = (
    "## Connection Issues\n\n"
    "### ECU Not Detected\n\n"
    "**Symptoms**: **Connect to my ECU** finds nothing, the port is not in the list, or the port"
    " does not open\n\n"
    "**Solutions**:\n"
    "1. Use a data USB cable (some cables only charge) and try another USB port\n"
    "2. Verify the ECU has power (ignition on, LEDs lit)\n"
    "3. Read the message clibretune shows: *in use by another program* (close TunerStudio or a"
    " serial monitor), *no permission* (Linux: run the `usermod` command it shows, then log out"
    " and back in), *the device went away* (cable or ECU reset)\n"
    "4. Built-in serial ports (`/dev/ttyS*`, COM1) are skipped by the search: tick **Show all"
    " ports** in **File \u2192 Connection Settings...**\n"
    "5. Windows with a CH340 Arduino clone: install the CH340 driver (see Installation)\n\n"
    "### Connection Timeout\n\n"
    "**Symptoms**: *no answer* from the ECU\n\n"
    "**Solutions**:\n"
    "1. Verify the baud rate (usually 115200) in **File \u2192 Connection Settings...**\n"
    "2. Check the ECU is running its firmware, not its bootloader\n"
    "3. Power cycle the ECU\n"
    "4. Check that no other program uses the port\n\n"
    "### Signature Mismatch\n\n"
    "**Symptoms**: the ECU's firmware is not the one the project's definition (INI) was made for\n\n"
    "**Solutions**:\n"
    "1. Let clibretune pick the definition: with an exact match among its 167 bundled"
    " definitions it switches by itself; with a close match it warns and offers **Find Exact"
    " INI...**\n"
    "2. Otherwise use **File \u2192 Search for INI Online...**, or the INI file that came with"
    " the firmware (**File \u2192 Update Project INI...**)\n"
    "3. Update the ECU firmware to match the definition\n\n"
    "### Communication Errors\n\n"
    "**Symptoms**: random disconnects, failed frames\n\n"
    "**Solutions**:\n"
    "1. Check USB cable quality and length; add ferrite cores\n"
    "2. Look for electrical noise sources (ignition leads, coils)\n"
    "3. Watch **Tools \u2192 Connection Metrics...** (or click the connection state in the status"
    " bar): it counts good and failed realtime frames and shows the last protocol error\n\n"
    "clibretune reconnects by itself when the same device comes back after a dropout and"
    " keeps your edits.\n\n"
    "### Gauges Stall or Data Is Slow\n\n"
    "**Symptoms**: gauges freeze or update slowly; **Connection Metrics** shows a low realtime"
    " rate\n\n"
    "**Solutions**:\n"
    "1. In **File \u2192 Settings... \u2192 Connection**, set **Runtime packet mode** to **Force"
    " Burst** (Speeduino, MegaSquirt) or **Force OCH** (rusEFI-family ECUs on slow links);"
    " **Auto** picks one from the INI. It applies at the next connect\n"
    "2. Check **Realtime rate (Hz)** on the same page\n"
    "3. Verify the loaded INI matches your firmware version (a mismatch can mis-size the"
    " realtime data)\n"
    "4. Disconnect and connect again\n\n"
    "## Table Editing Issues\n\n"
    "### Values Not Saving\n\n"
    "**Symptoms**: changes lost after a restart or power-off\n\n"
    "**Solutions**:\n"
    "1. Press Ctrl+S to save the tune file in the project\n"
    "2. **Burn** (Ctrl+B) to store the tune in the ECU's flash: changes sent to the ECU only"
    " live in its RAM until then\n"
    "3. Check project folder permissions and free disk space\n\n"
    "### Wrong Values Displayed\n\n"
    "**Symptoms**: numbers don't match what you expect\n\n"
    "**Solutions**:\n"
    "1. Check the units in **File \u2192 Settings... \u2192 General** (temperature, pressure,"
    " AFR or lambda)\n"
    "2. Verify the correct INI is loaded for the firmware\n"
    "3. Connect to the ECU: clibretune compares the project tune with the ECU's and asks which"
    " to keep\n\n"
    "### Can't Edit Cells\n\n"
    "**Symptoms**: cells appear locked\n\n"
    "**Solutions**:\n"
    "1. Check if the cells are locked in AutoTune\n"
    "2. Check that the INI does not define the table as read-only\n\n"
    "## AutoTune Issues\n\n"
    "### No Recommendations\n\n"
    "**Symptoms**: all cells gray, no corrections shown\n\n"
    "**Solutions**:\n"
    "1. Check the engine is at operating temperature (**Min coolant** filter)\n"
    "2. Verify RPM is within the filter range\n"
    "3. Confirm the wideband is working\n"
    "4. Look at **Statistics** in the AutoTune panel: it counts accepted and rejected samples"
    " and names the most common rejection reasons\n\n"
    "### Samples Rejected\n\n"
    "Samples taken during **overrun fuel cut** (lift-off) and **railed wideband readings**"
    " (sensor at a stop, unpowered heater, or a cut still washing out) are excluded by design,"
    " as are samples outside the filters. If the wideband reads railed while the engine runs,"
    " fix the sensor or its heater first: those readings carry no mixture information.\n\n"
    "### Erratic Recommendations\n\n"
    "**Symptoms**: values jumping around\n\n"
    "**Solutions**:\n"
    "1. Lower **Max TPS rate**\n"
    "2. Tick **Exclude accel enrichment**\n"
    "3. Check wideband sensor health\n"
    "4. Look for vacuum and exhaust leaks\n\n"
    "### Not Reaching Cells\n\n"
    "**Symptoms**: some cells never get data\n\n"
    "**Solutions**:\n"
    "1. Drive in those RPM/load conditions, steadily (no throttle changes)\n"
    "2. Widen the filter ranges slightly\n"
    "3. Some cells may need a dyno\n\n"
    "## Dashboard Issues\n\n"
    "### Gauges Not Updating\n\n"
    "**Symptoms**: values frozen or \"--\"\n\n"
    "**Solutions**:\n"
    "1. Check the ECU connection (status bar)\n"
    "2. Press **Validate** on the dashboard toolbar: it lists gauges whose channel is not in"
    " the ECU definition\n\n"
    "### Gauges Missing\n\n"
    "**Symptoms**: the dashboard appears empty\n\n"
    "**Solutions**:\n"
    "1. Pick another dashboard with the dashboard button at the left of the dashboard toolbar"
    " (it also creates, imports and manages dashboards)\n"
    "2. Check that the dashboard file exists in the project's `dashboards` folder\n\n"
    "## Performance Issues\n\n"
    "### App Running Slowly or Using Much CPU\n\n"
    "**Solutions**:\n"
    "1. Close unused tabs\n"
    "2. Lower **Realtime rate (Hz)** in **File \u2192 Settings... \u2192 Connection**\n"
    "3. Start clibretune from a terminal with `LT_PERF=1` set: it prints frame and ECU timings"
    " every second\n"
    "4. On Linux, if the window draws wrongly, try `LT_RENDER=immediate` (redraw every frame in"
    " full) or `LT_X11_SHM=0` (no shared-memory drawing)\n\n"
    "## Diagnostics\n\n"
    "clibretune has no protocol log file or debug log level. To see what the ECU link is"
    " doing:\n\n"
    "- **Tools \u2192 Connection Metrics...** (or click the connection state in the status bar):"
    " signature, realtime rate, good and failed frames, packets and bytes sent and received,"
    " the read timeout, the protocol (CRC envelope or legacy) and the last protocol error. The"
    " same numbers are at the bottom of **File \u2192 Settings... \u2192 Connection**.\n"
    "- **Tools \u2192 ECU Console** (listed when the ECU definition declares a text console, as"
    " rusEFI and FOME do) sends commands to the ECU and shows its answers.\n"
    "- The messages in the status bar and in dialogs say what failed and what to do.\n\n"
    "For display problems, start it from a terminal with one of these set:\n\n"
    "```bash\n"
    "LT_SCALE_DEBUG=1 ./clibretune   # how the UI scale was chosen\n"
    "LT_PERF=1 ./clibretune          # frame and ECU-service timings each second\n"
    "LT_RENDER=immediate ./clibretune  # redraw every frame in full\n"
    "```\n\n"
    "## Getting Help\n\n"
    "If these solutions don't work:\n\n"
    "1. Check [GitHub Issues](" + ISSUES + ")\n"
    "2. Search existing issues first\n"
    "3. Create a new issue with:\n"
    "   - clibretune version and build (**Help \u2192 About clibretune**, or"
    " `clibretune --version`)\n"
    "   - Operating system\n"
    "   - ECU type and firmware\n"
    "   - Steps to reproduce\n"
    "   - Error messages, and the numbers from **Connection Metrics** for connection problems\n\n"
    "The build is the git commit the program was built from, for example `clibretune 0.1.1"
    " (build 44925d1)`.\n")

REWRITES = [
    ("getting-started/installation", r"## Download\n.*?(?=## Updating)", INSTALL),
    ("getting-started/installation", r"## Building from Source\n.*?(?=## Next Steps)",
     "## Building from Source\n\n"
     "clibretune builds with any C compiler and nothing else: `./build.sh app` on Linux, macOS"
     " and BSD, `build.bat` from a Visual Studio command prompt on Windows. The README in the"
     " source repository has the details.\n\n"),
    ("reference/troubleshooting", r"## Connection Issues\n.*", TROUBLE),
    ("getting-started/installation", r"## Updating\n.*?(?=## Building from Source)",
     "## Updating\n\n"
     "clibretune does not download or install updates by itself. **Help \u2192 Check for Updates**"
     " shows the version and build you are running and opens the releases page (" + RELEASES +
     ") in your web browser. Download a newer build from there and replace the old program file"
     " with it.\n\n"),
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
] + manual_fixes.FIXES
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
