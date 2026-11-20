# Open items that need hardware, accounts or outside help

Everything here was left undone because it cannot be done or proven from a development machine
alone. Each item says what is needed and how to tell it is finished.

## 1. Hardware validation

All firmware flashing so far has run only against the simulated bootloaders in `src/fw/fw_sim.c`;
ECU communication has run against the built-in simulator and recorded fixtures. Before calling a
release stable, run this matrix on real boards and record the results (OS, board, firmware
version, LibreTune commit) in the release notes.

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
| macOS USB | The IOKit backend (`src/plat/posix/usb_macos.c`) has only ever been compiled in CI | Scan lists the bootloader; a DFU flash completes |

## 2. Signed release builds

Unsigned binaries trigger Gatekeeper (macOS) and SmartScreen (Windows) warnings.

- **macOS**: an Apple Developer ID (paid membership). Sign the universal binary with `codesign
  --options runtime` and notarize with `xcrun notarytool` in `.github/workflows/release.yml`;
  store the certificate (base64 .p12), its password and an app-specific password as repository
  secrets. Ship it inside an `.app` bundle (or a signed .zip/.dmg) so notarization can be stapled.
- **Windows**: an Authenticode certificate. Free options for open-source projects exist (for
  example SignPath Foundation); otherwise an OV certificate or Azure Trusted Signing. Sign
  `libretune.exe` with `signtool` in the release job.
- **Linux**: optionally publish SHA256SUMS signed with a project GPG key.

Done when: the release workflow signs every tagged build and a fresh machine opens it without a
warning.

## 3. Beta programme

Tag a `v0.x` beta, announce it where rusEFI, Speeduino and MegaSquirt users meet, and add GitHub
issue templates (bug report asks for OS, ECU, firmware, INI signature, and attaches the
`libretune.log` and a log file). Done when a handful of people have run it on real cars and their
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
