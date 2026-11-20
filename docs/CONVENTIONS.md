# LibreTune-C coding conventions (MANDATORY for every source file)

**The coding standard is [`UNIVERSAL_C.md`](UNIVERSAL_C.md) ("Universal C").** Read it in full;
where this file and UNIVERSAL_C.md differ, UNIVERSAL_C.md wins. Everything below is only
the project-specific glue.

Goal: one code base that compiles warning-free on gcc and clang for
`-std=c89/c99/c11/c17/c2x` and behaves identically on 32/64-bit, big/little endian, signed/
unsigned `char` and LP64/LLP64 (Linux, macOS, Windows, BSD). There is one native build per
OS; no universal "fat" binary.

## Project mapping of the standard
- The standard's project prefix `p_` / `P_` is **`lt_` / `LT_`**:
  `lt_bool LT_TRUE LT_FALSE`, `lt_u8 lt_s8 lt_u16 lt_s16 lt_u32 lt_s32`, `lt_u64 lt_s64`
  (only when `LT_HAS_64`), `LT_INLINE LT_RESTRICT LT_STATIC_ASSERT(cond, tag)
  LT_UNUSED LT_COUNTOF LT_BEGIN_DECLS LT_END_DECLS LT_STDC`.
- `src/lt.h` is the portability header and the **only** file that may test compiler/version
  macros (`__GNUC__`, `_MSC_VER`, `__STDC_VERSION__`, ...). Nothing else may use `#if` on
  compiler, OS or language-version macros.
- Section 5 helpers: `lt_add_int lt_mul_int lt_mul_size lt_add_size lt_strlcpy lt_store_u32le
  lt_load_u32le lt_store_u32be lt_load_u32be lt_rotl32 lt_u64_to_dec`, plus the older
  `lt_rd_be/le`, `lt_wr_be/le`, `lt_snprintf`, `lt_vsnprintf`, `lt_buf_*`, `lt_vec_*`, `lt_map_*`.
  `lt_vsnprintf` and `lt_buf_vprintf` are single-pass: **never use `va_copy`**.
- C99 library is banned (`snprintf strtoll round isnan ... %zu %lld`): use the `lt_` helpers and
  print sizes with `%lu` and a cast. `sprintf` is banned too; use `lt_snprintf`.
- Allocation: `lt_malloc/lt_calloc/lt_realloc/lt_free` (abort on OOM). Always overflow-check
  size arithmetic with `lt_mul_size/lt_add_size`.
- Names: public symbols `lt_<module>_<name>`, types `lt_<name>`, macros `LT_`. Also avoid every
  reserved name from UNIVERSAL_C.md section 3, including `str*/mem*/is*/to*` + lowercase
  (`stream`, `stroke`, `total`, `token`, `top`, `toolbar` ...) and `E` + uppercase (`EPS`)
  for functions, types, macros, globals, enumerators and struct members.
  Rename them (`strm`, `pen`, `sum_n`, `tok_str`, `ytop`, `bar`, `LT_EPS` ...).
- Limits: objects <= 32767 bytes (malloc larger ones), string literals <= 509 chars,
  identifiers unique in 31 chars, no `//`, no declarations after statements.
- Error convention: functions returning success use `int` 0 = ok, negative = error; set a
  message with `lt_err_set(fmt, ...)`, read it with `lt_err_get()`.
- Style: 4-space indent, braces on the same line, <= 100 columns.

## Platform code (UNIVERSAL_C.md section 11)
Only per-platform files may include OS headers; they are chosen by the **build system**
(`tools/target.sh`, used by `build.sh` and the check script), never by `#ifdef`:

| TARGET | `src/plat/` | `src/gui/os/` | link |
|--------|-------------|---------------|------|
| linux  | `posix/*.c` + `posix/os_linux.c` | `unix/stubs.c` | `-lm` |
| bsd    | `posix/*.c` + `posix/os_bsd.c`   | `unix/stubs.c` | `-lm` |
| macos  | `posix/*.c` + `posix/os_macos.c` | `macos/{be_cocoa,stubs}.c` | `-lm -framework Cocoa -framework CoreGraphics -framework CoreFoundation` |
| windows| `win32/*.c` | `win32/{be_win32,stubs}.c` | `-lws2_32 -lgdi32 -luser32 -lshell32 -ladvapi32` |

- `src/plat/plat.h` is the universal OS interface; `plat_common.c` holds the shared
  universal parts. `os_<name>.c` hold OS identity, config dir, exe path, device names.
- GUI backends that need OS APIs live in `src/gui/os/<os>/`; universal ones (`be_x11.c` raw X11
  over plat sockets, `be_web.c`, `be_tty.c`) stay in `src/gui/` and are built everywhere.
  Each OS has one `stubs.c` providing the `lt_backend_*_create` constructors that do not exist
  on it (returning NULL), so no `#ifdef` is ever needed.
- Native backends: `win32/be_win32.c` (GDI window, `SetDIBitsToDevice` 1:1 present, DPI aware, links
  user32/gdi32 directly) and `macos/be_cocoa.c` (pure C over the Objective-C runtime: `NSWindow` +
  an `NSView` subclass built with `objc_allocateClassPair`, own event pump, no Objective-C source,
  no Apple headers: the runtime/CoreGraphics prototypes are declared by hand so the file stays
  C89-strict and compiles on any host). The OS-independent half of the Cocoa backend (key codes,
  modifiers, text filter, scroll accumulation) is universal `src/gui/be_cocoa_map.c`, unit-tested
  everywhere. Cocoa canvas is in logical points; Retina doubles pixels exactly (no blur).
  The Cocoa file was written without a Mac: `tests/test_macos_smoke.c` (runs on the macOS CI
  runner, skips elsewhere or without a window-server session) is its only runtime check.
