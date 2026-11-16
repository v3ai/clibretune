# LibreTune-C architecture

Strictly portable C89 (also valid C23), one binary, one thread, no runtime
dependencies. This document fixes how the core modules fit together. The
public headers (`src/<module>/<module>.h`) are the contract; the specs in
`docs/spec/` say how each function must behave. Rules for source files are in
`docs/CONVENTIONS.md`.

## 1. Module graph

Arrows point from a module to the modules it includes/uses ("depends on").
Nothing below `lt.h` may depend on anything above it. Only `plat` touches the OS.

```
                              app / main.c  (src/app, wires everything)
        +-----------+----------+-----------+----------+----------+--------+
        |           |          |           |          |          |        |
      gui         dash      autotune    project     basemap     sim      (datalog)
        |           |          |  \         |          |          |         |
        |           |          |   \        |          |          |         |
        |           +----------+----+---> tableops ----+----------+         |
        |                      |    |         |                    |         |
        |                    tune <-+---------+--------------------+         |
        |                      |    |                              |         |
        |                      |  proto  <------------------------ +  (sim provides an lt_proto_io)
        |                      |    |                                        |
        |                      +----+--------------> ini  <--------------------+
        |                                              |
        |                              expr (ini/expr.h, used by ini, tune, dash, datalog, autotune)
        |
   xml (used by tune, dash)        json (used by project, datalog math channels)

  everyone  ->  lt.h (buf, vec, map, fmt, files, crc32 ...), lt_codes.h
  proto, sim, gui, project  ->  plat/plat.h (serial/tcp, time, dirs, files)
```

Layers (lowest first):

| layer | modules | notes |
|---|---|---|
| base | `lt.h`/`lt_base.c`, `lt_codes.h`, `plat/` | the only OS access is `plat.h` |
| formats | `xml/`, `json/`, `ini/expr.h` | no knowledge of ECUs |
| definition | `ini/ini.h` | the ECU definition model + codec for page images |
| data | `tableops/`, `tune/`, `datalog/`, `dash/` | tables/tunes/logs/dashboards in memory and on disk |
| device | `proto/`, `sim/` | `proto` talks bytes; `sim` is a virtual ECU behind the same bytes |
| features | `autotune/`, `basemap/`, `project/` | use the layers below |
| presentation | `gui/` (toolkit + backends), `src/app/` screens | immediate mode, framebuffer |

Dependency details worth knowing:

* `ini` is the hub. Everything that needs offsets, scales, names, menus or
  channels reads an `lt_ini`. `ini.h` defines `lt_pagesrc`, the tiny
  "give me page N / write bytes into page N" interface that lets the INI codec,
  `tableops`, `basemap` and `sim` work on **any** owner of page images without
  knowing who it is (`lt_ecu_mem` in proto.h, `lt_tune` in tune.h, a test array).
* `proto.h` owns the **ECU memory shadow** `lt_ecu_mem` (page images, page
  states, per-byte dirty bits, pending-burn tracking). It is the single
  implementation of spec 03 section 13 and spec 04 section 2 ("TuneCache").
  `tune.h` only converts between a `lt_tune` and an `lt_ecu_mem`.
* `sim.h` plugs into `proto.h` through `lt_proto_io` (see section 5). `sim`
  also uses `lt_pagesrc` to seed the VE table, so it does not depend on `tune`.
* `datalog.h` depends on `ini.h`/`expr.h` only (channel names, math channels),
  not on `proto`.
* `autotune.h` depends on `tableops.h` (the VE table view) and `datalog.h`
  (replay). It never touches `proto`; the app feeds it samples.
* `dash.h` includes `gui/gui.h` for `lt_gfx`/`lt_color` and renders directly.
* New shared helpers beyond the originally planned modules: `src/lt_codes.h`
  (result codes) and `src/json/json.h` (tiny JSON DOM used by project.json,
  the INI repository index and math channels).

## 2. Memory ownership

General rules (apply to every header):

1. A function that returns a pointer **named `*_new`, `*_load*`, `*_parse*`, or
   documented "malloc'd"** transfers ownership; the matching `*_free`/`*_close`/
   `lt_free` releases it. Everything else returns **borrowed** pointers.
2. Borrowed pointers into a container (`lt_ini_const *`, `lt_tune_entry *`,
   `lt_xml_node *`, `lt_dash_gauge *`, ...) stay valid until that container is
   freed or the element is replaced/removed. `lt_ini` element pointers are
   stable for the life of the definition (the GUI may cache them).
