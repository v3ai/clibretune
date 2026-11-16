# LibreTune (C89 rewrite)

LibreTune is open-source ECU tuning software (INI-driven, rusEFI / Speeduino / MegaSquirt
style): table, curve and INI dialog editing, AutoTune, dashboards, data logging and
playback, restore points, projects and a built-in ECU simulator for demo mode.
This is a from-scratch rewrite in strictly portable C89 with no third-party dependencies.

## Build

Any C compiler (C89 or newer, tested with `-std=c89 -pedantic` and `-std=c2x`):

    ./build.sh app            # -> build/libretune
    ./build.sh test           # all test programs
    STRICT=1 ./build.sh app   # C89 pedantic, -Werror     (STRICT=23: C2x)
    cc -O2 -Isrc -o libretune src/main.c $(find src -name '*.c' ! -path src/main.c) -lm

Single portable binary (Actually Portable Executable, runs on Linux/macOS/Windows/BSD):

    ./build_cosmo.sh          # needs .toolchain/cosmocc -> build/libretune.com (~2.7 MB)

## Running

    libretune [--backend NAME] [--demo] [--ini FILE] [--project DIR]

- `--demo --ini tests/fixtures/ini/demo.ini` starts the simulated ECU, no hardware needed.
- Backends (`LT_BACKEND` or `--backend`): `auto`, `x11`, `win32`, `web`, `tty`.
  - `web` serves the UI on 127.0.0.1 and prints a tokenised URL (`LT_NO_BROWSER=1` to not open a browser).
  - `tty` renders in a terminal.
- Screenshot harness: `LT_SCREENSHOT=out.ppm [LT_SHOT_SIZE=WxH LT_SHOT_FRAMES=N LT_SHOT_OPEN=target LT_SHOT_CLICK=x,y LT_SHOT_THEME=..]`.

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
- Unit settings are stored but not yet applied to displayed values.
- Tooth/composite logger tabs and the port editor are placeholders.
- AutoTune apply/replay paths are lightly tested; the demo AFR target table is all zeros (fill it first).
- Burn button in the status bar is slightly clipped at the bottom edge.

## License and credits

GPL-2.0-or-later (see `LICENSE`). Based on the original LibreTune project
(https://github.com/RallyPat/LibreTune). Bitmap text uses GNU Unifont (GPL-2.0-or-later with
the font embedding exception); the vector font uses Hershey fonts data.