- Windows tests run under wine in a private prefix (`build/wineprefix`, null graphics driver, so
  windows can be created without a display); the serial test drives COM1 through a pty
  (`tests/os/win32/serial_wine.sh`).
- A platform file whose OS headers cannot pass `-pedantic-errors` must say so in a header comment
  that contains the words `PLATFORM FILE` (the check script skips such files on foreign hosts).
  POSIX feature-test macros (`_POSIX_C_SOURCE` ...) are passed by the build for platform files
  only; Windows files get `_WIN32_WINNT` from `src/plat/win32/win_priv.h`.
- Tests that need OS APIs live in `tests/os/<posix|win32>/` and are built only on that OS.
  Tests that need a display or a pty must skip gracefully when none is available.

## Layout
```
src/lt.h, src/lt_base.c        portability header + utilities (strings, buf, vec, map, fmt, files)
src/ini/      INI parser, expression engine, definitions model
src/proto/    ECU protocol (framing, CRC, commands, connection)
src/plat/     OS layer (plat.h interface; plat_common.c; posix/ ; win32/)
src/tune/     .msq XML, .table files, diff, migration (src/xml/ minimal XML)
src/dash/     .dash/.gauge parse+write, templates
src/datalog/  MLG/CSV read/write, recorder, playback
src/autotune/ VE analyze
src/script/   Lua 5.x subset interpreter (console, ECU script editor)
src/sim/      demo ECU simulator
src/fw/       firmware images, DfuSe / OpenBLT flashing engines, simulated bootloaders
src/basemap/  base map generator
src/tableops/ table operations, lookups, unit conversion
src/project/  projects, properties, restore points
src/gui/      immediate-mode toolkit, fonts, rasterizer, universal backends; gui/os/<os>/ native ones
src/app/      application screens wiring everything together
src/main.c
tests/        tests/test_<module>.c (universal), tests/os/<os>/ (platform), tests/fixtures/
tools/        target.sh, check_universal.sh, audit_names.py, font generators
```

## UI scale (pixel metrics)
The toolkit scale is a percentage (`lt_ui_set_scale_pct`, 100..300). Write every pixel
size as it would be at 100% and route it through `lt_ui_px(ui, n)`; toolkit entry points that
take sizes at 100% (`lt_ui_layout_row/space/indent/gap`, `lt_ui_group_begin`, `lt_ui_modal_begin`,
`lt_ui_grid_begin`, `lt_vk_*` font/icon sizes) scale them for you. Use the `_px` variants
(`lt_ui_layout_row_px`, ...) when the value is already in device pixels (taken from a rect, the
theme or a text measurement) and `lt_ui_unpx` to go back. Custom draw callbacks use
`lt_gfx_px` / `lt_gfx_fpx`. Cell (tty) mode ignores the scale.

## Build
```
./build.sh [app|test|clean|list]      TARGET=linux|macos|windows|bsd (auto), CC, CFLAGS, LDFLAGS,
                                      STRICT=1|23, RUNNER, JOBS
TARGET=windows CC=x86_64-w64-mingw32-gcc ./build.sh test      # tests run under wine
CC=s390x-linux-gnu-gcc LDFLAGS=-static RUNNER=qemu-s390x ./build.sh test   # big-endian
build.bat                                                      # MSVC
```
Windows builds use the console subsystem so `--help` prints; add `-mwindows` to `LDFLAGS` for a
console-less release build. Object files and test binaries live in `build/obj-<target>-<hash>/`
(one directory per compiler/flags combination, so different configurations never clobber each
other); `build/libretune[.exe]` is the app.

## Checking your work (THE gate)
```
tools/check_universal.sh                 # full: gcc+clang x 5 standards, platform files,
                                         # audit, cppcheck, tests (native, clang, ASan+UBSan,
                                         # -funsigned-char, -m32, s390x/qemu, Windows/wine)
tools/check_universal.sh --quick         # gcc+clang c89/c2x, native tests only
tools/check_universal.sh --only src/ini --no-tests     # just your slice
python3 tools/audit_names.py --only src/ini            # reserved names, banned calls, limits
```
All of it must pass before a change is done. The compile flags are
`-Wall -Wextra -pedantic-errors -Werror -Wconversion -Wsign-conversion -Wshadow -Wformat=2
-Wcast-align -Wstrict-prototypes -Wmissing-prototypes`.

## Tests
- `tests/test_<module>.c`, using `tests/lt_test.h` (`LT_TEST(name)`, `LT_CHECK(cond)`,
  `LT_CHECK_NEAR(a,b,eps)`, `LT_CHECK_STR(a,b)`). A test that `#include`s sources directly
  carries `LT_TEST_STANDALONE` and is linked without the library objects.
- `./build.sh test` builds all of them and runs each (under `RUNNER` when cross-building).
  Fixtures are under `tests/fixtures/`. Set environment variables with `lt_plat_setenv`
  (`putenv`/`setenv` are not C89).
