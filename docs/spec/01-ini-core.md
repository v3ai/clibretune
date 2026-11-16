# 01 - INI core (TunerStudio / MegaSquirt ECU definition files)

Source of truth: `ref/LibreTune/crates/libretune-core/src/ini/{mod,parser,types,constants,tables,encoding,error,inc_tables,output_channels,gauges,diagnostic_logger}.rs`.
Expression language (`{ ... }` bodies, `bitStringValue`, etc.) is OUT OF SCOPE here (see expression spec); this layer stores `{...}` bodies as raw trimmed strings.
OUT OF SCOPE: tracing/logging, serde, chrono hashing, Lua, network. `ftpBrowser` is parsed but unused.

Conventions: `lt_` prefix, C89, no heap-free-assumptions. Strings that may carry NUL bytes (ECU commands) MUST be `(ptr,len)` byte buffers, not C strings. All names (constants/channels/tables) are case-SENSITIVE; section names, keys, and keywords are case-INSENSITIVE (ASCII).

---------------------------------------------------------------------
## 1. Pipeline overview

```
bytes -> decode_ini_bytes (UTF-8 / cp1252) -> line loop
  per logical line: continuation join -> strip_comment -> trim -> skip empty
  -> preprocessor (#set #unset #if #else #endif #define #include, other '#' skipped)
  -> if inside a false #if arm: skip
  -> section header "[...]"  -> set current_section
  -> else "key = value" (split at FIRST '=') -> dispatch by lowercase(current_section)
post: flush pending reference limits; variable substitution; menu retyping;
      dynamic shapes; table sizes; ecu type; table roles (outermost only);
      diagnostic loggers (block parse of raw [LoggerDefinition] lines)
```
Errors only come from I/O / #include problems. Malformed individual lines are silently dropped (never an error). `lt_ini_parse_string` of garbage returns an empty definition with defaults.

### 1.1 Errors (error.rs)
```c
typedef enum { LT_INI_OK=0, LT_INI_E_IO, LT_INI_E_PARSE /*line,msg*/, LT_INI_E_MISSING_SECTION,
  LT_INI_E_MISSING_FIELD, LT_INI_E_INVALID_VALUE, LT_INI_E_UNKNOWN_DATATYPE, LT_INI_E_EXPRESSION,
  LT_INI_E_CIRCULAR_INCLUDE, LT_INI_E_INCLUDE_NOT_FOUND, LT_INI_E_INCLUDE_DEPTH } lt_ini_err;
```
Messages: `I/O error: %s`; `Parse error at line %d: %s`; `Missing required section: [%s]`; `Missing required field '%s' in section [%s]`; `Invalid value for '%s': %s`; `Unknown data type: %s`; `Expression parse error: %s`; `Include error: circular reference detected for '%s'`; `Include error: file not found '%s'`; `Include error: maximum depth (%d) exceeded`. In practice only IO, CIRCULAR_INCLUDE, INCLUDE_NOT_FOUND, INCLUDE_DEPTH are ever produced. (Test: error text for circular contains the word "circular".) Not-found on the TOP-LEVEL file also yields INCLUDE_NOT_FOUND (read_ini_file maps NotFound that way); other read errors -> IO.

### 1.2 Text encoding (encoding.rs)
`lt_ini_decode(bytes,len)`: (1) strip a leading UTF-8 BOM EF BB BF; (2) if remainder is STRICTLY valid UTF-8 (no overlongs, no surrogates D800-DFFF, max U+10FFFF) keep as-is; (3) else decode every byte as Windows-1252 (WHATWG variant) to UTF-8. Never emits U+FFFD. Bytes 00-7F and A0-FF map to the same code point (Latin-1). 0x80-0x9F table:
```
80 20AC 81 0081 82 201A 83 0192 84 201E 85 2026 86 2020 87 2021
88 02C6 89 2030 8A 0160 8B 2039 8C 0152 8D 008D 8E 017D 8F 008F
90 0090 91 2018 92 2019 93 201C 94 201D 95 2022 96 2013 97 2014
98 02DC 99 2122 9A 0161 9B 203A 9C 0153 9D 009D 9E 017E 9F 0178
```
Whole file is decoded at once; a file with ONE invalid UTF-8 byte is entirely cp1252.
Tests: `"Configura\xE7\xF5es"`->"Configurações"; `"90\xB0C"`->"90°C"; `"acent\xE3o ma\xF1ana \xFCber"`->"acentão mañana über"; BOM+"Mapa de Combustível" -> BOM stripped; for every single byte 0x80..0xFF result contains no U+FFFD; ASCII round-trips. File-level: `[MegaTune]\nsignature = "Configurações de Ignição 90°C"\n` as UTF-8, UTF-8+BOM (`signature="Mapa de Combust\xC3\xADvel"` -> "Mapa de Combustível") and cp1252 (`Configura\xE7\xF5es de Igni\xE7\xE3o 90\xB0C`) all give signature "Configurações de Ignição 90°C".

---------------------------------------------------------------------
## 2. Lexical layer

### 2.1 Lines
Split on `\n`; strip one trailing `\r` (Rust `str::lines`). Lone `\r` is not a separator.
**Continuation**: while the current line ends with `\` followed by optional whitespace (regex `\\\s*$`) AND another line exists: remove that suffix, append the NEXT line `trim()`med (both sides), advance. (Danger: a value ending in a literal backslash escape like `"...\"` is joined. Replicate.)

### 2.2 strip_comment(line)  (exact)
1. If first non-whitespace char is `;` -> return "" (whole-line comment, including `;name = value`).
2. has_equals = line contains a `=` outside double quotes (quote state toggles on every `"`, no escapes).
3. Scan chars with in_quotes and found_equals: `"` toggles in_quotes (kept); first `=` outside quotes sets found_equals (kept); `;` outside quotes: if has_equals -> if found_equals STOP (comment) else keep (so `fieldname;+help text = value` keeps the `;` before `=`); if no `=` in line -> STOP. Everything else kept.
4. Result is then trimmed by caller.
Tests:
```
"key = value ; comment"                      -> "key = value "
"key = \"value ; with semi\""                -> unchanged
"# comment line"                              -> unchanged   ('#' not a comment char here)
"fieldname;+help text = value"                -> unchanged
"fieldname;+help = value ; comment"           -> "fieldname;+help = value "
"      ;fuelLoadBins = array, U08, 272, [ 16], \"kPa\", 2.0, 0.0, 0.0, 511.0, 0" -> ""
"   ; keyword = referenceName, DisplayName"  -> ""
";settingOption = BOOSTPSI, \"PSI\""          -> ""
"bias_resistor;+Pull-up resistor = 4700"      -> unchanged
"someField = 42 ; trailing note" (trimmed)    -> "someField = 42"
"[MegaTune] ; this is a comment"              -> header recognised (no '=', cut at ';')
```
Consequence: a value line `blockReadTimeout = 3000; Milliseconds` -> value `3000`; `noCommReadDelay = true;` -> `true`.

### 2.3 key = value
`parse_key_value`: split at the FIRST `=`; key=trim(left), value=trim(right). No `=` -> line ignored. (Keys therefore cannot contain `=`; values can.) `key=value`, `key = value` both fine.

### 2.4 split_ini_line(value) -> list of trimmed strings (used almost everywhere)
State: in_quotes(bool), depth(int, signed, NOT clamped). For each char: `"` toggles in_quotes and is kept; `{` when !in_quotes depth++ (kept); `}` when !in_quotes depth-- (kept); `,` when !in_quotes && depth==0 splits (push trim(current)); else append. At end ALWAYS push trim(current) (so `a,b,` -> ["a","b",""] and "" -> [""]). A stray unmatched `}` makes depth negative and suppresses all later splits (replicate by testing `depth==0`). Quotes are NOT removed; callers call `trim_matches('"')` (strips ALL leading and trailing `"` chars, no unescaping).
Tests: `{ bitStringValue(algorithmUnits , algorithm) }` stays one element; `array, U08, 272, [16], { bitStringValue(algorithmUnits , algorithm) }, 1.0, 0.0, 0, 255, 0` -> units field is the braced text (see §5).
Some handlers use naive `value.split(',')` + trim (called "naive split" below) - table/curve editor, setting groups, outputchannels regular lines, gridOrient, recordField etc. Keep the exact distinction.

### 2.5 Helpers
`unquote(s)`: trim; if starts AND ends with `"` strip exactly one each side; else trimmed s.
`braced(s)`: s starts with `{` and ends with `}` (after trim). `inner(s)` = `trim_matches('{','}')` then often `.trim()` (noted per site; Field/panel/table sites do NOT inner-trim in the reference, others do; a C port should always trim - harmless).
`parse_ini_bool(s)`: cut at first `;`, trim, lowercase, true iff in {true,1,on,yes}. (Used for the "optional spec" protocol keys and filter flags.)
Numeric parsing mirrors Rust `str::parse`: whole string must be consumed after the caller's trim (no trailing junk); floats accept `inf/nan/+1.5/1e3`; unsigned ints reject a leading `-`; failure -> documented default. Implement with strtod/strtol + full-consume check.

---------------------------------------------------------------------
## 3. Preprocessor
State: `symbols` (set of strings, shared across all included files, seeded by `lt_ini_set_default_symbols()` e.g. `CELSIUS`, `LAMBDA`, `CAN_COMMANDS`, `mcu_stm32`, `COMMS_COMPAT` ...), `tested` (set), `cond_stack` (bool stack, PER FILE), `defines` map.
Directive tests are on the stripped+trimmed line, checked IN THIS ORDER:
| directive | rule |
|---|---|
| `#set NAME` | line must start with `#set ` (note space); live only if ALL stack entries true; adds trim(rest) |
| `#unset NAME` | `#unset ` prefix; live only; removes |
| `#if NAME` | prefix `#if `; symbol=trim(rest) exact string (no `!`, no `defined`, no expressions; `!X` is just a never-defined symbol); adds to `tested` ALWAYS (even in dead arms); pushes `symbols.contains(NAME)` |
| `#else` | exact line `#else`; flips top of stack (no-op if empty) |
| `#endif` | exact line `#endif`; pop (no-op if empty) |
| `#define ...` | prefix `#define`; live only; see 3.1 |
| `#include X` | prefix `#include`; live only; see 3.2 |
| other `#...` | skipped (so `#ifdef`, `#ifndef`, `#elif`, `#pragma` are ignored; an unmatched `#endif` pops wrongly - replicate) |
Note a `#` line is never a comment-strip target; `#if X ; note` works because header-like lines without `=` cut at `;`. Since `[` header lines and key lines are examined only after directives, a line like `#Another` is silently skipped.
Dead arm = any stack element false -> every non-directive line (headers too) is skipped. `#set/#unset/#define/#include` inside a dead arm have NO effect (tests: `#set LEAKED` inside `#if NEVER_DEFINED` doesn't leak; `#unset KEEPME` in dead arm doesn't clear live symbol).
After a full parse: `def.active_symbols = symbols`, `def.tested_symbols = tested` (the definition remembers which arms it took; `symbol_is_active("CELSIUS")`, `tests_symbol(s)`).
Tests (default seed = empty set unless noted):
- `#set CAN_COMMANDS` ... `#if CAN_COMMANDS queryCommand="r\x00\x0f\x00\x00\x00\x14" #else queryCommand="Q" #endif` -> query starts with `r`; without #set -> exactly `Q`.
- `#set FEATURE` `#unset FEATURE` then `#if FEATURE` -> else arm ("disabled").
- Seed {CELSIUS}: `[Constants] page=1 #if CELSIUS tempTest = scalar, U08, 0, "C", 1.0, -40, -40, 102.0, 0 #else tempTest = scalar, U08, 0, "F", 1.8, -22.23, -40, 215.0, 0 #endif` -> units "C"; empty seed -> "F". Changing the seed after parsing does not alter an already-parsed definition (`symbol_is_active("CELSIUS")` stays true).
- `tested_symbols` contains CELSIUS even if defined; INI with no `#if CELSIUS` has empty tested set.

