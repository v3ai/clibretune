# LibreTune In Universal C

LibreTune is open-source ECU tuning software (INI-driven, rusEFI / Speeduino / MegaSquirt
style): table, curve and INI dialog editing, AutoTune, dashboards, data logging and
playback, restore points, projects and a built-in ECU simulator for demo mode.
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
- UI scale: `Auto` by default (X11 `Xft.dpi`/`GDK_SCALE`/`QT_SCALE_FACTOR`, Windows per-monitor DPI), or set
  `LT_SCALE=150` (percent) / Settings > Appearance > UI scale. The window opens at 1280x800 logical px
  (min 1024x700) and its size/position/maximized state are remembered in `settings.conf`.

## Keyboard shortcuts

Ctrl+N new project, Ctrl+O load tune, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+B burn,
Ctrl+Q quit, Ctrl+Shift+C / D connect / disconnect, Ctrl+D dashboard, Ctrl+L data log,
Ctrl+Shift+A autotune, Ctrl+, settings, Ctrl+W close tab, Ctrl+Tab next tab, Ctrl+1..9 tab, F9 sidebar.
Table editor: arrows/click/drag select, digits or Enter edit, `=` average, `+ - > <` step,
`q w` fine step, `*` scale, `/ h v` interpolate, `s` smooth, Ctrl+C/V copy/paste,
Ctrl+R/D fill, Ctrl+Z/Y undo/redo, `f` follow, `g` go to live cell. AutoTune: Space start/stop, L lock.

## Known limitations

- Signature-mismatch / tune-mismatch prompts on connect: ECU data simply replaces the tune.
- Table editor 3D view: no zoom/pan; no table compare or resize. Curve points drag vertically only.
- Dashboard designer: no undo, copy/paste or layers; click actions, background images and embedded fonts unsupported.
- Tooth/composite logger tabs and the port editor are placeholders.
- AutoTune apply/replay paths are lightly tested; the demo AFR target table is all zeros (fill it first).
- Burn button in the status bar is slightly clipped at the bottom edge.

## License and credits

GPL-2.0-or-later (see `LICENSE`). Based on the original LibreTune project
(https://github.com/RallyPat/LibreTune). UI text is IBM Plex Sans / IBM Plex Mono
(SIL OFL 1.1, `assets/fonts/OFL.txt`), rasterized by our own TrueType renderer (`src/gui/ttf.c`);
icons are from Lucide (ISC, `assets/icons/LICENSE`). The 8x16 bitmap font used by the terminal
backend is GNU Unifont-derived (GPL-2.0-or-later with the font embedding exception); the legacy
vector font uses Hershey fonts data.