3. Structs that own strings document it; "free" functions free the *contents*
   and never the struct itself unless the name says `_free` on a pointer type
   (`lt_ini_free(lt_ini *)` frees the object; `lt_log_free(lt_log *)` clears a
   caller-allocated struct).
4. Output buffers use `lt_buf` (growable, NUL terminated) where the size is
   unknown, or `(buf, cap)` pairs where it is bounded. Nothing returns pointers
   into static storage except constant strings (`lt_rc_message`, severities).
5. Allocation failure aborts (`lt_malloc` family). No error paths for OOM.

Who owns what in a running app:

| object | owner | borrowed by |
|---|---|---|
| `lt_ini *ini` | app (`app.ini`) | proto, tune bridge, tableops views, dash, datalog, autotune, sim, basemap |
| `lt_ecu_mem *mem` | app | `lt_pagesrc` views handed to ini codec / tableops / basemap |
| `lt_tune *tune` | `lt_project` (`project.tune`) | `lt_tune_pagesrc` views, `lt_varenv.pcvars` |
| `lt_proto *proto` | app | - (borrows `ini`; io is a struct copy) |
| device (`lt_stream *`, `lt_ecusim *`) | app | `lt_proto_io` (non-owning unless `lt_proto_close`) |
| `lt_ini_rt *rt` | app | `lt_varenv`, autotune sampling, dash (via env) |
| `lt_varenv` | app | dash view, menu/dialog evaluation, tables |
| `lt_tblview` | the table editor screen | commits via a `lt_pagesrc` |
| `lt_dash_file` / `lt_dash_view` | dashboard screen | view borrows file |
| `lt_recorder` / `lt_log` / `lt_player` | datalog screen | player borrows log |
| `lt_at_state` | autotune screen | - |

## 3. Error conventions

* Anything that can fail returns `int`: `0` success, negative `LT_E*`
  (`src/lt_codes.h`). Functions that return pointers return `NULL` on failure.
  The human-readable reason is **always** set with `lt_err_set()` at the point
  of failure (include context: file name, line, page/offset) and read with
  `lt_err_get()` (single global, not thread safe - we are single threaded).
  Callers show `lt_err_get()` in a toast/dialog and do not parse it.
* Modules with structured detail keep it additionally:
  `lt_proto_errinfo(p)` (code, CRC values, status byte, page/offset for write
  verification). They still return a generic `LT_E*`:

  | proto condition | return code |
  |---|---|
  | no reply in time | `LT_ETIMEOUT` |
  | cancel flag / progress callback | `LT_ECANCEL` |
  | closed stream, not connected | `LT_ENOTCONN` |
  | bad frame/CRC | `LT_ECRC` (CRC) / `LT_EDATA` (length/structure) |
  | ECU status byte is an error code | `LT_EPROTO` |
  | refused burn after failed page | `LT_EBUSY` |
  | page/offset outside memory | `LT_ERANGE` |

* The INI parser, dash/tune readers and expression compiler are **tolerant**:
  malformed individual lines/elements are dropped, not errors. Hard errors are
  I/O, include failures, and "not a valid file" (`lt_tune_parse_msq`).
* Expression evaluation: unknown variables are 0.0; visibility/enable
  conditions that fail to compile or evaluate count as **true**
  (`lt_expr_test`), so a broken INI never hides UI.
* No `assert` for input validation (only for programmer errors). No `errno`
  exposed. No exceptions: `setjmp` is allowed inside a module only.

## 4. Threading and timing model

Single thread, cooperative, **blocking calls with timeouts**:

* `lt_proto_*` calls block until reply/timeout. For long operations (page read
  of tens of KB, write all, burn which sleeps seconds) the app installs
  `lt_proto_set_progress()`; the callback runs the GUI's frame pump (draw a
  progress bar, handle events) and returns non-zero if the user pressed
  Cancel. `lt_proto_cancel_flag()` can also be raised from a signal handler.
* Realtime streaming is a **timer in the main loop**, not a thread: each loop
  iteration, if `now - last_poll >= interval`, call `lt_proto_get_realtime`
  (a few ms at 115200 baud for ~130-300 bytes, fine).
* `lt_plat_wait()` provides the idle sleep so the loop wakes for GUI input or
  a readable serial stream; the GUI backend may add its own fd.