### 3.1 `#define`
Line text after `#define` (trim), optional leading `=` (`#define = name = v`) stripped+trimmed; then split at first `=` -> name, values. values = split_ini_line(values_str) each: trim, strip `"`; if starts with `$ref` -> replace with existing `defines[ref]` list (if unknown keep the literal `$ref` text); skip empty. Store only if name non-empty and list non-empty (later define overwrites). Example: `#define gpio_list="NONE", "INVALID", "PA0", ...` (rusEFI demo has several with 100+ items). Defines are used ONLY to expand `$name` tokens in `bits` option lists at constant-parse time, so they must be defined BEFORE the constant line.

### 3.2 `#include "path"`
Path = trim(rest) with `"` stripped. Resolve: absolute as-is; else relative to the CURRENT file's directory (updated while parsing a nested include and restored after); with no base (string parse) relative to cwd. Checks in order: `depth >= 16` -> INCLUDE_DEPTH(16); canonical path already in `included_files` -> CIRCULAR_INCLUDE (set holds the root file and EVERY file ever included, never removed -> including the same file twice, even non-circularly, errors); read file (missing -> INCLUDE_NOT_FOUND(path)); recurse with FRESH parser state (page=0,last_offset=0, current_* None, own cond_stack) but SHARED symbols; then `merge(target, included)`. The included parse also runs its own post-processing; roles inference only at depth 0.
Merge rules (included `source` into `target`): maps `extend` (source overrides same key): defines, constants, output_channels, tables, table_map_to_name, curves, gauges, setting_groups, dialogs, help_topics, pc_variables, default_values, maximum_elements, indicator_panels, readout_panels, controller_commands, logger_definitions, port_editors, reference_tables, ftp_browsers, datalog_views. Lists appended: menus, datalog_entries, key_actions, maintain_constant_values, requires_power_cycle. frontpage: taken from source only if target has none. Scalars: signature, query_command, version_info, page_sizes, n_pages taken from source when source's is non-empty/non-zero (NOTE query_command default is "Q" so non-empty always -> an include ALWAYS overwrites query_command with its own value, "Q" if it didn't set one; replicate or fix deliberately). ve/wue/gamma analyze: source overrides if present. NOT merged: protocol settings, endianness, signature_prefix, ini_spec_version, diagnostic_loggers, curve_map_to_name, table_write_command/blocking_factor.
Tests: basic include; nested (main->level1->level2); a.ini<->b.ini cycle -> error containing "circular"; `#set INCLUDE_EXTRA #if INCLUDE_EXTRA #include "extra.ini" #endif` includes, absent #set does not; missing file -> error.

---------------------------------------------------------------------
## 4. Sections: dispatch and global state
`current_section` = header inner text trimmed. Header = line starting `[` ending `]` (after strip/trim). **Conditional header**: `[Name &C1 &!C2]`: name = text before first `&`; each `&`-token (split on `&`, trim, non-empty) is a symbol, optional `!` negation; if ANY fails -> section suppressed: current_section="" (following keys fall to the no-op arm until the next header). `[UiDialogs]` (case-insens) is an alias: current_section="UserDefined". Tests: `[TunerStudio &EXTRA_TUNERSTUDIO]` with `#set EXTRA_TUNERSTUDIO` overrides signature "BaseECU"->"OverrideECU"; absent -> stays; `[TunerStudio &!CAN_COMMANDS]` active when undefined.
Dispatch on `lowercase(section)` exact match:
`megatune, tunerstudio, constants, outputchannels, burstmode, gaugeconfigurations, settinggroups, pcvariables, datalog, defaults, menu, userdefined, settingcontexthelp, frontpage, controllercommands, loggerdefinition, porteditor, referencetables, ftpbrowser, datalogviews, keyactions, veanalyze, wueanalyze, gammae, constantsextensions`.
Else (default arm): if lowercase(section) CONTAINS `tableeditor` -> table editor; CONTAINS `curveeditor` -> curve editor; else if section non-empty and not passive -> warn once (stderr) "unknown section". Passive (accepted, ignored silently): `file_header verbiageoverride turbobaud replay extendedreplay accelerationwizard encodeddata tuningviews eventtriggers tools`. Everything else (`[Tuning]`, `[Tools]`'s addTool, `[EventTriggers]`, `[PcVariables]`-adjacent unknowns like `[Replay]`) is dropped. Lines before any header (e.g. rusEFI's top-level `enable2ndByteCanID = false`) are ignored (section ""), so that key keeps its default (true).

Parser state (reset per file): `current_page`(0-based), `last_offset`, `current_table`, `current_dialog`, `current_indicator_panel`, `current_readout_panel`, `current_curve`, `current_help`, `current_reference_table`, `current_setting_group`, `pending_reference_limits[]`.

### 4.1 EcuDefinition (all fields; defaults in parentheses)
```c
typedef struct lt_ecu_def {
  lt_ecu_type ecu_type;          /* UNKNOWN */
  char *signature;               /* "" */
  char *signature_prefix;        /* NULL */
  char *query_command;           /* "Q" */  /* also protocol.query_command "Q" */
  char *version_info;            /* "" */
  char *ini_spec_version;        /* "3.64" */
  lt_strlist_map defines;        /* name -> string list */
  lt_strset active_symbols, tested_symbols;
  lt_endian endianness;          /* BIG */
  u32 *page_sizes; int n_page_sizes;  /* use u32; reference truncates def.page_sizes to u16 but protocol.page_sizes is u32 */
  int n_pages;                   /* 0 */
  lt_protocol protocol;
  map constants, output_channels, tables(+table_map_to_name), curves(+curve_map_to_name unused/empty),
      gauges, setting_groups, dialogs, help_topics, indicator_panels, readout_panels,
      controller_commands, logger_definitions, port_editors, reference_tables, ftp_browsers, datalog_views;
  vec menus, datalog_entries, key_actions, maintain_constant_values, requires_power_cycle(names), diagnostic_loggers;
  map pc_variables (name->u8), default_values (name->f64), maximum_elements (name->size_t);
  lt_frontpage *frontpage; lt_veanalyze *ve_analyze; lt_wueanalyze *wue_analyze; lt_gammae *gamma_e;
  char *table_write_command; int table_blocking_factor /* -1 none */;
} lt_ecu_def;
```
Maps are keyed by exact name; later duplicate insert replaces earlier (keep insertion order separately where UI needs it: menus, datalog entries, indicator order). Iteration order of Rust HashMaps is unspecified; do not depend on it - recommend preserving file order.

### 4.2 EcuType detection (after parse; lower-cased signature + hint)
Order: contains `fome` (sig or hint) -> FOME; sig or hint contains `epicefi`/`epicecu` -> EpicEFI; sig contains `rusefi` -> RusEFI; contains `speeduino` -> Speeduino; sig startswith `ms3` -> MS3; sig startswith `ms2` or contains `ms2extra` -> MS2; else Unknown. (Hint check only applies to fome/epic: the Rust passes `ctx.base_dir.file_name()` which is actually the PARENT DIRECTORY name, not the .ini name - a bug; a port should pass the INI basename.) supports_console = RusEFI|FOME|EpicEFI. Display names: Speeduino, rusEFI, FOME, epicEFI, "MegaSquirt 2", "MegaSquirt 3", Unknown. Test: `"epicEFI dev.2026.09.12.alphax-8chan_f7.134812558"` -> EpicEFI; `("epicECU test","rusEFI2025.epicECU.ini")` -> EpicEFI.

---------------------------------------------------------------------
## 5. [MegaTune] and [TunerStudio] (identity + protocol)
Keys lowercased; values from `parse_key_value` (comments already stripped). `clean(v)` = cut at `;`, trim. `q(v)` = strip `"` both ends (trim_matches). Both sections feed the same definition.

| key (case-insens) | section | effect |
|---|---|---|
| signature | both | `signature = q(v)` (last wins) |
| signaturePrefix | both | `signature_prefix = q(v)` (msEnvelope spec 3.4: ECU sig leading bytes matching prefix is compatible) |
| queryCommand | MegaTune: `def.query_command = q(v)` ONLY; TunerStudio: ALSO `protocol.query_command` | gotcha: set both in the port |
| versionInfo | MegaTune only | `version_info=q(v)` |
| iniSpecVersion | TunerStudio | `ini_spec_version=q(v) trimmed`; warn if numeric > 14.0 (cap) |
| pageSize / pageSizes | TunerStudio (also `pagesize` in Constants) | CSV of unsigned ints (unparsable items dropped) -> page_sizes; `n_pages = count` |
| nPages / numPages | TunerStudio (`npages` in Constants) | `n_pages = parse u8 or 0` |
| endianness | TunerStudio, Constants | lowercased contains `little` -> LITTLE else BIG (default BIG) |
| defaultIpAddress | TunerStudio, Constants | `protocol.default_ip_address=q(v)` |
| defaultIpPort | both | TS: parse u16 else 29001; Constants: only set if parses |
| delayAfterPortOpen | Mega/TS/Constants | `clean` parse u32 else 0 |
| interWriteDelay | Mega/TS/Constants | u32 else 0 |
| pageActivationDelay / pageActivationDelayMs | Mega/TS (Constants: pageactivationdelay only) | u32 else 500 |
| messageEnvelopeFormat | TS, Constants | `Some(q(v))`; `uses_modern_protocol()` = value contains `msEnvelope` |
| maxUnusedRuntimeRange | Mega/TS/Constants | u32 else 0 |
| ochGetCommand | Mega/TS/OutputChannels/BurstMode | `Some(q(clean(v)))` (BurstMode: q only, no clean) |
| ochBlockSize | Mega/TS/OutputChannels/BurstMode | u32 else 0 |

### 5.1 ProtocolSettings (types.rs) with defaults
```c
typedef struct {
  char *message_envelope_format;      /* NULL */
  bytes query_command;                /* "Q" */
  u32 delay_after_port_open;          /* 0 */
  bytes *page_identifiers; int n_pid; /* decoded bytes per page */
  char **page_identifiers_raw;        /* the INI text, kept to re-substitute $vars later */
  u32 *page_sizes;
  char **page_read_commands, **page_chunk_write_commands, **burn_commands, **crc32_check_commands; /* one per page; "" = none */
  char *och_get_command;              /* NULL */
  u32 och_block_size;                 /* 0 */
  u32 max_unused_runtime_range;       /* 0 (0 = disabled) */
  char *burst_get_command;            /* "A" */
  char *retrieve_config_error;        /* NULL */
  u32 page_activation_delay;          /* 500 ms */
  u32 blocking_factor;                /* 256 */
  u32 inter_write_delay;              /* 0 */
  u32 block_read_timeout;             /* 1000 ms */
  int write_blocks;                   /* 1 */
  int enable_can_id;                  /* 1 (enable2ndByteCanID) */
  u32 default_baud_rate;              /* 115200 (never set by parser) */
  char *default_ip_address;           /* NULL */
  u16 default_ip_port;                /* 29001 */
  char *table_write_command, *table_crc_command; int table_blocking_factor /*-1*/;
  char *replay_config_table, *replay_read_command, *replay_record_count_param;
  u32 no_comm_read_delay;             /* 0 */
  int refresh_local_store_on_activity, restrict_squirt_relationship, force_big_endian_protocol,
      use_legacy_f_temp_units, suppress_config_error_verbiage, validate_array_bounds,
      ignore_missing_bit_options, filter_echo_bytes, enveloped_scan_commands;   /* all 0 */
  int default_runtime_record_per_sec; /* -1 none */
  char **page_activate_commands;
} lt_protocol;
```
`num_pages()` = count of protocol.page_sizes.

