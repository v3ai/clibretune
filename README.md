# LibreTune In Universal C

LibreTune is open-source ECU tuning software (INI-driven, rusEFI / Speeduino / MegaSquirt
style): table, curve and INI dialog editing, AutoTune, dashboards, data logging and
playback, restore points, projects and a built-in ECU simulator for demo mode.
It also covers the TunerStudio MS/Ultra extras: WUE Analyze and per-cylinder Trim Table Analyze,
the port editor, calculators, partial tunes (`.msqpart`), expression-triggered actions with sound alerts,
MSL logs with stacked graph pages, scatter plots and histograms, X-Y log plots on curves, live math
channels, and a firmware flasher (ST DFU, OpenBLT over USB / UART / CAN, the Arduino Mega STK500v2
bootloader and Teensy HalfKay) with presets for the [miata-ecus](https://github.com/v3ai/miata-ecus) board
and Speeduino.
This is a from-scratch rewrite in Universal C (see [docs/UNIVERSAL_C.md](docs/UNIVERSAL_C.md)) with no third-party dependencies.

## Build

Separate native builds per operating system; any C compiler (C89 or newer). Nothing to install
beyond a compiler. The code follows the [Universal C](docs/UNIVERSAL_C.md) standard and builds
warning-free with gcc and clang for `-std=c89` through `-std=c2x`.

| OS | Command | Output |
|---|---|---|
| Linux / BSD | `./build.sh app` | `build/libretune` (x11 window) |
| macOS | `./build.sh app` | `build/libretune` (cocoa window; links Cocoa/CoreGraphics) |
| Windows, MSVC | `build.bat` (from a Developer Command Prompt) | `build\libretune.exe` |
| Windows, MinGW | `TARGET=windows CC=x86_64-w64-mingw32-gcc ./build.sh app` | `build/libretune.exe` |

    ./build.sh test           # build and run all test programs
    STRICT=1 ./build.sh test  # -std=c89 plus the full Universal C warning set, as errors
    tools/check_universal.sh  # full portability gate: gcc+clang over c89..c2x, sanitizers, 32-bit,
                              # big-endian (qemu), Windows (wine), cppcheck, name audit

`TARGET` is `linux`, `macos`, `windows` or `bsd` and is detected from `uname`; it selects the
per-OS source files (`src/plat/<posix|win32>/`, `src/gui/os/<unix|macos|win32>/`) in
`tools/target.sh`, never through `#ifdef`. See `docs/CONVENTIONS.md`. Prebuilt binaries for
Linux (x86-64, ARM64), Windows and macOS (universal) are attached to each release.

## Running

    libretune [--backend NAME] [--demo] [--ini FILE] [--project DIR]

- `--demo --ini tests/fixtures/ini/demo.ini` starts the simulated ECU, no hardware needed.
- Backends (`LT_BACKEND` or `--backend`): `auto`, `x11` (Linux/BSD), `cocoa` (macOS), `win32` (Windows),
  `web` and `tty` (everywhere). `x11`, `cocoa` and `win32` are native windows; `auto` picks them first.
  - `web` serves the UI on 127.0.0.1 and prints a tokenised URL (`LT_NO_BROWSER=1` to not open a browser).
  - `tty` renders in a terminal.
  - `win32` (Windows) and `cocoa` (macOS) are the native window backends; `auto` picks them first on
    their OS. On Windows `./build.sh app` (MinGW) or `build.bat` (MSVC) gives `build/libretune.exe`
    (console subsystem; add `-mwindows` to `LDFLAGS` for a console-less release build).
- Screenshot harness: `LT_SCREENSHOT=out.ppm [LT_SHOT_SIZE=WxH LT_SHOT_FRAMES=N LT_SHOT_OPEN=target LT_SHOT_CLICK=x,y LT_SHOT_THEME=.. LT_SCALE=150]`.
- UI scale: `Auto` by default (X11: the desktop's `Xft.dpi`, XSETTINGS, `GDK_SCALE`/`QT_SCALE_FACTOR`,
  else the monitor's measured density from RandR; Windows: per-monitor DPI), or set
  `LT_SCALE=150` (percent) / Settings > Appearance > UI scale, which shows what Auto picked. The window
  opens at 1280x800 logical px (min 1024x700) and its size/position/maximized state are remembered in
  `settings.conf` (`ui_scale=auto|<percent>`; the `scale` key of older builds is ignored).
  Wrong size? `LT_SCALE_DEBUG=1 ./libretune` prints every source and the decision; `--version` and
  Help > About show the build.

## Firmware update

Tools > Firmware Update flashes ECUs with no external tool:

- **USB DFU** (the STM32 ROM bootloader, `0483:df11`): ST DfuSe protocol, erasing only the flash sectors
  the image covers, then write, read-back verify and start. First flash: hold BOOT, tap RESET, release BOOT.
- **OpenBLT over USB** (`1d50:60ac`): later updates without buttons on boards with the OpenBLT bootloader;
  the bootloader part of a merged image is skipped.
- **OpenBLT over a UART** ("XCP on RS232": a length byte, then the packet, no checksum byte - OpenBLT's
  default) on any serial port, baud rate selectable (57600 default), and **OpenBLT over CAN** through an
  SLCAN / Lawicel ASCII adapter (CANable with slcan firmware, USBtin, ...): bit rate 125-1000 kbit/s and
  the CAN ids are configurable (defaults 0x667 to the ECU, 0x7E1 from it; 29-bit ids too). Both reuse the
  USB engine's XCP logic (connect, erase, program, read-back verify, reset).
- **Speeduino on an Arduino Mega 2560**: the STK500v2 ("wiring") bootloader over the board's USB serial port
  at 115200 baud, like `avrdude -c wiring`: DTR/RTS reset pulse, signature check (ATmega2560, 1E 98 01),
  256-byte page writes, read-back verify, start. Intel HEX images; the 8 KB bootloader at 0x3E000 is never
  written (an image that reaches it is refused). If the ECU connection uses the same port it is closed first.
  Ports without DTR/RTS: press the reset button after Start.
- **Speeduino on Teensy 3.5 / 3.6 / 4.1**: PJRC's HalfKay bootloader (`16c0:0478`) with HID SET_REPORT
  control transfers, block layout and timeouts as in PJRC's `teensy_loader_cli` (1024-byte blocks behind a
  64-byte header holding the 3-byte address; Teensy 4.x `.hex` files at 0x60000000 are rebased; the first
  block erases the chip; address 0xFFFFFF reboots). HalfKay cannot read back, so there is no verify. Linux
  detaches the HID driver itself. **Windows: not supported** - HalfKay is a HID device and this flasher only
  talks to WinUSB devices; use PJRC's Teensy Loader there (replacing the HID driver with Zadig would work
  in principle but is not recommended). The window may not respond for the few seconds the first block's
  erase takes.
