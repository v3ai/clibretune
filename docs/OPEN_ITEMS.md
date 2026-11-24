# Open items that need hardware, accounts or outside help

Everything here was left undone because it cannot be done or proven from a development machine
alone. Each item says what is needed and how to tell it is finished.

## 1. Hardware validation

All firmware flashing so far has run only against the simulated bootloaders in `src/fw/fw_sim.c`;
ECU communication has run against the built-in simulator and recorded fixtures. Before calling a
release stable, run this matrix on real boards and record the results (OS, board, firmware
version, clibretune commit) in the release notes.

| Area | What to do | Done when |
|---|---|---|
| USB DFU | Flash a miata-ecus board (STM32F407) from the BOOT button: Linux, Windows (WinUSB via Zadig), macOS | Write + verify + start succeeds on all three; ECU settings in sectors 10-11 survive with *Keep ECU settings* |
| Reboot into DFU / OpenBLT | From a connected rusEFI ECU, use the reboot buttons | Bootloader appears within the wait time; after flashing, *Reconnect* works |
| OpenBLT over USB | Update a board that already has OpenBLT, no buttons | Same as above; a merged image skips the bootloader part |
| miata-ecus wideband | *Flash on-board wideband* | Status reaches "done: wideband flashed and verified" |
| Speeduino flashing | Mega 2560 over serial, Teensy 3.5/3.6/4.1 over USB (where the build supports it) | Write + verify + the ECU connects afterwards |
| Interrupted flash | Pull the USB cable mid-write | The app reports it clearly; the board is recoverable from the BOOT button |
| Tuning, rusEFI/FOME | Connect, sync choice, edit while running, burn, power-cycle, reconnect | Values persist; no false "tune differs" prompt afterwards |
| Tuning, Speeduino | Same, on a Mega 2560 and on a Teensy/STM32 board | Same |
| Tuning, MegaSquirt | Same, on MS2/Extra and MS3 | Same |
| Link faults | Unplug mid-burn and mid-log on each family | No corrupted project tune; the user is told the ECU may hold a partial burn |
| AutoTune / WUE / Trim | One real session each with a wideband | Proposed changes are sane; applied values burn correctly |
| UI speed on real desktops | Run with `LT_PERF=1` on Linux X11 and Xwayland (HiDPI and 1080p), Windows and macOS; the MIT-SHM zero-copy path, the Win32 DIB-section path and the Cocoa partial redraw were only tested against a mock X server, wine and a compile check | Frame times match `tools/bench_ui.sh`; no stale areas after resize, scale change or moving between monitors |
| macOS USB | The IOKit backend (`src/plat/posix/usb_macos.c`) has only ever been compiled in CI | Scan lists the bootloader; a DFU flash completes |

## 2. Signed release builds

Unsigned binaries trigger Gatekeeper (macOS) and SmartScreen (Windows) warnings.

- **macOS**: an Apple Developer ID (paid membership). Sign the universal binary with `codesign
  --options runtime` and notarize with `xcrun notarytool` in `.github/workflows/release.yml`;
  store the certificate (base64 .p12), its password and an app-specific password as repository
  secrets. Ship it inside an `.app` bundle (or a signed .zip/.dmg) so notarization can be stapled.
- **Windows**: an Authenticode certificate. Free options for open-source projects exist (for
  example SignPath Foundation); otherwise an OV certificate or Azure Trusted Signing. Sign
  `clibretune.exe` with `signtool` in the release job.
- **Linux**: optionally publish SHA256SUMS signed with a project GPG key.

Done when: the release workflow signs every tagged build and a fresh machine opens it without a
warning.

## 3. Beta programme

Tag a `v0.x` beta, announce it where rusEFI, Speeduino and MegaSquirt users meet, and add GitHub
issue templates (bug report asks for OS, ECU, firmware, INI signature, and attaches the
`clibretune.log` and a log file). Done when a handful of people have run it on real cars and their
reports are triaged.

## 4. Features that need hardware or vendor information

- **OpenBLT seed/key unlock**: the key algorithm is board specific (a shared library in OpenBLT).
  Needs the algorithm from each board vendor that enables it.
- **CAN bootloaders through native adapters** (PEAK PCAN, Kvaser, Linux SocketCAN): each needs its
  driver API and an adapter to test with. (OpenBLT over CAN through an SLCAN serial adapter is
  implemented and tested against a simulated adapter only.)