### 5.2 Protocol keys inside [Constants] (checked BEFORE constant-line parsing, case-insens, `return` after match)
`page` (see 6.1); `messageEnvelopeFormat`; `maxUnusedRuntimeRange`; `endianness`; `nPages`; `pageSize`; `pageIdentifier`; `pageReadCommand`; `pageValueWrite`|`pageChunkWrite` (both -> page_chunk_write_commands; last wins); `burnCommand`; `crc32CheckCommand`; `retrieveConfigError` (q); `pageActivationDelay` (plain u32 else 500); `blockingFactor` (u32 else 256); `interWriteDelay`; `blockReadTimeout` (else 1000); `delayAfterPortOpen`; `writeBlocks`|`tsWriteBlocks` (true iff lowercase in {on,true} or exactly "1"; any other string -> false); `enable2ndByteCanID` (false iff lowercase=="false" or value=="0"; else true); `tableWriteCommand` (q); `tableCrcCommand` (q); `tableBlockingFactor` (u32 or none); `replayConfigTable`/`replayReadCommand`/`replayRecordCountParam` (q); `noCommReadDelay` (u32 else 0 - so `true` -> 0); `refreshLocalStoreOnActivity`, `restrictSquirtRelationship`, `forceBigEndianProtocol`, `useLegacyFTempUnits`, `surpressConfigErrorVerbiage`|`suppressConfigErrorVerbiage` (sic), `validateArrayBounds`, `ignoreMissingBitOptions`, `filterEchoBytes`, `envelopedScanCommands` -> parse_ini_bool; `defaultRuntimeRecordPerSec` (u32 or none); `pageActivate` (command list); `defaultIpAddress`; `defaultIpPort`.
NOTE: several of these appear in real INIs inside `[TunerStudio]` (e.g. rusEFI's `useLegacyFTempUnits`, `ignoreMissingBitOptions`, `noCommReadDelay`, `defaultRuntimeRecordPerSec`) where the reference does NOT parse them (TunerStudio arm only knows the keys in the §5 table). A port should accept the whole §5.2 key set in both [TunerStudio] and [Constants]; the reference behavior is the minimal set.
`blockingFactor` etc: `blockingFactor = 2560 ; max chunk size` -> 2560.

### 5.3 Command lists
`parse_command_list(v)`: split_ini_line -> for each: trim, strip `"`, then `$var` substitution (5.4). Escapes (`\x00`) are LEFT AS TEXT (decoded later by the protocol layer, e.g. `"R%2i%2o%2c"`, `"B%2i"`, `""`). `%2i` page, `%2o` offset, `%2c` count, `%v` value bytes are interpreted by the protocol layer. An empty string entry (e.g. burnCommand `"B%2i", "", ""`) means "no command for that page".
`pageIdentifier`: `page_identifiers_raw` = split items with `"` stripped (pre-substitution); `page_identifiers[i]` = `parse_escape(substitute(raw))` -> bytes.
`parse_escape(s)` (chars, index i): `\x`|`\X` + 2 chars available (i+3<len) -> parse 2 hex digits to a byte (push if valid; skip 4 chars either way); `\0` -> 0x00 (2 chars); `\n` 0x0A; `\r` 0x0D; `\\` single backslash; any other `\` -> push '\\' and advance 1 (the next char is then processed normally); plain char -> push low 8 bits of the char. If `\x` has fewer than two following chars the backslash is emitted literally. (No `\t`, no octal, no `\"`.)

### 5.4 `$var` substitution `substitute(s, pc_variables)`
Scan: `\$name` or `$name` where name = [A-Za-z0-9_]+ (Unicode alphanumeric in Rust; ASCII is enough). If `pc_variables[name]` exists, replace the WHOLE token (including the backslash of `\$`) with ONE byte of that value (may be 0x00! may be >= 0x80); else leave the token text unchanged (`\$unknown` stays `\$unknown`). Tests (vars tsCanId=0 then 5): `"$tsCanId\\x04"` -> `\0` + literal `\x04`(4 chars); `"\$tsCanId\x04"` same; `"prefix$tsCanId"`->`prefix\0`; `$unknownVar` and `\$unknownVar` unchanged; tsCanId=5 -> byte 0x05.
Implementation note: result contains raw NUL bytes inside strings -> use length-carrying buffers. In Rust the byte becomes a `char` U+0000..U+00FF; for >=0x80 this encodes as 2 UTF-8 bytes but later `as u8` truncation restores the single byte for escape-decoded fields; for command strings sent as-is this is a latent bug - in C just emit the single raw byte.

### 5.5 Post-pass `post_process_variable_substitution` (runs only if pc_variables non-empty)
1. Re-resolve every pc_variable byte (see 8.2) now that `[Defaults]`/`[ConstantsExtensions]` were read.
2. Substitute in `query_command`, `protocol.query_command`, `version_info`.
3. For each command in page_read/page_chunk_write/burn/crc32 lists containing `$`: substitute.
4. If any `page_identifiers_raw` contains `$`: rebuild ALL `page_identifiers` = parse_escape(substitute(raw)).
5. och_get_command and burst_get_command if containing `$`.
(`page_activate_commands` and table_write_command are NOT re-substituted.) Result for query `"r\$tsCanId\x0f\x00\x00\x00\x14"` with tsCanId default 0: starts with `r` + NUL byte then literal `\x0f...` text.
Tests: `[PcVariables] tsCanId = bits, U08, [0:3], "CAN ID 0", "CAN ID 1", "CAN ID 2"` + `[Constants] pageIdentifier = "\$tsCanId\x04", "\$tsCanId\x05"` -> identifiers [[0,4],[0,5]], pc_variables[tsCanId]==0. Declared default: `[Constants] pageIdentifier="\x00$tsCanId" pageReadCommand="r$tsCanId%2o%2c"` + `[PcVariables] tsCanId = scalar, U08, "", 1, 0, 0, 15, 0` + `[Defaults] defaultValue = tsCanId, 3` -> pc_variables[tsCanId]==3, identifier [0,3], read cmd `r\x03%2o%2c` (byte 3 after 'r').
Test: `[Constants] delayAfterPortOpen = 1000 ; let the board settle` -> 1000 (default 0); `tsWriteBlocks = on` sets write_blocks true.

---------------------------------------------------------------------
## 6. [Constants]

### 6.1 Line grammar
`name[;+help] = class, type, offset, ...` Pages: `page = N` (1-based in INI) -> `current_page = max(N-1, 0)` (`parse u8 else 1`, saturating_sub), `last_offset = 0`. `page = 0` stays 0. Other key=value lines first tested against §5.2 keys, else constant.
Help: `extract_help_text(key)`: find first `;` in key; name = trim(before). If text after `;` (trimmed) starts with `+`: help = text after `+`, trimmed, cut at the next `;` (trim), `"` trimmed from both ends; if non-empty -> Some. No `+` -> no help. No `;` -> (trim(key), none). Tests: `bias_resistor;+Pull-up resistor value on your board` -> help "Pull-up resistor value on your board"; with `;"Ohm"` suffix same help; `baseFuelMass;+"Base mass ..."` -> unquoted; `field_name;This text...` -> no help; `"  field_name  ;  +  Help text with spaces  "` -> "Help text with spaces". (Note strip_comment keeps `;` before `=`; but `;` inside `"` after `+` ... all preserved because it is before `=`.)
Result constant stored under the clean name; `last_offset = constant.offset` (the constant's OWN offset, bits included). `lastOffset` keyword = "reuse previous constant's offset" (overlay; never advances). Tests: after `flagsByte = bits, U08, 100, [0:3], "A","B"`, `afterBits = scalar, U08, lastOffset, ...` has offset 100; `lambdaTable = array,U08,0,[16x16],...` then `afrTable = array,U08,lastOffset,[16x16],...` -> afrTable.offset 0; then `rpmBinsAFR = array,U08,256,[16],...` -> 256; `ego_min_afr=...8`, `ego_min_lambda=...lastOffset` ->8, `ego_max_afr ...9`, `ego_max_lambda lastOffset`->9.
Page numbers: `pageSize = 256, 128`, `page = 1` -> constant.page 0; `page = 2` -> 1.

### 6.2 `parse_constant_line(name, value, page, last_offset, help)` -> Constant or NULL
parts = split_ini_line(value); need >= 3 else NULL. `class = lowercase(parts[0])` (scalar|array|bits|string; any other class value is accepted and treated like scalar!). `(type, endian_override) = parse_type_with_endianness(parts[1])` else NULL. offset = `lastoffset` (case-insens) -> last_offset, else parse u16 (fails -> NULL; offsets > 65535 reject - port should use u32 and not reject, rusEFI pages are 40560 bytes but offsets stay < 65536).
```c
typedef struct { char *name,*label; u8 page; u32 offset; lt_dtype data_type; int endian_override /*-1 none,0 big? */;
  lt_shape shape; int bit_position/*-1*/, bit_size/*-1*/; i8 display_offset; char *units;
  double scale/*1*/, translate/*0*/, min/*0*/, max/*255*/; u8 digits; char *help, *visibility_condition;
  char **bit_options; int is_pc_variable; lt_dynsize *dynamic_size;
  char *scale_expr,*translate_expr,*min_expr,*max_expr; int range_resolved/*1*/; } lt_constant;
```
`Constant::new` defaults: min 0, max 255, scale 1, translate 0, digits 0, units "", range_resolved true.

Class handling:
- **bits**: `data_type = BITS` (storage type from parts[1] is discarded, only `endian_override` kept). If `parts.len()>3`: `parse_bit_range_spec(parts[3])` -> (bit_position, bit_size, display_offset) (on failure fields stay none). For parts[4..]: `opt = trim(part) minus surrounding quotes`; keep if non-empty AND not starting with `{`. Return immediately (no units/scale). Then in the caller `bit_options` entries starting with `$` are expanded via `defines` (`$name` -> the define's list; unknown -> kept as literal `$name`; empty items dropped; quotes trimmed).
  GOTCHA (rusEFI): options may be written `0="DEFAULT_FRANKENSO",22="BMW_M52",...` (value=label, sparse). Reference stores raw items like `0="DEFAULT_FRANKENSO` (leading char not a quote so only the trailing `"` is trimmed). Downstream code treats list position as the value. Recommended C behavior: if an option matches `^[0-9]+=` store a sparse `(index,label)` pair with label unquoted; otherwise positional. Also note `[0:7]` with `"", "Idle", ""` style lists include empty strings between commas: `""` -> opt "" is DROPPED (empty after quote strip), which shifts later positions - reference bug; port should preserve empty strings in positional lists (positions matter) except trailing.
- **array** (needs parts.len()>3): `parse_shape_field(parts[3])`.
- **string** (needs >3): `parts[3]` parsed as usize -> shape = Array1D(len) (size in bytes); data_type from parts[1] (ASCII/STRING -> String).
- scalar/other: nothing special.
`units_idx = 4` for bits/array (bits already returned), else 3 (NOTE: for class `string`, units_idx is 3 so `units` becomes the length text e.g. "32" - gotcha, harmless). If `parts.len()>units_idx` units = strip quotes(parts[units_idx]) (for braced unit expressions this is the whole `{ ... }` text).
`apply_scale_and_range(parts, scale_idx = units_idx+1)`: scale=parse f64 else 1.0 (and `scale_expr = deferred_expr(field)`); translate (idx+1) else 0.0; min (idx+2) else 0.0; max (idx+3) else 255.0; digits (idx+4) parse u8 else 0. `deferred_expr(f)`: trim; if parses as a number -> none; else if `{...}` -> trimmed inner (non-empty) else none. `range_resolved = (min_expr==none && max_expr==none)`. Extra trailing tokens (`noMsqSave`, `controllerPriority`) are ignored. Missing trailing fields keep defaults (so a scalar with only units gets min 0 max 255).
Examples: `reqFuel = scalar, U16, 0, "ms", 0.1, 0.0, 0, 25.5, 1` -> U16, offset 0, scale 0.1, min 0, max 25.5, digits 1. `IAT_X_Voltage = array, U16, 19, [16], "V", 0.001, 0, 0, 5.000, 3` -> digits 3, scale .001. `afrTable = array, U08, lastOffset, [16x16], "AFR", 0.1, 0.0, 7, 25.5, 1` with last_offset 1234 -> offset 1234, Array2D rows16 cols16. `loadBinsAFR = array, U08, 272, [16], { bitStringValue(algorithmUnits , algorithm) }, 1.0, 0.0, 0, 255, 0` -> 16 elements.

### 6.3 Data types
```c
typedef enum { LT_U08,LT_S08,LT_U16,LT_S16,LT_U32,LT_S32,LT_F32,LT_F64,LT_STRING,LT_BITS } lt_dtype;
```
`from_ini_str(s)`: trim + uppercase, then
U08: `U08 UINT8 BYTE BU08`; S08: `S08 INT8 SBYTE BS08`; U16: `U16 UINT16 WORD BU16`; S16: `S16 INT16 SWORD BS16`; U32: `U32 UINT32 DWORD BU32`; S32: `S32 INT32 SDWORD BS32`; F32: `F32 FLOAT BF32`; F64: `F64 DOUBLE BF64`; STRING: `ASCII STRING`; BITS: `BITS`. Unknown -> fail.
`with_endianness`: uppercase s; override = BIG if `len>1 && s[0]=='B' && s[1] is ASCII uppercase letter` (so BU16/BS32/BF32 and also BYTE and BITS, harmless). Per-field override beats the global endianness. Test (global `endianness = little`): `normalU16 = scalar, U16...` -> override none; `BU16`, `BS32`, `BU08` -> Some(Big).
`size_bytes`: U08/S08/BITS 1; U16/S16 2; U32/S32/F32 4; F64 8; STRING 0. Constant.size_bytes: BITS->0; STRING->element_count; else type_size*element_count.
`read(data,off,endian)`: if off+size > len -> none; U08/BITS byte; S08 signed; others per endianness; result as double. `raw_range_bounds`: U08/BITS 0..255; S08 -128..127; U16 0..65535; S16 -32768..32767; U32 0..4294967295; S32 -2147483648..2147483647; F32/F64/STRING none.
`write(data,off,value,endian)`: if off+size > len -> no-op. Integers: ROUND to nearest (`round()` half away from zero) THEN saturating cast (Rust `as` saturates; NaN->0) - rounding rather than truncation is essential (14.7/0.1 = 146.99999999999997 must store 147). F32: `(float)value`; F64 direct; STRING no-op. S08 stores two's complement.
Constant conversions: `raw_to_display(raw) = raw*scale + translate`. `display_to_raw(d)`: if range_resolved clamp d to [min(min,max), max(min,max)] else no clamp; `(v - translate)/scale`. `is_in_range(d)`: min<=d<=max. Test: scale 0.1: raw 147 -> 14.7 (±0.01); 14.7 -> 147±0.01.
Bit-range spec `parse_bit_range_spec(s)`: strip any leading/trailing `[` `]` chars after trim; split on `:`; start = parse u8 (fail -> none). One part -> (start,1,0). Else end part: if contains `+`: end = text before `+` (u8 else fail), display_offset = parse i8 of text after (else 0); else if rfind `-` at position >0: end = text before, display_offset = -(parse i8 after, else 0); else end = parse u8. Fail if start>=32 or end>=32. size = (end>=start) ? end-start+1 : 1. Tests: `[6:6]`->(6,1,0); `[4:7+1]` -> pos 4 size 4 offset +1; `[0:3-1]` -> (0,4,-1); `[0:7]` -> size 8; `[0:3]` size 4.
Bits value in tune memory (display): `(raw >> bit_position) & ((1<<bit_size)-1)` + display_offset (for the option index use the raw field value; display_offset is added for human numbering e.g. nCylinders [4:7+1] shows 1..16). Multi-byte tune bit extraction used by the tune code (tests/bit_extraction.rs): bytes_needed = ceil((bit_in_byte+size)/8); bit_in_byte = bit_position % 8 relative to the byte at offset + bit_position/8; value assembled LSB-byte-first: for byte i: lo = (i==0? bit_in_byte:0), hi = (i==last? bit_in_byte+size : 8); `bits = (byte>>lo) & mask(hi-lo)`; `value |= bits << (8*i)` (mask(n>=8)=0xFF). Write is the inverse preserving other bits (tests: round-trip all positions/sizes; zero size = no-op; empty data = no-op).

### 6.4 Shapes (types.rs)
```c
typedef enum {SH_SCALAR,SH_1D,SH_2D} sh_kind; typedef struct{sh_kind k; size_t n; size_t rows, cols;} lt_shape;
typedef struct{ char *cols_const; /*NULL for 1D*/ char *rows_const; /*count const for 1D*/ } lt_dynsize;
```
`parse_shape_field(s)`: trim; empty -> scalar. inner = s with ALL leading `[` and trailing `]` removed, trimmed. If inner contains `x` or `X`: split on x/X into exactly 2 parts a,b (trimmed): both `{name}` forms -> Dynamic2D{cols=a,rows=b}; both integers -> Fixed Array2D with **cols = first, rows = second** (COLUMN FIRST: `[6x2]` -> cols 6 rows 2, x_size 6, y_size 2, 12 elements; `[8X12]` -> rows 12 cols 8; the old row-first reading was a bug); else (mixed/other) -> scalar. Else if inner is `{name}` (len>=3, no inner `{`) -> Dynamic1D{count=name}; else integer -> Array1D(n); else scalar. Whitespace OK: `[ 16]`. `element_count`: scalar 1; 1D n; 2D rows*cols. `x_size`: scalar 1, 1D n, 2D cols. `y_size`: 2D rows else 1.
Dynamic shapes get placeholder (Array1D(0)/Array2D 0x0) and `dynamic_size` refs, resolved in post-pass 9.2.

### 6.5 [PcVariables]
Entries: `name[;+help] = ...`. `parse_pc_variable_line` (offset-less variant of 6.2): parts>=2; class=lowercase(parts[0]); type=parts[1] (plain `from_ini_str`, no endianness override; fail -> NULL, e.g. `tuneCrcPcVariable = continuousChannelValue, tuneCrc16` is dropped); constant page=255, offset 0, is_pc_variable=1. bits: `parts[2]` bit spec, options from parts[3..] (same filtering). array: shape = parts[2], units = parts[3], scale idx 4 (`wueAfrTargetOffset = array, S16, [ 16], ":1", 0.1, 0.0, -3.0, 3.0, 1`; min/max may be `{ 10 - 2 }` -> `max_expr`). scalar: units = parts[2], scale idx 3 (`rpmwarn = scalar, U16, "rpm", 1, 0, 0, 30000, 0` -> page 255, units "rpm", max 30000). Stored in `constants` under clean name (so dialogs find them). Test: `tsCanId = bits, U08, [0:3], "CAN ID 0", "CAN ID 1", "CAN ID 2"` -> BITS, pos 0, size 4, 3 options, display_offset 0.
Additionally if `uppercase(parts[0])` in {BITS, SCALAR, U08, S08, U16, S16}: `pc_variables[name] = resolve_pc_variable_byte(name)` (arrays not registered).

### 6.6 Dynamic sizing keys: see [ConstantsExtensions] (7.14) and post-pass 9.2.

---------------------------------------------------------------------
## 7. Other sections

### 7.1 [OutputChannels]
Keys `ochBlockSize`, `ochGetCommand` (case-insens; handled as in §5, then return). Every other `name = value` -> `parse_output_channel_line` (junk keys like `scatteredGetEnabled = { 0 }` would become computed channels - they are commented out in practice; skip known `scattered*` keys in a port).
```c
typedef struct { char *name,*label; lt_dtype data_type; u32 offset; int bit_position/*-1*/, bit_count/*-1*/;
  lt_dtype bit_storage /*U08*/; char *units; double scale/*1*/, translate/*0*/; char *expression; void *cached_ast; } lt_outch;
```
`parse_output_channel_line(name, value)`: v=trim. If starts with `{`: computed: expression = text between `{` and the FIRST `}` trimmed (no nested braces); rest after `}`: trim, drop one leading `,`, trim; units = first naive-comma token, trim, `"`-stripped. data_type=F32, offset 0, scale 1. (No `}` -> NULL.) Else naive split on `,` + trim; need >=2 parts. First token lowercased `bits`/`scalar` -> consumed (is_bits/is_scalar). Need >=2 remaining; type=parts[0] via `from_ini_str` (NO endianness-override info kept - `BU16` etc. parse as U16) else NULL; offset = parse u16 else NULL.
- bits: `bit_storage = type; data_type=BITS`; parts[2] if starts with `[`: strip `[`/`]`, split `:`; bit_position = parse u8 of first; bit_count = (pos, end parse u8 with end>=pos) -> end-pos+1; bare `[n]` -> 1; otherwise none. If parts[2] doesn't start with `[` -> it's units (quotes stripped). scale/translate stay 1/0 for bits.
- scalar / bare: units=parts[2] (quotes stripped), scale=parts[3] parse else 1.0, translate=parts[4] else 0.0. (Bare form `rpm = U16, 0, "RPM", 1.0, 0.0`; `clt = scalar, S16, 2, "C", 0.1, -40.0`.)
`raw_to_display = raw*scale+translate`. `size_bytes`: computed 0; BITS -> bit_storage size; else type size. Decode `parse(data, endian)`: computed -> none (evaluated elsewhere by the expression engine over a name->value context, result as f64); BITS with position: read the backing word of bit_storage at offset (honoring endianness; none if out of range), `word = (u64)(i64)raw` (sign-extending for signed storage), `value = (word >> pos) & mask(count)` where count=bit_count.clamp(1..64) default 1, pos>=64 -> 0; then apply scale/translate. Otherwise read type at offset, apply scale/translate.
Tests: `rpm = U16, 0, "RPM", 1.0, 0.0` -> U16/0/"RPM"; `crank = bits, U32, 852, [11:11]` -> bits, storage U32, pos 11, count 1; with offset 0 and block[1]=0b00001000 little-endian -> 1.0; `flag = bits, U32, 1, [8:8]` size_bytes 4 and parse on 2-byte block -> none; `run = bits, U08, 0, [1:1]` block 0b10 -> 1; `nSquirts = bits, U08, 84, [5:7]` count 3, 0b01000000 -> 2, 0b11100000 -> 7; `errorNum [0:1]` & `currentError [2:7]` (count 6) on byte `(42<<2)|3` -> 3 and 42; `afr_type = bits, S08, 753, [0:2]`, `launchMode [6:7]` on 0b11000101 -> 5 and 3; `hasSync = bits,U08,31,[7:7]`; computed `afr = { ego1 / 10.0 }, "AFR"` -> expression "ego1 / 10.0", units "AFR"; `clt scale 0.1 translate -40`.
Computed channels: cache parsed AST at definition time (for speed); fold passes evaluate computed channels 3 times to resolve helper dependencies.

### 7.2 [BurstMode]
`getCommand = "A"` -> `protocol.burst_get_command = q(v)`; `ochGetCommand` (q, no clean) and `ochBlockSize` also handled.

### 7.3 [GaugeConfigurations]
`gaugeCategory = "Name"` lines are NOT special-cased in the reference (they create a junk gauge named `gaugeCategory`); a port should skip key `gaugeCategory` (case-insens) or keep as category marker. Others: `name = channel, title, units, lo, hi, loDanger, loWarn, hiWarn, hiDanger, valDigits[, labelDigits]` via split_ini_line:
```c
typedef struct { char *name,*channel,*title,*units; double low_warning/*0*/,low_danger/*0*/,high_warning/*100*/,high_danger/*100*/,lo/*0*/,hi/*100*/; u8 digits;
  char *lo_expr,*hi_expr,*low_danger_expr,*low_warning_expr,*high_warning_expr,*high_danger_expr; } lt_gauge;
```
parts non-empty and parts[0] non-empty else NULL; channel=strip quotes(parts[0]); [1] title (quotes stripped; braced title kept as `{ ... }` text); [2] units; [3] lo (fallback 0); [4] hi (100); [5] low_danger (0); [6] low_warning (0); [7] high_warning (100); [8] high_danger (100); [9] digits (parse u8 else 0). Each numeric field: parse f64 OK -> value; else if `{expr}` -> value=fallback AND `*_expr` = trimmed inner; else fallback. Status helpers: `is_danger(v) = v<=low_danger || v>=high_danger`; `is_warning(v) = (v<=low_warning && v>low_danger) || (v>=high_warning && v<high_danger)`; `is_normal(v) = v>low_warning && v<high_warning`.
Tests: `rpm, "Engine Speed", "RPM", 0, 8000, 300, 600, 6500, 7000, 0` -> hi 8000, title; `rpm, "Engine Speed", "RPM", 0, {rpmhigh}, 300, 600, {rpmwarn}, {rpmdang}, 0, 0` -> hi_expr "rpmhigh", high_warning_expr "rpmwarn", high_danger_expr "rpmdang", low_danger 300, lo 0; zones rpm: low_danger 500, low_warning 800, high_warning 6500, high_danger 7000 -> 400 danger, 7500 danger, 600 warning, 6800 warning, 3000 normal; `gppwmOutput1, { bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }, "%", 0, 100, 0, 0, 100, 100, 1` -> title is the braced text, units "%", hi 100, digits 1.

### 7.4 [SettingGroups]
`settingGroup = refName, "Display"` (naive comma split; need >=2) -> group{name,label}; current_group=name. `settingOption = value, "Label"` (naive split, >=2) -> appended to CURRENT group (dropped with warning if none). Test: 8 groups each with own option end up correctly attached. Keys case-insens. (Commented `;  settingGroup` lines are comments.)

### 7.5 [Defaults]
`defaultValue = constName, number` (split_ini_line; >=2; parts[1] parse f64) -> `default_values[constName]`.

### 7.6 [SettingContextHelp]
`constName = "help text"`: if `constants[key]` exists (EXACT key; section must follow [Constants]) -> `help = strip quotes(value)`. Escape text like `\n` stays literal two chars.

### 7.7 [Datalog]
Any key (normally `entry`) -> `parts = split_ini_line(value)`; channel=strip quotes(parts[0]); fields `field(i)` = parts[i] with quotes stripped, None if empty/missing: label=field(1) or ""; data_type=field(2) or "float"; format=field(3) or "%.2f"; condition: field(4) (quotes stripped) must be `{...}` -> inner trimmed, else none; enabled=true. Columns are `Channel, Label, Type, Format[, {cond}]`. Tests: `rpm, "RPM", int, "%d"`; `time, "Time", float, "%.3f"`; `stagingActive, "Fuel Staging", int, "onOff", { stagingEnabled }` -> format "onOff", condition "stagingEnabled"; `boostDuty, "Boost Duty", int, "%d", { bitStringValue(boostLabels, boostType) }` -> condition keeps its comma; `afr, "AFR"` -> type "float", format "%.2f".

### 7.8 [FrontPage]
`gaugeN = name` (key lowercased startswith `gauge`; N parse usize; N must be 1..=64 else ignored/warn): vector grows with empty strings; `gauges[N-1] = value`. `indicator = {expr}, "off", "on", offBg, offFg, onBg, onFg`: value must start `{`; expression = text up to the brace matching depth (nested braces counted), trimmed; remaining after matching `}`: trim, drop one leading `,`, trim; split with `split_frontpage_parts` (like split_ini_line but quote toggling only when brace depth==0, and final part appended only if non-empty) -> need >=6: label_off, label_on (quotes stripped; on-label may be a `{dynamic}` expression kept verbatim), bg_off, fg_off, bg_on, fg_on (raw). If fewer than 6 or malformed -> dropped. Key `indicator` case-insens. frontpage created on first key of the section. Color names -> hex (`color_to_css`, lowercase): white #ffffff, black #000000, red #ff0000, green #00ff00, blue #0000ff, yellow #ffff00, orange #ffa500, cyan #00ffff, magenta #ff00ff, gray/grey #808080, lightgray/lightgrey #d3d3d3, darkgray/darkgrey #a9a9a9, lime #00ff00, maroon #800000, navy #000080, olive #808000, purple #800080, silver #c0c0c0, teal #008080; anything else returned lowercased as-is (hex `#rrggbb` passes through).
Test: 8 gauges tachometer..iatGauge; `indicator = { (tps > tpsflood) && (rpm < crankRPM) }, "FLOOD OFF", "FLOOD CLEAR", white, black, red, black` -> expression `(tps > tpsflood) && (rpm < crankRPM)`, bg_on red. Test cap: `gauge4000000000 = bogus` ignored, `gauge1 = accelGauge` kept, vector len <= 64.

### 7.9 [TableEditor]  (section name CONTAINS "tableeditor")
`table = tableName, mapName, "Title"[, page]` (naive split; need >=3): name=parts[0]; map_name=parts[1]; title=strip quotes(parts[2]); page=parts[3] parse u8 else 0 (NOT 1-based-normalized here); `table_map_to_name[mapName]=name`; current_table=name; default type 2D, y_size 1, x_size 0. Following keys (lowercase) apply to current table:
- `xBins = constName[, outputChannel]` -> x_bins, x_output_channel (naive split)
- `yBins = constName[, outputChannel]` -> y_bins, y_output_channel, and table_type=3D
- `zBins = constName` -> `map` (trim)
- `gridHeight = float`; `gridOrient = yaw, pitch, roll` (3 floats, naive split, all must parse); `topicHelp = "text"` -> help (quotes stripped); `xyLabels = "X", "Y"` or braced expressions (split_ini_line; x_label=[0], y_label=[1], quotes stripped); `upDownLabel = "Up", "Down"` (split_ini_line).
Test: `table = veTable1Tbl, veTable1Map, "VE Table", 2` + `xBins = rpmBins, rpm`, `yBins = fuelLoadBins, fuelLoad`, `zBins = veTable1`, `gridHeight = 2.0`, `gridOrient = 250, 0, 340` (-> (250,0,340)), `upDownLabel = "(RICHER)", "(LEANER)"`.
(Other keys inside a table block, e.g. `topicHelp`, accepted; unknown ignored.)

### 7.10 [CurveEditor]  (section contains "curveeditor")
`curve = name, "Title"` (naive; >=1 part) starts a curve. Keys: `columnLabel = "X", "Y"` (naive, >=2) -> column_labels; `xAxis = min,max,step` / `yAxis` (3 floats, f32 in reference); `xBins = bin[, outChannel]`; `yBins = bin` (FIRST token only of split_ini_line; extra output channel ignored); `size = n`; `topicHelp`; `gauge = name`; `lineLabel = "text"` (repeatable, appended; strip quotes); others (`showTextValues`, etc.) ignored.
```c
typedef struct { char *name,*title,*x_bins,*x_output_channel,*y_bins; char *col_x,*col_y; int has_xaxis; float xaxis[3]; int has_yaxis; float yaxis[3];
  int size /*-1*/; u8 page; char *help,*gauge; char **line_labels; } lt_curve;
```
Test: curve `warmup_analyzer_curve, "Warmup Enrichment"` with two `lineLabel`s -> ["Current WUE","Recommended WUE"]; `yBins = wueRates`; `knockThresholdCurve` with `xAxis = 0, 8000, 9`, `xBins = knockNoiseRpmBins, RPMValue`.

### 7.11 [Menu]
Key lowercased. 
- `menu = "Title"` (split_ini_line; first part, quotes stripped; BOTH name and title = that text, e.g. `&Setup` keeps ampersand) appends a menu. `menuDialog = main` and unknown keys ignored.
- `subMenu = ...` / `groupChildMenu = ...`: parts=split_ini_line(value). Let first=parts[0].trim. Three syntaxes:
  1. first contains `"` (space-separated target+label): target = text before first quote (trimmed); label = text between first quote and next quote (or rest); conditions: parts.len()>=3 -> (vis=cond(parts[1]), en=cond(parts[2])); ==2 -> (none, cond(parts[1])).
  2. parts.len()>=2 (comma form `target, "Label", ...`): target=first (quotes stripped), label=strip quotes(parts[1]); len>=4 -> (vis=cond(parts[2]), en=cond(parts[3])); len==3 -> (none, cond(parts[2])).
  3. only target: label = target, no conditions.
  `cond(s)`: strip `{`/`}` chars at both ends, trim; if empty or "0" or "1" -> none. (So `subMenu = x, "L", 0, {cond}` -> enable cond only; `{ 1 }, { uiMode == 0 || uiMode == 1 }` -> en = `uiMode == 0 || uiMode == 1`, vis none.)
  Empty target -> skipped. Item kind: target `std_separator` -> Separator; startswith `std_` -> Std; target in help_topics -> Help; target in tables or curves (by name only at this time) -> Table; else Dialog. Appended to the last menu; for `groupChildMenu`, appended to the `items` of the menu's LAST item if that is a SubMenu (else falls to menu level).
- `groupMenu = "Label"[, vis/en conds]` (split_ini_line; conds: len>=3 -> (vis=cond(p1), en=cond(p2)); len==2 -> (none, cond(p1))) -> SubMenu item with empty items on the last menu.
- `std_separator` key -> Separator item.
Post-pass `post_process_menu_items` (recursively through SubMenus): any `Dialog` item whose target is a help topic -> Help; whose target is a table name OR a table map_name OR a curve name -> Table (only Dialog items are retyped; visible/enabled set true).
```c
typedef enum {MI_DIALOG,MI_TABLE,MI_SUBMENU,MI_STD,MI_HELP,MI_SEPARATOR} mi_kind;
typedef struct lt_menuitem { mi_kind kind; char *label,*target; char *vis_cond,*en_cond; int visible/*1*/,enabled/*1*/; struct lt_menuitem *items; int n_items; } lt_menuitem;
```
Tests: `menu = "Test Menu"` with `subMenu = dwell_tblMap    "Dwell Map", { useDwellMap }`, `subMenu = stagingMap      "Fuel Staging", { stagingMode == 0 }`, `subMenu = normalMenu, "Normal Label", { someCondition }` -> 3 Dialog items, labels do not start with `{`, enable conds "useDwellMap", "stagingMode == 0", "someCondition". `subMenu = dcMotorActuatorHw  "DC motor actuator(s) hardware", { 1 }, { uiMode == 0 || uiMode == 1 }` -> target dcMotorActuatorHw, label, vis none, en `uiMode == 0 || uiMode == 1`.

### 7.12 [UserDefined]  (dialogs, panels, help)
Keys lowercased; maintains `current_dialog`, `current_indicator_panel`, `current_readout_panel`, `current_help`. Indentation is irrelevant (components attach to the most recent dialog).
- `help = name[, "Title"]`: topic (title defaults to name), becomes current_help. `webHelp = "url"` -> current help web_url (quotes stripped). `text = "line"` -> appended to current help text_lines (trim, strip quotes).
- `dialog = name, "Title"[, layoutHint]` (split_ini_line): title defaults to name; layout_hint = first part at index>=2 not starting `{` (e.g. `xAxis`, `border`); sets current_dialog, clears current panels. Test: `dialog = userTable1left, ""` -> empty title.
- `topicHelp = "url"` -> current dialog.topic_help (overwrites).
- `indicatorPanel = name[, columns][, {visibility}]`: columns parse u8 of parts[1] else 2; visibility = first braced part at idx>=2 (inner, `{}` chars trimmed only at ends, NO inner trim); sets current_indicator_panel, clears readout panel AND dialog.
- `readoutPanel = name[, cols][, {vis}]`: columns = first non-braced numeric u8 part at idx>=1 else 1; vis = first braced part idx>=1 (inner trimmed). Sets current_readout_panel, clears panel and dialog.
- `readout = channel, "Title"[, "units"], ..., digits, precision` (current readout panel; need >=2): channel=parts[0]; title; units=parts[2] (quotes stripped) or ""; digits = parts[len-2] parse u8 else 0; precision = parts[len-1] parse u8 else 0.
- `indicator = {expr}, "off", "on"[, offFg, offBg, onFg, onBg]` (need >=3): expression = parts[0] with `{`/`}` trimmed at ends (not inner-trimmed); labels quotes stripped; colors parts[3..6] raw (note ORDER here is fg/bg per pair: `color_off_fg=parts[3], color_off_bg=parts[4], color_on_fg=parts[5], color_on_bg=parts[6]` but FrontPage order is `offBg, offFg, onBg, onFg` - follow each as coded). If current_indicator_panel is set -> added to that panel; else (if current dialog) -> dialog Indicator{expression,label_off,label_on}.
Dialog components (require current_dialog; unknown keys ignored):
| key | parse | component |
|---|---|---|
| panel | split_ini_line; name=parts[0]; position = first part idx>=1 not starting `{`; brace parts = all `{..}` parts (inner trimmed); conditions: 2+ -> enabled=b[0], visibility=b[1]; 1 -> enabled=b[0] only; each trivial value ("", "0", "1") -> none | Panel{name,position,enabled_cond,visibility_cond}. NOTE order enabled-first here. Test dual-condition in `test_parse_panel_dual_condition`. A panel name like `std_ms3Rtc`/`std_injection` refers to built-in panels (see 7.15) |
| liveGraph | `name, "Title"[, position]` (>=2) position default "South" | LiveGraph{channels=[]} |
| graphLine | value trimmed | appended to LAST component if it is a LiveGraph |
| runtimeValue | `"Label", channel[, {vis}]` (>=2); vis = first braced part idx>=2 inner trimmed | RuntimeValue |
| field | `"Label", name[, {c1}[, {c2}]]` (>=2): braces: 2+ -> visibility=c[0], enabled=c[1]; 1 -> enabled=c[0]; (inner NOT trimmed: `{ x }` keeps spaces). With ONE part (e.g. `field = "# blue text"` or `"! red text"` or `"normal text"`) -> Label{text}. Leading `#`/`!` in the label text = blue section header / red note styling by convention | Field/Label |
| checkbox | `"Label", name[, flipValue][, {c1}[, {c2}]]` (>=2); `flipValue` case-insens token; conds as field (inner trimmed): 2 -> vis=c0, en=c1; 1 -> en=c0 | Checkbox{flip_value}: unflipped: checked iff value!=0, writes 1/0; flipped: inverted |
| slider | `"Label", name[, horizontal|vertical][, {c1}[, {c2}]]` (>=2) | Slider{orientation lowercased or none} |
| displayOnlyField | `"Label"[, name][, {c1}[, {c2}]]`: name = parts[1] unless empty, `blankfield`, or braced; conds are braced parts after the name slot (empty braces skipped), same 2/1 rule | DisplayOnlyField{name optional} |
| settingSelector | `"Label"[, {vis}]` (vis = first part idx>=1 starting `{`) | SettingSelector{options=[]} |
| settingOption | only valid if LAST component is a SettingSelector (else dropped): `"Option Label", constA=1.5, constB=2`: label=parts[0] unquoted; each later part split at first `=` -> name trim, value parse f64 (non-numeric / no `=` dropped) | appended to selector.options as {label, assignments[]} |
| table | value trimmed | Table{name} |
| commandButton | `"Label", commandName[, {enable}][, clickOnClose|clickOnCloseIfEnabled|clickOnCloseIfDisabled]` (>=2; tokens case-insens) | CommandButton |
| gauge | `gaugeName[, position]` | Gauge{name,position} |
Tests: `dialog = userTable1left, ""` with `field = "Enabled", userTable1Enabled`, `runtimeValue = "X Axis value", userTableXAxis1, { userTable1Enabled }`, `runtimeValue = "Output value", userTableOutput1` -> 3 components; comp[1] RuntimeValue vis "userTable1Enabled". Checkbox: `checkbox = "Use Prediction", use_prediction, { !use_prediction }` -> en "!use_prediction", vis none, flip false; `checkbox = "Don't use Prediction", use_prediction, flipValue, { use_prediction }` -> flip true. SettingSelector: `settingSelector = "Common MAP Sensors", {iat_adcChannel != 0}` with `settingOption = "MPX4115", EngineCylinder=10.6, EngineVolume=121.7` and `"GM 1-BAR", EngineCylinder=10, EngineVolume=105` -> 2 options (assignment names [EngineCylinder, EngineVolume], first value 10.6), vis "iat_adcChannel != 0", following `field` is a separate component.
Dialog struct: `{name,title,layout_hint,topic_help,components[]}`; component is a tagged union as above. IndicatorPanel{name,columns,vis_cond,indicators[]}, Indicator{expression,label_off,label_on,off_fg,off_bg,on_fg,on_bg}, ReadoutPanel{name,columns,vis_cond,readouts[{channel,title,units,digits,precision}]}, HelpTopic{name,title,web_url,text_lines[]}.

### 7.13 [ControllerCommands], [LoggerDefinition], [PortEditor], [FtpBrowser], [DatalogViews], [KeyActions]
- **ControllerCommands** `cmd_name = "raw1" , refOtherCmd, ..., {enable}`: split_ini_line; braced part -> enable_condition; quoted part (starts and ends `"`) -> Raw(strip quotes, escapes left as text e.g. `"Z\x00\x12\x00\x01"`); other non-empty -> Reference(name). If no parts parsed, Raw(parts[0] quotes stripped). label = name. Chains execute in order.
- **LoggerDefinition**: processed twice. (a) legacy per-line: `key = "Label", rate, ch1, ch2..., {cond}` (>=3 parts; rate parse f64 else 100) stored in logger_definitions under the key (note: the real `loggerDef = name, "Label", composite` line also lands here with key "loggerDef"; harmless/noise - a port may skip). (b) Block parser on raw lines (strip everything after FIRST `;` ignoring quotes; split at first `=`): `loggerDef = name, "Label", kind` (naive split) starts a block (default data_read_timeout 5000); then keys (case-insens): `startCommand`,`stopCommand`,`dataReadCommand` (value `"` trimmed; escapes/`\$tsCanId` left as text), `dataReadTimeout` (u64), `continuousRead` (=="true" case-insens), `dataReadyCondition = { cond }` (strip braces, trim), `recordDef = headerLen, footerLen, recordLen` (3 usize; naive split, non-numeric skipped), `recordField = name, "Label", startBit, bitCount, scale, "units"` (naive split; >=5 parts; startBit/bitCount parse u32 else 0; scale f64 else 1; units optional; scale may be a `{expr}` in demo -> falls to 1.0), others (`dataLength`, `calcField`) ignored. Decoding: record bits numbered from the LOW bit of the LAST byte of the record (bit i of field = bit (start+i)%8 of byte `len-1-(start+i)/8`); value = raw*scale; `bitCount` 1..64 else none; field beyond record -> none. `record_count(payload_len) = max(0, payload-header-footer)/record_len` (0 if record_len 0). Tests: tooth `recordDef=0,0,4`, `toothTime 0,32,1.0`: bytes [00 00 04 94] -> 1172, [..04 90] -> 1168, [00 00 09 20] -> 2336; record_count(508)==127; composite recordDef 0,0,5 fields priLevel(0,1), secLevel(1,1), sync(4,1), refTime(8,32,0.001): [00 01 ED 28 19] -> priLevel 1, secLevel 0, sync 1, refTime 126.248; a commented-out `;dataReadCommand` must not be taken; blocks do not bleed (2 loggers; start cmds "H"/"h" and "J"/"j"; read command starts with `T`).
- **PortEditor** `name = "Label"[, {enable}]` (split_ini_line; first braced part idx>=1 = condition). (TS keys like `topicHelp`, `enabledPorts` also land here as junk entries - skip names `topichelp enabledports operators threshold hysteresis`.)
- **FtpBrowser** `name = "Label", server, port[, {cond}]` (>=3; port u16 else 21). OUT OF SCOPE (network) - parse only.
- **DatalogViews** `name = "Label", ch1, ch2,...` (>=2).
- **KeyActions**: `showPanel = keyCombo, panelName` -> {key=combo, action="showPanel:"+panel, label="Show "+panel}; else `keyCombo = action, "Label"[, {cond}]` (>=2). Entries appended.

### 7.14 [ReferenceTables]  (sensor calibration tables)
Section keys: `tableWriteCommand = "w\$tsCanId%2i%2o%2c%v"` (unquote; the MS wire layout: `%2i %2o %2c` are BIG-endian 2-byte fields in this command) -> `def.table_write_command`; `tableBlockingFactor` (usize parse; last assignment wins; honors `#if` arms; e.g. speeduino fixture `#if mcu_stm32 =64 #else =256 #endif #if COMMS_COMPAT =64 #endif`) -> `def.table_blocking_factor`. Blocks: `referenceTable = name, "Label"` (flushes pending limits of the previous block first; creates block with adc_count 0, bytes_per_adc 1, scale 1.0). Block keys (case-insens): `topicHelp` (unquote); `tableIdentifier = 000, "Coolant Temperature Sensor", 001, "Air Temperature Sensor", ...` (split_ini_line, chunks of 2: id u8 (leading zeros ok), label unquoted; chunk without parsable id skipped); `adcCount` usize; `bytesPerAdc` u8; `scale` f64; `solutionsLabel`; `tableGenerator = kind, "Label"[, "xUnits", "yUnits", xLow, xHi, yLow, yHi]` (>=2; bounds = numeric parts from index 4 on, Some only if >=4 numbers; x_units/y_units = parts[2]/[3] unquoted if present); `thermOption = "Name", biasResistor, t1, r1, t2, r2, t3, r3` (need >=8 parts; all 7 numbers must parse; stored points[(t,r)x3]); `solution = "Label", body` (>=2): body `{expr}` -> Expression(trimmed inner) else Generator(name); a solution whose expression is empty (`solution = " ", { }`) is dropped. `tableLimits = id, min, max, default` is recorded in a pending list (even before its identifier) and applied when the block ends (next `referenceTable` or EOF): every identifier with that id gets `limits=(min,max,default)`. Sub-keys before any `referenceTable` are ignored (must not create bogus blocks). `wire_bytes() = adc_count*bytes_per_adc`; `has_identifier(id)`.
Fixture expectations (tests/fixtures/calibration + calibration_ini_parsing.rs): speeduino file has exactly two blocks `std_ms2geno2`, `std_ms2gentherm`; therm: label "Calibrate Thermistor Tables.", adc_count 32, bytes 2, scale 10, wire_bytes 64, identifiers [(0,"Coolant Temperature Sensor",limits(-40,350,180)),(1,"Air Temperature Sensor",(-40,350,70))], topic_help contains "wiki", 12 thermOptions (e.g. Mazda bias 50000 first point (-40,2022088); "VW L-Jet Cylinder Head Temp Sensor II" bias 1100 point0 (-13.888, 11600)); afr block label "Calibrate AFR Table...", adc 1024, 1 byte, scale 10, wire 1024, identifier 2 with limits None, solutions_label "EGO Sensor", >20 solutions whose expressions keep commas and braces; linearGenerator "Custom Linear WB" x_units "Volts", y_units "AFR", bounds (1,4,9.7,18.7); fileBrowseGenerator bounds None; `table_blocking_factor == Some(256)`, 1024/256 = 4 chunks. ms3 fixture: 3 identifiers on one line (000,001,003 "Custom#1 Temperature Sensor"), adc 1024, 2 bytes, wire 2048, limits for 000 chosen by `#if EXPANDED_CLT_TEMP` arm. ms2extra: MAF block id 3, adc 1024, 2 bytes, scale 1.0, solutions >= 6.

### 7.15 [VeAnalyze] / [WueAnalyze] / [GammaE]
Config structs created lazily on first key in the section.
- VeAnalyze: `veAnalyzeMap = veTableName, targetTableName, lambdaChannel, egoCorrectionChannel[, {activeCondition}]` (>=4 parts; 5th optional, default ""; Speeduino has 4); `lambdaTargetTables = a, b, ...`; `filter = ...`; `option = text`.
- WueAnalyze: `wueAnalyzeMap = wueCurve, afrTempCompCurve, targetTable, lambdaCh, coolantCh, wueCh, egoCorrCh` (>=7); `lambdaTargetTables`; `wuePercentOffset = f64 (else 0)`; `filter`; `option`.
- GammaE: `gammaEMap` or `gammaETable = gammaTable, lambdaChannel, targetTable` (>=3); `filter`; `option`.
- filter parse: parts=split_ini_line. One part: if starts `std_` -> filter{name=display=part, channel "", op Equal, default 0, adjustable false} else dropped. >=5 parts: name=[0], display=[1] unquoted, channel=[2], operator=[3] in {`<` LessThan, `>` GreaterThan, `=` Equal, `!=` NotEqual, `&` BitwiseAnd, `|` BitwiseOr} (unknown -> Equal), default=[4] f64 else 0, user_adjustable = parse_ini_bool of the FIRST NON-EMPTY field among parts[5..] (handles 6-field `.., 500, true` and 7-field `.., 71, , true` forms; none -> false). 2-4 parts -> dropped. Tests: `minCltFilter, "Minimum CLT", coolant, <, 71, , true` -> adjustable; `minRPMFilter, "Minimum RPM", RPMValue, <, 500, true`; `accelFilter, "Accel Flag", engine, &, 16, , false` -> false (also 6-field false); values `1`, `yes`, `true ; adjustable` -> true. NB in demo.ini lines like `filter = deltaTps, "dTPS", deltaTps  >  , 50, , true` (missing comma) parse with a mangled channel; treat as given.
- Role inference after parse (outermost only): ve_names=[ve.ve_table_name]; afr_target=[ve.target_table_name]+ve.lambda_target_tables+wue.target_table_name+wue.lambda_target_tables; wue=[wue.wue_curve_name]. For each table: match if name==cand OR map_name==cand OR digit-sibling(cand,name) OR digit-sibling(cand,map_name). Priority: Ve, then AfrTarget, then WarmupEnrichment; else lowercase(name) contains `ign`|`spark`|`advance` -> Ignition; else Other. `split_digit_run(name)`: last digit run -> (prefix, digits, suffix) or none; `digit_sibling(a,b)` = both split, same prefix and suffix, different digits (veTable1Tbl~veTable2Tbl yes; veTableMafTbl no). Tests: veTable1Tbl Ve; afrTable1Tbl AfrTarget; sparkTbl Ignition; veTable2Tbl Ve; afrTable2Tbl AfrTarget; veTableMafTbl Other. Roles: Ve, Ignition, AfrTarget, WarmupEnrichment, Other.

### 7.16 [ConstantsExtensions]
- `defaultValue = const, v1 v2 v3...` (split_ini_line; join parts[1..] with `,`, split on whitespace; FIRST token parsed f64) -> `default_values[const]`. (Array defaults only keep the first element; lines like `defaultValue = wueAfrTargetOffset, -1.5 -1.4 ...` -> -1.5.)
- `maintainConstantValue = const, { expr }`: name = text before first `{` trimmed of trailing `,` and spaces; expr = between first `{` and LAST `}` trimmed -> list.
- `maximumElements = array, cellCount` -> map.
- `requiresPowerCycle = const` -> list.
- (also flags like `noMsqSave`/`controllerPriority` are per-constant trailing tokens, ignored.)

### 7.17 Built-in std panels (mod.rs `std_panel_definition(name)`)
Dialog `panel = std_xxx` or menu `std_xxx` targets not defined in the INI. Synthesized (only for constants that exist; if none exist -> none):
- `std_injection`, title "Injection Setup": fields (const -> label) in order: algorithm "Control Algorithm", reqFuel "Required Fuel", nCylinders "Number of Cylinders", twoStroke "Engine Stroke", engineType "Engine Type", injType "Injector Type", divider "Injector Divider", alternate "Injection Timing", nInjectors "Number of Injectors", injOpen "Injector Open Time".
- `std_ms3Rtc`, title "Real Time Clock": rtc_trim "RTC Trim (ppm)".
- others -> none. Other `std_*` names seen: std_realtime, std_ms2gentherm, std_ms2geno2, std_port_edit, std_separator, std_ms3SdConsole, std_dev, ... (handled by UI / reference-table code).

---------------------------------------------------------------------
## 8. Cross-section helpers

### 8.1 INI capabilities (mod.rs) - derived booleans
has_{constants,output_channels,tables,curves,gauges,frontpage(Some),dialogs,help_topics,setting_groups,pc_variables,default_values,datalog_entries,datalog_views,logger_definitions,controller_commands,port_editors,reference_tables,key_actions,ve_analyze,wue_analyze,gamma_e} = collection non-empty/Some; `supports_console = ecu_type.supports_console && controller_commands non-empty`; `dfu_command_name` = key equal-ignore-case `cmd_dfu`; `openblt_command_name` = `cmd_openblt`; `lua_script_constant` = constant named (ci) `luaScript` with data_type STRING.

### 8.2 pc_variable byte
`resolve_pc_variable_byte(name)`: `default_values[name]` clamped to 0..255 truncated; else `constants[name].min` clamped; else 0. Evaluated at declaration time (constants lookup sees just-inserted entry -> its min) and again in the post-pass (defaults now known). Used by `$var`.

---------------------------------------------------------------------
## 9. Post-passes (order matters)
1. Flush `pending_reference_limits` into the last reference table.
2. `post_process_variable_substitution` (5.5).
3. `post_process_menu_items` (7.11).
4. `post_process_dynamic_shapes`; 5. `post_process_table_sizes`.
6. ECU type (4.2). 7. Roles (7.15, depth 0 only). 8. diagnostic loggers (7.13).
### 9.2 Dynamic shapes
For each constant with `dynamic_size`:
- 2D (`cols_const` set): def_cols = round(default_values[cols_const] or 0), def_rows likewise for rows_const; max_el = maximum_elements[name] else def_cols*def_rows. If def_rows>0 && def_cols>0 && def_rows*def_cols==max_el -> (rows,cols)=(def_rows,def_cols); else if def_cols>0 && max_el>0 && max_el%def_cols==0 -> (max_el/def_cols, def_cols); else if max_el>0 -> (max_el,1); else (max(def_rows,1), max(def_cols,1)). Shape=Array2D{rows,cols}.
- 1D: allocated = round(constants[count_const].max) if >0, else round(default_values[count_const]) else 0; then max(.,1). Shape=Array1D(allocated).
Test: veTableRows/Cols `scalar, U08, .., 1.0, 0, 8, 32, 0`; `veTable = array, U16, 2, [{veTableCols}x{veTableRows}], "%", 0.01, 0, 0, 650, 2`; `veLoadBins = array, U16, 514, [{veTableRows}], ...`; `veRpmBins [{veTableCols}]`; ConstantsExtensions `defaultValue = veTableRows, 16`, `defaultValue = veTableCols, 16`, `maximumElements = veTable, 256` -> veTable 256 elements, veLoadBins/veRpmBins 32 each (the scalar max), table.x_size 16, y_size 16, cols_size_const "veTableCols", rows_size_const "veTableRows", max_elements 256, is_resizable.
### 9.3 Table sizes
For each table: x_size_bins = constants[x_bins].shape.x_size else 0; y_size_bins = constants[y_bins].shape.x_size if y_bins set and found else 1 (2D: 1). `dynamic_defaults` = if constants[map] has dynamic_size with cols_const: (round(default_values[cols_const]), round(default_values[rows_const])) when both >0. final = dynamic_defaults if present; else if constants[map] exists: Array2D -> (cols, rows); Array1D(n) -> (x_size_bins>0 ? x_size_bins : n, 1); Scalar -> (x_size_bins, y_size_bins); else (x_size_bins, y_size_bins). Apply: `if table.x_size==0 && final_x>0 -> set`; `if table.y_size<=1 && final_y>1 -> set`; resize meta (rows_size_const/cols_size_const/max_elements) copied from map constant's dynamic_size.
Tests: `veRpmBins=array,U08,0,[16]`, `veFuelBins=array,U08,16,[16]`, `veTable=array,U08,32,[16x16]`, table with xBins/yBins/zBins -> x 16, y 16. `veTable = array, U08, 0, [8x12]` with only zBins -> x_size 8, y_size 12.
TableDefinition `cell_count = x_size*y_size`, `is_3d = table_type==3D`. Lookup `get_table_by_name_or_map(n)`: name first, then via table_map_to_name. `get_curve_by_name_or_map` same (curve_map_to_name is never populated). `total_memory_size = sum(page_sizes)`.
Structural hash (optional): over signature, n_pages, page sizes, and non-PC constants sorted by name (name, type, page, offset, scale bits, translate bits).

---------------------------------------------------------------------
## 10. Deferred expression fields & dynamic scale resolution
Constants whose scale/translate/min/max is `{expr}` keep the numeric fallback (scale 1, translate 0, min 0, max 255) plus `*_expr`. After a tune is loaded the app calls `resolve_dynamic_scales(values)`: context = values + computed output channels folded 3 passes; for every constant evaluate each `*_expr` and write back scale/translate/min/max; min/max resolved sets `range_resolved=true` when both resolved. `display_to_raw` does NOT clamp while `range_resolved` is false. `constant_feeds_dynamic_scale(name)`: true if `name` appears as an identifier in any scale/translate expression, or (following up to 3 levels of output-channel computed expressions starting from those identifiers) inside a helper channel's expression. Example: load axis scale `{fuelLoadRes}`, channel `fuelLoadRes = { ((algorithm == 0) || (algorithm == 2)) ? 2.000 : 0.500 }` -> algorithm feeds the scale; literal "1.0"/" 0.5 " -> not deferred; `{}` -> not deferred.

---------------------------------------------------------------------
## 11. .inc lookup tables (inc_tables.rs)
Used for sensor linearization files. `lt_inc_parse(text,name)` per line: trim; skip empty; skip lines beginning `;` `#` `'` `:`; cut inline comment at first `;`, trim, skip empty. If `upper(line)` starts with `DB` or `DW` -> DIRECT-INDEX mode: value_part = line[2..] trimmed, trailing `T`/`t` chars removed (`210T`), trim, parse f64; if OK store at next sequential index (index counter only advances on success; vector zero-extended); `continue`. Otherwise key/value: if line contains TAB split on `\t`, else split on whitespace taking first two tokens; need >=2 and both parts (trimmed) parse f64 -> push (key,value). Mode = direct if ANY DB/DW line seen (then key/value pairs are discarded); else key/value pairs sorted ascending by key (stable-ish; NaN order equal).
`lookup(key)`: KeyValue: empty -> none; key<=first.key -> first.value; key>=last.key -> last.value; else binary search partition (first idx with k>=key), interpolate between idx-1 and idx: if |x1-x0|<DBL_EPSILON -> y0 else `y0 + (key-x0)/(x1-x0)*(y1-y0)`. DirectIndex: `idx=(size_t)round(key)` (negative saturates 0); value or none if out of range. `is_direct_index`, `len`, `is_empty`. Name = file stem. Cache: `lt_inc_cache{search_paths, tables by filename}`: `get_or_load(filename)`: cached; for each search path: `path/filename` if exists and loads; else if filename lacks `.inc` (case-insens) try `path/filename.inc`. `add_search_path` dedups. File read uses the same decoder as INI (§1.2). Name used as table name (`file_stem`). Tests (from header): `0.039 19.17 / 0.085 17.24 / 5.0 160` interpolation; `DB 210T ; 0 - sensor failure`, `DB 475T`, `DW 123` -> indices 0,1,2 = 210, 475, 123.

---------------------------------------------------------------------
## 12. Proposed C API
```c
lt_ecu_def *lt_ini_load_file(const char *path, lt_ini_err *err, char *errmsg, size_t n);   /* resolves #include */
lt_ecu_def *lt_ini_load_string(const char *text, size_t len, lt_ini_err *err, char *errmsg, size_t n); /* #include relative to cwd */
void        lt_ini_set_default_symbols(const char *const *syms, size_t n);    /* global seed; applied to every parse */
void        lt_ecu_def_free(lt_ecu_def *);
char       *lt_ini_decode(const unsigned char *b, size_t n, size_t *outlen);  /* §1.2 */
size_t      lt_split_ini_line(const char *v, char ***parts);                  /* §2.4; free with lt_strv_free */
char       *lt_strip_comment(const char *line);
int         lt_ini_substitute(const char *in,size_t n,const lt_u8map *vars,char **out,size_t *outn); /* §5.4 */
size_t      lt_ini_parse_escapes(const char *s, unsigned char **out);          /* §5.3 */
int         lt_dtype_parse(const char *s, lt_dtype *t, int *endian_override);
int         lt_dtype_size(lt_dtype);
int         lt_dtype_read(lt_dtype, const unsigned char *d, size_t len, size_t off, lt_endian, double *out);
void        lt_dtype_write(lt_dtype, unsigned char *d, size_t len, size_t off, double v, lt_endian);
int         lt_parse_shape(const char *s, lt_shape *out, lt_dynsize *dyn);
int         lt_parse_bit_range(const char *s, int *pos, int *size, int *disp);
lt_constant *lt_parse_constant_line(const char *name,const char *v,u8 page,u32 last_off,const char *help);
lt_constant *lt_parse_pcvar_line(const char *name,const char *v,const char *help);
double lt_const_raw_to_display(const lt_constant*,double); double lt_const_display_to_raw(const lt_constant*,double);
lt_outch *lt_parse_outch_line(const char *name,const char *v);  int lt_outch_decode(const lt_outch*,const u8*,size_t,lt_endian,double*);
lt_gauge *lt_parse_gauge_line(const char *name,const char *v);
const lt_table *lt_ecu_table(const lt_ecu_def*,const char *name_or_map);
const lt_curve *lt_ecu_curve(const lt_ecu_def*,const char *name);
lt_dialog *lt_ecu_std_panel(const lt_ecu_def*,const char *name);
lt_ecu_type lt_ecu_detect(const char *sig,const char *filename_hint);
void lt_ecu_infer_table_roles(lt_ecu_def*);
lt_inc_table *lt_inc_parse(const char *text,const char *name); int lt_inc_lookup(const lt_inc_table*,double key,double *out);
```
Memory: arena-allocate the whole definition (one arena per parse) for simple free; all strings NUL-terminated except command byte buffers (carry length).

---------------------------------------------------------------------
## 13. Gotcha checklist for the C port
1. Column-first `[AxB]` arrays; dynamic `[{cols}x{rows}]` too.
2. INI `page` is 1-based; internal 0-based; TableEditor `table = ..., page` is NOT normalized.
3. `lastOffset` = same offset as previous constant (overlay), not previous+size.
4. Case-insensitive sections/keys, case-sensitive names; `[...]` section name `contains` match for tableeditor/curveeditor.
5. `strip_comment` keeps `;` before `=` (help syntax) and inside quotes; whole-line `;` always comment.
6. Dead `#if` arms suppress `#set/#unset/#define/#include`; `#if` only supports a bare symbol; same file included twice is an error.
7. Commands contain NUL after `$tsCanId` substitution; escapes (`\x..`) stay as text for commands/query, but are decoded for `pageIdentifier`.
8. `$var` default = `[Defaults]/ [ConstantsExtensions] defaultValue` else constant's min else 0, re-resolved in a post-pass.
9. Integer writes round (not truncate).
10. Output-channel `bits` fields read the whole backing word (U32 etc.) honoring endianness and sign-extension.
11. Section/key typos kept: `surpressConfigErrorVerbiage`, `tsWriteBlocks`/`writeBlocks`, `pageValueWrite`/`pageChunkWrite`, `nPages`/`numPages`, `pageSize`/`pageSizes`.
12. `queryCommand` under [MegaTune] must also initialize protocol.query_command in a port.
13. Positional `bits` option lists: do not drop empty strings; support `N="label"` sparse form (rusEFI).
14. Conditions are stored as trimmed inner text of `{...}`; trivially `0`/`1` conditions on menu items/panels are dropped.
15. Dialog `panel` brace order = (enable, visibility); `field/checkbox/slider/displayOnlyField/subMenu/groupMenu` = (visibility, enable) when two, single = enable.

---------------------------------------------------------------------
## 14. Exhaustive keyword index (for a lexer test table)
Preprocessor: `#set #unset #if #else #endif #define #include`.
Sections handled: MegaTune, TunerStudio, Constants, OutputChannels, BurstMode, GaugeConfigurations, SettingGroups, PcVariables, Datalog, Defaults, Menu, UserDefined (alias UiDialogs), SettingContextHelp, FrontPage, ControllerCommands, LoggerDefinition, PortEditor, ReferenceTables, FtpBrowser, DatalogViews, KeyActions, VeAnalyze, WueAnalyze, GammaE, ConstantsExtensions, *TableEditor*, *CurveEditor*. Accepted-ignored: File_Header, VerbiageOverride, TurboBaud, Replay, ExtendedReplay, AccelerationWizard, EncodedData, TuningViews, EventTriggers, Tools. Present in real files but unhandled (warned): Tuning, etc.
Constant classes: scalar, array, bits, string. Types: see §6.3. Keyword `lastOffset`.
[MegaTune]/[TunerStudio]: signature, signaturePrefix, queryCommand, versionInfo, iniSpecVersion, pageSize(s), nPages/numPages, endianness, defaultIpAddress, defaultIpPort, delayAfterPortOpen, interWriteDelay, pageActivationDelay(Ms), messageEnvelopeFormat, maxUnusedRuntimeRange, ochGetCommand, ochBlockSize.
[Constants] protocol: page, messageEnvelopeFormat, maxUnusedRuntimeRange, endianness, nPages, pageSize, pageIdentifier, pageReadCommand, pageValueWrite, pageChunkWrite, burnCommand, crc32CheckCommand, retrieveConfigError, pageActivationDelay, blockingFactor, interWriteDelay, blockReadTimeout, delayAfterPortOpen, writeBlocks, tsWriteBlocks, enable2ndByteCanID, tableWriteCommand, tableCrcCommand, tableBlockingFactor, replayConfigTable, replayReadCommand, replayRecordCountParam, noCommReadDelay, refreshLocalStoreOnActivity, defaultRuntimeRecordPerSec, restrictSquirtRelationship, forceBigEndianProtocol, useLegacyFTempUnits, surpressConfigErrorVerbiage, validateArrayBounds, ignoreMissingBitOptions, filterEchoBytes, envelopedScanCommands, pageActivate, defaultIpAddress, defaultIpPort.
UserDefined: help, webHelp, text, topicHelp, dialog, indicatorPanel, readoutPanel, readout, panel, liveGraph, graphLine, runtimeValue, field, checkbox, slider, displayOnlyField, table, indicator, commandButton (clickOnClose, clickOnCloseIfEnabled, clickOnCloseIfDisabled), settingSelector, settingOption, gauge; tokens flipValue, horizontal, vertical, blankfield.
Menu: menu, subMenu, groupMenu, groupChildMenu, std_separator, std_* targets.
TableEditor: table, xBins, yBins, zBins, gridHeight, gridOrient, topicHelp, xyLabels, upDownLabel. CurveEditor: curve, columnLabel, xAxis, yAxis, xBins, yBins, size, topicHelp, gauge, lineLabel.
FrontPage: gaugeN (1..64), indicator. Datalog: entry. Defaults: defaultValue. ConstantsExtensions: defaultValue, maintainConstantValue, maximumElements, requiresPowerCycle. KeyActions: showPanel, `<key> = action,"label"`. Analyze: veAnalyzeMap, wueAnalyzeMap, gammaEMap, gammaETable, lambdaTargetTables, wuePercentOffset, filter, option. ReferenceTables: tableWriteCommand, tableBlockingFactor, referenceTable, topicHelp, tableIdentifier, tableLimits, adcCount, bytesPerAdc, scale, tableGenerator, thermOption, solutionsLabel, solution. LoggerDefinition: loggerDef, startCommand, stopCommand, dataReadCommand, dataReadTimeout, continuousRead, dataReadyCondition, recordDef, recordField (dataLength, calcField ignored).
Not parsed by the reference (present in real INIs, safe to ignore): scatteredOchGetCommand, scatteredOffsetArray, scatteredGetEnabled, gaugeCategory (see 7.3), showTextValues, [Tuning] keys (spotDepth, cursorDepth, gaugeColumns, pageButtons), [Tools] addTool, [EventTriggers] triggeredPageRefresh, `menuDialog`, `[PortEditor]` TS keys, `calcField`.

## 15. Suggested test corpus to port
1. strip_comment table (§2.2). 2. split_ini_line braces/quotes (§2.4). 3. data type names + BU/BS override (§6.3). 4. Shape tests `[16]`,`[16x16]`,`[8X12]` (rows 12 cols 8), `[6x2]`, `[{veTableCols}x{veTableRows}]`, `[{veTableRows}]`, empty -> scalar. 5. Constant-line tests of §6.2 and bit-range tests (§6.3). 6. Raw/display (147 <-> 14.7). 7. Help-text extraction (§6.1 incl. full-INI test with `injectionMode;+"..." = bits, U08, 9, [0:1], "Simultaneous","Sequential","Semi-Sequential"`). 8. Page normalization (pageSize 256,128; page=1->0; page=2->1; page=0->0). 9. lastOffset overlays (§6.1). 10. Preprocessor tests (§3). 11. Conditional headers (§4). 12. Include tests (§3.2). 13. `$tsCanId` tests (§5.4, 5.5). 14. Output channel + multibit tests (§7.1). 15. Gauge tests (§7.3). 16. Menu syntaxes (§7.11). 17. Dialog components (§7.12). 18. Table/curve fidelity keys (§7.9/7.10). 19. Frontpage + cap (§7.8). 20. Datalog columns (§7.7). 21. Analysis filters + roles (§7.15). 22. Setting groups/selector (§7.4, 7.12). 23. Dynamic table sizing (§9). 24. Reference tables on the three fixtures (§7.14). 25. Logger decode (§7.13). 26. .inc lookup (§11). 27. Encoding tests (§1.2). 28. Smoke: parse the 12,570-line `libretune-app/src-tauri/resources/demo.ini` (rusEFI epicECU, endianness little, nPages 3, pageSize 40560,256,2048, messageEnvelopeFormat msEnvelope_1.0, `#if LAMBDA` arms, 100+ item `#define` lists, `bits, U16, 0, [0:6], 0="DEFAULT_FRANKENSO",...`) without error and confirm: signature `rusEFI 2025062409.2025.06.26.epicECU.2089451084`, queryCommand `S` (TunerStudio), pageIdentifier [[0,0],[0,1],[0,2]], pageReadCommand `R%2i%2o%2c` x3, burnCommand [`B%2i`,"",""], crc32CheckCommand `k%2i%2o%2c`, retrieveConfigError `e`, pageActivationDelay 500, writeBlocks on, interWriteDelay 10, blockReadTimeout 3000, blockingFactor 2560, ochGetCommand `O%2o%2c`, ochBlockSize 2268, burst getCommand `A`, defaultIpPort 29001, ecu_type EpicEFI (signature contains "epicecu", which is tested before "rusefi").
