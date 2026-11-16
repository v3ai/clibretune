# LibreTune-C coding conventions (MANDATORY for every source file)

Goal: build with ANY C compiler from C89 through C23, needing only the C
standard library (+ POSIX for platform code). Run as one Cosmopolitan APE
binary on Linux/BSD/macOS/Windows with zero runtime dependencies.

## Language rules (C89 that is also valid C99/C11/C17/C23)
- `/* */` comments only. Never `//`.
- Declarations only at the top of a block. No `for (int i = ...)`.
- No: `inline`, `restrict`, `long long`, `<stdint.h>`, `<stdbool.h>`,
  `<inttypes.h>`, designated initializers, compound literals, variadic
  macros, flexible array members, VLAs, `__func__`, `//`, trailing comma in
  enum lists, mixed code/decls, `_Bool`, `static_assert`.
- Never use C23 keywords as identifiers: `bool true false nullptr typeof
  constexpr static_assert thread_local alignas alignof auto(as type)`.
- Every function prototype has parameters; zero-arg functions are `f(void)`.
- No K&R definitions.
- String literals <= 509 chars (C89 limit). Split long text into arrays.
- C99 library functions are FORBIDDEN: `snprintf vsnprintf strtof strtoll
  round lround trunc fmin fmax isnan isinf fabsf sqrtf powf copysign log2
  strdup strndup strcasecmp strncasecmp`. Use the `lt_` helpers in `src/lt.h`.
  `sprintf` is forbidden too (overflow risk) -> `lt_snprintf`.
- Use `lt_u8 lt_s8 lt_u16 lt_s16 lt_u32 lt_s32 lt_bool LT_TRUE LT_FALSE`
  from `src/lt.h`. Use `double` for all real numbers. 32-bit unsigned math:
  mask with `& 0xFFFFFFFFUL` (lt_u32 may be 64-bit `unsigned long`... no: it is
  guaranteed >= 32 bits; always mask when relying on wraparound).
- Do not assume `char` signedness; cast to `unsigned char` before ctype calls.
- Only `src/plat/` may include POSIX/OS headers (`unistd.h termios.h
  sys/socket.h windows.h` etc). Everything else uses only C89 headers:
  `stdio stdlib string ctype math limits float stddef stdarg assert errno
  time setjmp signal locale`.
- Allocation: `lt_malloc/lt_calloc/lt_realloc/lt_free` (abort on OOM).

## Style
- Prefix: public symbols `lt_<module>_<name>`, types `lt_<name>` (typedef'd
  struct), macros `LT_`. Static helpers may be unprefixed.
- One module = `src/<module>/<module>.h` + one or more `.c` files.
- Functions returning success: `int` 0 = ok, negative = error; set a message
  via `lt_err_set(fmt, ...)` and read with `lt_err_get()`.
- 4-space indent, braces on same line, <= 100 columns.
- Keep code warning-free under:
  `gcc -std=c89 -pedantic -Wall -Wextra -Werror` and
  `gcc -std=c2x -Wall -Wextra -Werror` (and clang if available).

## Tests
- `tests/test_<module>.c`, using `tests/lt_test.h` (`LT_TEST(name)`,
  `LT_CHECK(cond)`, `LT_CHECK_NEAR(a,b,eps)`, `LT_CHECK_STR(a,b)`).
- `make test` builds and runs all of them. Fixtures under `tests/fixtures/`.

## Layout
```
src/lt.h, src/lt_base.c        portability + utilities (strings, buf, vec, map, fmt, files)
src/ini/      INI parser, expression engine, definitions model
src/proto/    ECU protocol (framing, CRC, commands, connection)
src/plat/     OS layer: serial, tcp, time, dirs, threads-free polling
src/tune/     .msq XML, .table files, diff, migration (src/xml/ minimal XML)
src/dash/     .dash/.gauge parse+write, templates
src/datalog/  MLG/CSV read/write, recorder, playback
src/autotune/ VE analyze
src/sim/      demo ECU simulator
src/basemap/  base map generator
src/tableops/ table operations, lookups, unit conversion
src/project/  projects, properties, restore points
src/gui/      immediate-mode toolkit, font, rasterizer, backends (x11, win32, web, tty)
src/app/      application screens wiring everything together
src/main.c
```