- **AVR EEPROM and fuses**: the Mega 2560 flasher writes flash only (Speeduino keeps its tune in
  EEPROM, which is left alone on purpose); fuse writes are not supported.
- **Teensy soft reboot**: the Teensy flasher waits for the board's program button; the 134-baud
  reboot request used by PJRC's loader is not sent yet. Needs a Teensy to test the timing.
- **OpenBLT UART checksum**: only the default framing (no checksum byte) is implemented.
- **MegaSquirt firmware loading** (MS2/MS3 serial monitor): needs the loader protocol and boards to
  test with; TunerStudio does not do this either (MegaSquirt ships its own loaders).
- **BSD USB backend**: `src/plat/posix/usb_bsd.c` is a stub; needs a BSD machine with a board.
- **Windows Teensy flashing**: the Teensy bootloader is a HID device; flashing it on Windows needs a
  HID backend (`hid.dll`/SetupAPI) in `src/plat/win32/`, and a Teensy to test with.

## 5. Base tune review

The embedded Speeduino base tunes are community calibrations. The miata-ecus research
(`review/2026-11/kb/research/R14-engine-data-and-reference-tunes.md` in that repository) found public
Miata base maps that disagree by 17-20 degrees of ignition advance at full load. Someone with the
cars should review which base tunes to recommend, and the browser should then mark them.

## 6. Maintenance runs and small follow-ups

- **Fuzzing** (`tools/fuzz/run.sh NAME SECONDS`, clang only): 13 harnesses ran about 65 minutes each
  before the first beta with no findings. Re-run them for a few hours before each release, and after
  changing a parser; keep the corpora in `build/fuzz/corpus`.
- **Real-INI corpus** (`tools/fetch_ini_corpus.sh` then `tools/ini_corpus_check.sh`): 157 rusEFI, FOME,
  Speeduino and MegaSquirt INIs, every menu screen opened under ASan/UBSan, all clean. Re-run when
  a new firmware release changes its INI.
- Logs, CSV exports, Lua scripts and logger captures are still written in place (not via
  `lt_plat_write_atomic` like tunes and settings); a crash mid-write can truncate that one export.
- **Full-screen redraws** (opening/closing a dialog, switching screens, changing the UI scale) at
  4K: `tools/bench_ui.sh --full` (every tile re-rendered every frame) measured 7-12 ms p95 at both
  150% and 200% (dashboard 10.4, VE table 9.7-10.5, datalog 11.1, curve 7.3-7.5, autotune 11.3-12.4,
  realtime 9.8-10.6). Before this work these cases were 12.4-16.3 ms. The Settings dialog
  open/close p95 went from 18.7/27.3 to 11.5/11.6 ms. The machine was under heavy load (load
  average 40+), so the bench now also prints processor time, and that was 6.8-10.6 ms p95.
  Steady frames are 0.4-2.9 ms. Already done: per-tile occlusion, the shadow ring tables, the
  span fill and blend runs, opaque/empty runs in premultiplied blits, a 4-way icon cache, and a
  Latin-1 glyph table with an ASCII kerning table. What is left:
  - About a third of a full redraw is the opaque span fills, which are bound by memory bandwidth
    on the 33 MB canvas.
  - Gauge layers (premultiplied blits) cost about 2 ms on the dashboard.
  - Text coverage blits cost about 2.3 ms on the VE table.
  - Next candidates are to treat opaque gauge faces (FCIRC) as cover through their inscribed
    square, and to keep each gauge's static layer opaque where it is.
- **Monkey testing** (`tools/monkey.py --build --runs 2000 --jobs 8`, needs
  `tools/gen_definitions.py --extract`): about 20,000 random sessions under ASan/UBSan were clean
  after the November 2026 usability pass. Re-run before each release and after UI changes.

## 7. Usability decisions

Found in the November 2026 usability pass; each needs a product decision rather than a bug fix.

- **Theme contrast** (stock themes done: tools/gen_themes.py adjusts them to WCAG 2.1 AA and
  tests/test_theme_contrast.c checks every drawn pair): themes derived from a user's bg, primary
  and accent (lt_theme_derive) are not checked; they could run the same lightness fix at runtime.
- **One spelling and one verb**: the UI mixes US and UK spelling (colour/color, centre/center)
  and "burn", "write" and "flash" for writing to the ECU.