* All time reads use `lt_plat_now_ms()`; modules that need determinism for
  tests accept a clock (`lt_proto_set_clock`, `lt_ecusim_set_clock`,
  `lt_rec_record_at`).

## 5. How the app wires the modules (main loop)

State held in `src/app` (`lt_app`):

```
lt_project   project;      /* path, config, tune */
lt_ini      *ini;          /* from project/projectCfg/definition.ini */
lt_ecu_mem  *mem;          /* ECU page images + dirty tracking */
lt_proto    *proto;        /* NULL while offline */
lt_ini_rt   *rt;           /* latest realtime values (decoded channels + computed) */
lt_varenv    vars;         /* layered variable resolver for { expr } */
lt_expr_cache xcache;      /* compiled conditions for menus/dialogs */
lt_recorder *rec;          /* datalog recorder */
lt_at_state *at;           /* autotune */
lt_dash_view *dash;
```

### 5.1 Startup / project open

1. `lt_project_open(path)` -> `cfg`, `tune` (CurrentTune.msq, pcVariableValues.msq).
2. `ini = lt_project_load_ini(&project, opts)` (`opts.symbols` seeds `CELSIUS`
   etc. from the user's unit preference).
3. `mem = lt_ecu_mem_from_ini(ini)`; offline editing: `lt_tune_apply_to_pages(tune, ini)`
   then `lt_tune_to_mem(tune, mem)`: pages become CLEAN, the UI can edit them.
4. `rt = lt_ini_rt_new(ini)`; `pages = lt_ecu_mem_pagesrc(mem)`;
   `lt_varenv_init(&vars, ini, &pages, rt, project.tune)`;
   `lt_ini_rt_set_parent(rt, &vars.consts)` so computed channels can read
   constants (`vars.consts` = pc variables > page constants > defaults, no
   channels, hence no recursion); `vars.env` is the full resolver (channels
   first) used by everything else.
5. Build the UI trees from the INI: menus (`ini->menus`), dialogs, tables,
   curves, front-page gauges (`lt_dash_from_frontpage`) - all conditions are
   evaluated per frame with `lt_expr_cache_test(&xcache, cond, &vars.env)`.
6. `lt_ini_resolve_dynamic(ini, &vars.env)` after the tune is loaded, and again
   whenever `lt_ini_feeds_dynamic_scale(ini, changed_name)` is true.

### 5.2 Connect (real ECU)

```
lt_stream *s = lt_serial_open(port, baud);          /* or lt_tcp_connect */
lt_proto_io io = lt_proto_io_from_stream(s, LT_TRUE);
proto = lt_proto_new(&io, ini, &cfg);
lt_proto_set_progress(proto, app_progress_cb, app);
lt_proto_connect(proto);                             /* delay, handshake, flip-retry */
if (lt_proto_signature_mismatch(proto)) -> offer matching INI from lt_ini_repo
lt_proto_read_all(proto, mem, LT_FALSE);             /* pages -> mem (CLEAN) */
```

Then either keep the ECU's values (ECU is source of truth, `lt_tune_capture_from_pages`)
or send the project's tune: `lt_tune_to_mem` then `lt_proto_write_dirty` + `lt_proto_burn`.

### 5.3 Per-frame loop

```
while (running) {
    wait for input or next timer (lt_plat_wait)
    events -> gui toolkit

    if (proto && now >= next_poll) {
        if (lt_proto_get_realtime(proto, &frame) == 0) {
            lt_ini_rt_update(rt, frame.data, frame.len);        /* channels + computed */
            if (rec && recording) lt_rec_record(rec, values_for_log, n);   /* values via lt_ini_rt_value */
            if (at && at running) {
                lt_at_sample_from_rt(rt, &chmap, now, &pt);
                lt_at_add_data_point(at, &pt, xbins, nx, ybins, ny, &set, &filt, &auth);
            }
        } else handle error (toast, drop to offline after N failures)
        next_poll += interval;
    }
    if (dash) lt_dash_view_update(dash, now, &vars.env);

    draw: toolkit calls ... lt_dash_view_render(...), tables via lt_tblview ...
}
```

Edits and write-through:

* A table editor cell edit -> `lt_tblview_set` (dirty in the view) ->
  `lt_tblview_commit(view, ini, &pages)` writes the changed elements into
  `mem` through `lt_pagesrc` (the shadow marks those bytes dirty).
* A dialog field edit -> `lt_ini_const_set(ini, c, &pages, idx, display)`.
* "Write to ECU" = `lt_proto_write_dirty(proto, mem)` (sends only dirty
  ranges, chunked to blockingFactor-8, paced) then `lt_proto_burn`. When
  offline the dirty state simply stays in `mem` and `lt_tune_from_mem` copies
  the images back into the tune before `lt_project_save_tune`.
* Autotune accept: `lt_at_apply_to_view(at, view)` then the commit above.

### 5.4 Demo mode (via sim)

Demo mode runs the **same code path** over a virtual ECU:

```
lt_ini *ini = lt_ini_load_file(".../demo.ini");   /* or the project's INI */
lt_ecusim *sim = lt_ecusim_new(ini);              /* seeds VE table, och layout */
lt_proto_io io = lt_ecusim_io(sim);
proto = lt_proto_new(&io, ini, &cfg);
lt_proto_connect(proto);  lt_proto_read_all(...);  /* identical to 5.2 */
```

`lt_ecusim` answers handshake, page read/write/burn/CRC and the realtime
commands in both legacy and msEnvelope framing (endianness and page
identifiers come from the INI), and animates the engine (idle, light load,
acceleration, WOT...) whenever it is polled; `lt_ecusim_set_mode()` lets the
UI force WOT for autotune demos. Nothing in the app branches on "demo"
except the connect button choosing `lt_ecusim_io` instead of a serial port.
`lt_demo` (cosmetic channel generator) exists only for dashboards previewed
with no INI loaded.

### 5.5 Logs and playback

* Recording: `lt_datalog_channels_from_ini` -> `lt_rec_new` -> `lt_rec_start(path)`
  (MLG v2 file written as it records) -> `lt_rec_record` per realtime frame ->
  `lt_rec_stop`.
* Playback: `lt_log_read(path, &log)`, `lt_player_init`, per tick
  `lt_player_apply_to_rt(&player, ini, rt)` so gauges, tables cursor and
  autotune replay all see the logged values exactly like live ones.
* Autotune on a log: `lt_replay(&log, &chmap, ...)`.

### 5.6 Variable resolution for `{ expr }`

One resolver, `lt_varenv.env`, used everywhere (menus, dialogs, gauges, axis
labels, indicators, computed channels' constants). Lookup order, first hit wins:

1. realtime output channels incl. computed (`lt_ini_rt`)
2. PC variables stored in `project.tune` (`pcvars`)
3. constants decoded from the page images (`pages`): scalar -> display value;
   bits -> option index; array -> element 0
4. `[Defaults]` / `[ConstantsExtensions] defaultValue`
5. unknown -> 0.0

`stringValue()` / `bitStringValue()` use the INI's string constants and bit
option lists; `table()` uses the `lt_inc_cache`.

## 6. Naming and collision rules

* All public symbols are `lt_<module>_*`; types `lt_*`; macros `LT_*`.
* `gui/gui.h` owns `lt_color` (0xAARRGGBB), `lt_cell`, `lt_cells`, `lt_rect`.
  Other headers must not redefine them. Hence: `tableops` selections are
  `lt_tcell`; the INI gauge is `lt_ini_gauge` and the dashboard gauge is
  `lt_dash_gauge`; the INI logger is `lt_ini_logger`, the datalog recorder
  `lt_recorder`; expression operators are internal (not in headers) so the
  autotune/ini filter operator enums are `LT_FOP_*`.
* Enumerations use no trailing comma. Booleans are `lt_bool`
  (`LT_TRUE`/`LT_FALSE`); all real numbers are `double`; sizes `size_t`;
  bytes `lt_u8`. No 64-bit integers: millisecond clocks are `double`,
  protocol-bounded values are `unsigned long`/`lt_u32`.

## 7. Testing contract

* Every module has `tests/test_<module>.c` using `tests/lt_test.h`, run by
  `./build.sh test`; it must pass warning-free under `STRICT=1` and
  `STRICT=23`.
* `tests/test_headers.c` includes every public header together: a header that
  breaks it breaks the build for everyone, so change headers deliberately and
  note the change in the module's pull request.
* Protocol tests use a fake `lt_proto_io` (in-memory pipe whose `write` hands
  the request to a scripted responder and whose `read` pops the reply) plus
  `lt_proto_set_clock` with a fake clock so no test sleeps. The sim is the
  second implementation of the same interface used for integration tests with
  the fixtures in `tests/fixtures/` (speeduino, ms3, ms2extra, rusEFI demo.ini).