- Images: `.bin` (at 0x08000000), Intel `.hex`, `.srec`/`.s19` and ST `.dfu` (suffix CRC checked). The
  summary shows the address range and sanity checks (vector tables, rusEFI's OpenBLT app CRC header).
- Board presets: **miata-ecus** (rusEFI on an STM32F407 with OpenBLT; "Get firmware" opens the repository's
  Firmware workflow, whose artifacts hold `rusefi_miata-ecu.bin/.dfu/.hex`), **rusEFI / FOME / epicEFI**, and
  **generic STM32**, **Speeduino (Arduino Mega 2560)**, **Speeduino (Teensy 3.5 / 3.6 / 4.1)** and **OpenBLT
  board (USB, UART or CAN)**. The rusEFI presets keep the ECU settings in flash sectors 10-11 (0x080C0000)
  unless you untick *Keep ECU settings*. A Speeduino ECU signature selects the Mega preset.
- When the ECU is connected, *Reboot ECU into DFU* / *Reboot into OpenBLT* sends the INI's `cmd_dfu` /
  `cmd_openblt` and waits for the bootloader; after a successful update the app offers to reconnect.
- miata-ecus: *Flash on-board wideband* starts the board's wideband flasher (`cmd_miata_wbo_uart_flash`, or
  the console command `wbo_uart_flash`) and follows `wbo_uart_status` until it reports done or failed.
- Linux needs access to the USB device: when permission is missing the screen shows the udev rule (all the
  ids the flasher uses: 0483:df11, 1d50:60ac, 16c0:0478) with *Copy rule* and *Save udev rule...* (writes it
  to a file you pick, default `~/50-libretune.rules`) and the two commands to install it
  (`sudo cp ... /etc/udev/rules.d/` and `sudo udevadm control --reload-rules && sudo udevadm trigger`);
  LibreTune never runs sudo itself. Serial ports need the `dialout` (or `uucp`) group; the screen says so
  when a port does not open. Windows needs the WinUSB driver on the USB bootloader device (Zadig, or
  STM32CubeProgrammer's driver).
- With `--demo` (or `LT_FW_SIM=1`) simulated bootloaders (DfuSe, OpenBLT on USB / UART / CAN, STK500v2,
  HalfKay) run the whole flow without hardware; the serial methods offer a "simulated port".

## Base tunes

File > New Project from Base Tune (also from the New Project dialog) and File > Load Base Tune open
the built-in Base Tune library: the 14 tunes of the official Speeduino tunes repository
(Generic, Audi, BMW, Mazda, Mitsubishi), embedded in the program. The browser filters by make and
searches name, board, provider, description and firmware, and shows each tune's author. For a new
project it picks the matching Speeduino INI from the INI library (or lets you choose an INI file, look
one up online, or connect to the ECU first). When the tune's firmware version differs from the
definition, or some values do not fit it, the migration report lists what does not map before
anything is applied. Base tunes are a starting point only: check injector, trigger and ignition
settings before starting an engine. rusEFI, FOME and miata-ecus boards do not need them: their
firmware carries its own base calibration (connect and read it).

## Keyboard shortcuts

Ctrl+N new project, Ctrl+O load tune, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+B burn,
Ctrl+Q quit, Ctrl+Shift+C / D connect / disconnect, Ctrl+D dashboard, Ctrl+L data log,
Ctrl+Shift+A autotune, Ctrl+, settings, Ctrl+W close tab, Ctrl+Tab next tab, Ctrl+1..9 tab, F9 sidebar.
Table editor: arrows/click/drag select, digits or Enter edit, `=` average, `+ - > <` step,
`q w` fine step, `*` scale, `/ h v` interpolate, `s` smooth, Ctrl+C/V copy/paste,
Ctrl+R/D fill, Ctrl+Z/Y undo/redo, `f` follow, `g` go to live cell. AutoTune: Space start/stop, L lock.

## Known limitations

- The full table and curve editor tabs have no `topicHelp` button (embedded editors in dialogs do).
- Raw USB access (firmware update) is implemented for Linux, Windows (WinUSB driver required, e.g. via
  Zadig) and macOS, but has only been exercised against simulated bootloaders so far; BSD has none.
  The same holds for the serial flashers (STK500v2, OpenBLT UART / SLCAN CAN) and Teensy HalfKay: they are
  written from the protocol descriptions (avrdude's wiring/stk500v2, OpenBLT's XCP transports, PJRC's
  teensy_loader_cli) and verified only against simulators. Teensy flashing does not work on Windows (HID
  driver), and on macOS it is untested (HID devices may stay owned by the HID manager).
- Firmware update: no seed/key unlock for OpenBLT, no OpenBLT UART checksum byte, no option-byte or
  read-protection changes, no AVR EEPROM or fuse writes; USB methods need this build's USB backend for the OS
  (where none exists, the screen says so - use dfu-util or STM32CubeProgrammer). The settings guard of the
  rusEFI presets assumes the STM32F4 layout (sectors 10-11). Images are downloaded in the browser
  (GitHub artifacts are zips: unpack the `.bin`/`.dfu`/`.hex` first).
- Not available by design (zero dependencies, no TLS): in-app updates, in-app HTTPS INI download
  (assisted browser flow instead), cloud AI providers (local OpenAI-compatible endpoints only),
  WASM plugins, `.ltlog` logs, pop-out windows.

## License and credits

GPL-2.0-or-later (see `LICENSE`). Based on the original LibreTune project
(https://github.com/RallyPat/LibreTune). UI text is IBM Plex Sans / IBM Plex Mono
(SIL OFL 1.1, `assets/fonts/OFL.txt`), rasterized by our own TrueType renderer (`src/gui/ttf.c`);
icons are from Lucide (ISC, `assets/icons/LICENSE`). The 8x16 bitmap font used by the terminal
backend is GNU Unifont-derived (GPL-2.0-or-later with the font embedding exception); the legacy
vector font uses Hershey fonts data.

The built-in base tunes are **not** under the GPL. They are the Speeduino base tunes from
https://github.com/speeduino/Tunes (commit 99b2a62e402144fa38c911f937b2b6571c8a720a) by the
Speeduino project and contributors (the author of each tune is shown in the browser and listed in
`assets/basetunes/speeduino/README.md`), licensed under Creative Commons Attribution-NonCommercial
4.0 (CC BY-NC 4.0, `assets/basetunes/speeduino/LICENSE`), embedded unmodified by
`tools/gen_basetunes.py` into `src/tune/basetune_data.c`. LibreTune is free software and is never
sold; because the program bundle contains these non-commercial tunes, it must not be sold either. To
distribute a build commercially, first regenerate the data without them
(`python3 tools/gen_basetunes.py --empty`). Help > About lists all licences.
