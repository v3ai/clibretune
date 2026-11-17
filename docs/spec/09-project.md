# 09 - Project, Properties, INI Repository, Restore Points, TS Import, Calibration, Pin Conflicts, Action Scripting

Sources: libretune-core/src/{project/{project,properties,repository,version_control}.rs, calibration/mod.rs, pin_conflict.rs, action_scripting.rs}.
Out of scope (ignore): online_repository.rs (network/TLS), git2 version control (replaced by snapshots below), Lua (`ExecuteLuaScript` is parsed/stored/validated only, never executed), uuid crate (write own), chrono/dirs/serde crates.

Conventions: `lt_` prefix; `int` return 0 = ok, negative = error code (`LT_ENOENT`, `LT_EEXIST`, `LT_EINVAL`, `LT_EDATA`, `LT_EIO`). All strings UTF-8 NUL-terminated; dynamic arrays via a small `lt_vec`/`lt_str` helper in the core. Time: UTC, formatted by hand (`time()` + own civil-from-days; no `gmtime_r` assumption).

---
## 1. Project directory layout

```
<ProjectsDir>/<SafeName>/
  project.json            JSON config (pretty, 2-space indent)
  CurrentTune.msq         current tune (MSQ XML, TS compatible; see tune spec)
  datalogs/               log files
  dashboards/             dashboard files
  restorePoints/          created lazily by create_restore_point; <Name>_<YYYY-MM-DD_HH.MM.SS>.msq
  projectCfg/
    definition.ini        copy of ECU definition
    pcVariableValues.msq  PC-variable values (written only if tune has pc variables)
    custom.ini            optional overrides (never created by core)
```
`ProjectsDir` default = `<Documents or $HOME>/LibreTuneProjects`. Portable rule: `$LIBRETUNE_PROJECTS` env if set, else `$HOME/Documents/LibreTuneProjects` if `$HOME/Documents` exists, else `$HOME/LibreTuneProjects`; Windows: `%USERPROFILE%`. Fail ENOENT if no home.
INI repository default dir: `<data_local_dir>/LibreTune/definitions` (Linux `$XDG_DATA_HOME` or `~/.local/share`; mac `~/Library/Application Support`; Windows `%LOCALAPPDATA%`; fallback `$HOME`).

### 1.1 project.json (ProjectConfig)
Keys (snake_case, exact). Reader must tolerate unknown keys and any whitespace; `max_restore_points` optional (default 20); all other keys required (missing -> LT_EDATA), except `port` and `dashboard` which may be `null`/absent.
```
{
  "version": "1.0",
  "name": "<display name, unsanitized>",
  "created": "2026-01-02T03:04:05.123456789+00:00",   RFC3339 UTC
  "modified": "...",
  "ecu_definition": "projectCfg/definition.ini",
  "signature": "<ECU signature>",
  "connection": { "port": null|"COM8", "baud_rate": 0, "timeout_ms": 0 },
  "settings": { "auto_load_tune": true, "auto_save_tune": true, "auto_connect": false, "max_restore_points": 20 },
  "dashboard": null|"dashboards/x.dsh"
}
```
Defaults: `ProjectConfig::default()` has baud_rate 115200, timeout_ms 1000, but **`create()` uses `ConnectionSettings::default()` = port null, baud_rate 0, timeout_ms 0** (gotcha; recommended C: use 115200/1000 in create, since 0 is useless; readers must still accept 0). `modified` is rewritten on every save_config. When writing, emit RFC3339 `YYYY-MM-DDTHH:MM:SSZ` (fractional secs optional); only string-compare is used for sorting so keep format consistent per file; list_projects sorts by `modified` string descending.

```c
typedef struct { char *port; /*NULL=none*/ unsigned baud_rate, timeout_ms; } lt_conn;
typedef struct { int auto_load_tune, auto_save_tune, auto_connect; unsigned max_restore_points; } lt_proj_settings;
typedef struct { char version[16], *name, *created, *modified, *ecu_definition, *signature, *dashboard; lt_conn conn; lt_proj_settings settings; } lt_proj_config;
typedef struct { char *path; lt_proj_config cfg; lt_tune *tune /*NULL*/; int dirty; } lt_project;
typedef struct { char *filename, *path, created[32]; unsigned long size_bytes; } lt_restore_info;
typedef struct { char *name, *path, *signature, *modified; } lt_project_info;
```

### 1.2 Operations
```c
int  lt_project_create(const char *name, const char *ini_src, const char *signature, const char *parent_dir /*NULL=default*/, lt_project *out);
int  lt_project_open(const char *path, lt_project *out);
int  lt_project_import_ts(const char *ts_path, const char *target_dir, lt_project *out);
int  lt_project_save_config(lt_project *p);          /* sets modified=now, writes project.json */
int  lt_project_close(lt_project *p);                /* if settings.auto_save_tune: save_current_tune; free */
int  lt_project_load_tune(lt_project *p);            /* no-op success if CurrentTune.msq missing */
int  lt_project_save_tune(const lt_project *p);
int  lt_project_list(lt_project_info **out, size_t *n); /* newest modified first */
```
**create**: safe_name = name with every char not (alphanumeric | '-' | '_' | ' ') replaced by '_' (per Unicode char in Rust; in C treat each non-ASCII UTF-8 *code point* as alphanumeric if letter - simplest: keep all bytes >=0x80 as-is, replace ASCII non-alnum). path = parent/safe_name. If path exists -> LT_EEXIST ("Project '<name>' already exists"). mkdir -p path, projectCfg, datalogs, dashboards (NOT restorePoints). Copy ini_src byte-for-byte to projectCfg/definition.ini. Config: version "1.0", name = original (unsanitized) name, created=modified=now, ecu_definition "projectCfg/definition.ini", dashboard null. Write project.json.
**open**: read project.json (error if missing/invalid JSON -> LT_EDATA). If auto_load_tune, call load_tune and ignore its error.
**load_tune**: if CurrentTune.msq exists: parse as tune; if parse fails return error. Then if pcVariableValues.msq exists, load PC vars into the tune (ignore errors). **save_tune**: if tune: write CurrentTune.msq; if tune has >=1 pc variable, write pcVariableValues.msq with the project signature (ignore errors of this second write).
**close**: auto_save only when `auto_save_tune`; nothing else (does not write config).
**list**: iterate subdirs of ProjectsDir having a parseable project.json; skip unparseable; return name/path/signature/modified; sort modified desc (string compare). Missing dir -> empty.

### 1.3 TunerStudio import (`lt_project_import_ts`)
1. `<ts>/projectCfg/project.properties` must exist else LT_ENOENT ("Not a valid TS project: project.properties not found").
2. Parse as Properties (sec. 2). `projectName` (default: basename of ts dir; if none "Imported Project"). `ecuConfigFile` (default "mainController.ini"). INI path = `<ts>/projectCfg/<ecuConfigFile>`; missing -> LT_ENOENT.
3. Signature: read INI bytes; if valid UTF-8 use as is else treat each byte as Latin-1 codepoint (note: this importer does NOT strip BOM or use cp1252; repository import does - see 3). `extract_signature`: for each line, trim; if lowercase(line) starts with "signature": find first '='; value = trim after '='; strip all leading/trailing `"` then all `'`... (Rust: `trim_matches('"').trim_matches('\'')`, i.e. strip double quotes first then single quotes); if non-empty return. NOTE: no section check and `; comment` is NOT stripped here (so `signature = "X" ; c` yields `X" ; c`) - improve: take quoted content if a quote pair exists, else cut at `;`. Not found -> signature "".
4. `lt_project_create(projectName, ini_path, signature, target_dir or default dir)` (errors propagate incl. EEXIST).
5. If `commPort` present -> connection.port. If `baudRate` parses as int -> baud_rate. (Other TS props ignored: useCommonDashboardDir, recordsPerSec, vehicle.properties not read.)
6. If `<ts>/CurrentTune.msq` exists: copy to project CurrentTune.msq then load_tune (error propagates).
7. If `<ts>/projectCfg/pcVariableValues.msq` exists: copy to projectCfg/pcVariableValues.msq; if tune loaded, reload.
8. If `<ts>/restorePoints/` exists: mkdir restorePoints, copy every regular file whose extension is exactly `msq` (case-sensitive) preserving name.
9. save_config. Return project.

---
## 2. Java-style `.properties` parser (`project.properties`, `vehicle.properties`)
```c
typedef struct { char **keys, **vals; size_t n; char **hdr; size_t nhdr; } lt_props; /* insertion-ordered, unique keys */
int  lt_props_parse(lt_props *p, const char *text);
int  lt_props_load(lt_props *p, const char *path);   /* bytes: valid UTF-8 -> as is; else each byte -> Latin-1 code point -> UTF-8 */
int  lt_props_save(const lt_props *p, const char *path);
const char *lt_props_get(const lt_props*, const char *key);        /* NULL if absent */
int  lt_props_get_i32(const lt_props*, const char*, int *out);     /* returns 1 if present+parses (strict, whole string, optional sign; NO trimming) */
int  lt_props_get_f64(...);                                        /* strtod, whole string must be consumed */
int  lt_props_get_bool(const lt_props*, const char*, int *out);    /* present -> true iff lowercase in {"true","yes","1"} */
void lt_props_set(lt_props*, const char *k, const char *v);        /* update in place keeps order, else append */
int  lt_props_remove(...); int lt_props_contains(...); size_t lt_props_len(...);
char *lt_props_to_string(const lt_props*);
```
**Parse** (line = split on `\n`, strip trailing `\r`):
- State: pending continuation (key,value-so-far), `in_header=true`.
- If continuation pending: `t = ltrim(line)`; continues = t ends with `\` and not `\\`; part = continues ? t minus last char : t; value += unescape(part); if continues, next line; else set(key,value), clear. (An empty line inside a continuation ends it with nothing appended.)
- Else `t = trim(line)`; empty -> skip; starts with `#` or `!` -> comment (if still in_header, append trimmed line to header_comments; comments after first entry dropped); else in_header=false.
- Split key/value: scan left to right for first `=` or `:` not preceded by an unescaped backslash (`prev_backslash = (c=='\\' && !prev_backslash)`). Key = unescape(rtrim(line[..pos])); value = ltrim(line[pos+1..]) (raw, still escaped). No separator: whole trimmed line is key, value "". (Whitespace-only separator like `a b` is NOT supported: key=`a b`.)
- If raw value ends with `\` and not `\\`: start continuation with value minus last char (value stored raw/un-unescaped at this point - later pieces are unescaped; asymmetry: first piece is NOT unescaped when continuation starts. Replicate or unescape both; harmless).
- Else set(key, unescape(value)).
- EOF with pending continuation: set(key, accumulated).
- Duplicate key: later value replaces, original position kept.
**unescape**: `\t \n \r \f(0x0C) \\ \= \: \<space>` -> char; `\uXXXX` consumes next 4 chars (fewer if EOF), parse hex, if valid code point append UTF-8 (surrogates: skipped, no pairing); any other `\x` -> `x`; trailing lone `\` dropped.
**Serialize**: header comment lines verbatim, then `key=value\n` per entry. escape_key: space->`\ `, `=`->`\=`, `:`->`\:`, `\`->`\\`, tab/nl/cr -> `\t \n \r`. escape_value: `\`->`\\`, tab/nl/cr escaped, a space only if it is the very first char -> `\ `; all else raw (no \u escaping; non-ASCII emitted as UTF-8).
Tests (port): `key1=value1`; `key:value`; `key\ with\ spaces=value with spaces` -> key "key with spaces"; `key=value1\`+NL+`value2\`+NL+`value3` -> `value1value2value3`; `key=Hello` -> Hello; `key=line1\nline2\ttabbed` -> real LF/TAB; TS project file (commPort=COM8, baudRate=115200 get_i32, projectName=MS3-Example_Project, ecuConfigFile=mainController.ini, useCommonDashboardDir=false get_bool=false, recordsPerSec=100); vehicle (weight=3200, firstGearRatio=3.42 f64 exact); roundtrip key1/key2; `limitsettingsGauge\ and\ Settings\ Limits_X=2563` -> key with spaces. Header sample: `#Project Attributes.\n#TunerStudio by EFI Analytics, Inc\n#Last Saved on: ...`.

---
## 3. Local INI repository
Dir contains `index.json` + `<id>.ini` files. index.json = pretty JSON array of entries:
```
[ { "id":"TestECU_1.0", "name":"Test Engine Control Unit", "signature":"TestECU 1.0",
    "path":"TestECU_1.0.ini", "imported":"<RFC3339>", "source":"/orig/path.ini"|null } ]
```
```c
typedef struct { char *id,*name,*signature,*path,*imported,*source; } lt_ini_entry;
typedef struct { char *dir; lt_ini_entry *e; size_t n; } lt_ini_repo;
int  lt_ini_repo_open(lt_ini_repo*, const char *dir /*NULL=default*/); /* mkdir -p; load index (missing=empty; bad JSON=LT_EDATA) */
const lt_ini_entry *lt_ini_repo_list(const lt_ini_repo*, size_t *n);
int  lt_ini_repo_import(lt_ini_repo*, const char *src, char **id_out);
const lt_ini_entry *lt_ini_repo_get(const lt_ini_repo*, const char *id);
char *lt_ini_repo_get_path(const lt_ini_repo*, const char *id);       /* dir/entry.path */
int  lt_ini_repo_remove(lt_ini_repo*, const char *id);                /* delete file if exists, drop entry, save index; unknown id still saves, ok */
int  lt_ini_repo_scan_dir(lt_ini_repo*, const char *dir, char ***ids, size_t *n); /* non-recursive; files with ext ".ini" case-insens; failures skipped w/ warning; missing dir -> empty */
```
**import**: decode bytes: strip UTF-8 BOM (EF BB BF); valid UTF-8 -> as is; else Windows-1252 -> UTF-8 (bytes 0x80-0x9F map via cp1252 table: 80 20AC,82 201A,83 0192,84 201E,85 2026,86 2020,87 2021,88 02C6,89 2030,8A 0160,8B 2039,8C 0152,8E 017D,91 2018,92 2019,93 201C,94 201D,95 2022,96 2013,97 2014,98 02DC,99 2122,9A 0161,9B 203A,9C 0153,9E 017E,9F 0178; 81,8D,8F,90,9D -> same-numbered code point; A0-FF -> Latin-1). Extract info (below). id = generate_id(signature): each char kept if alphanumeric or `-_.`, else `_`. If id already in index return it (nothing written). Else write the *decoded UTF-8 text* (not original bytes) to `<id>.ini`, append entry (imported=now, source=src path), save index.
**extract_ini_info**: per line (trim): if starts with `[`: header = text before first `;`, trimmed, strip `[`/`]` chars from both ends; in_section = header equals (case-insens) `MegaTune` or `TunerStudio`; continue. If not in_section skip. If line contains `=`: key = trim(before first '='), val = after. If signature still empty and key==`signature` (case-insens): signature = extract_quoted(val); else if name empty and key==`nEmu` (ci): name = extract_quoted(val). Signature empty -> LT_EDATA "Could not find signature in [MegaTune] or [TunerStudio] section of INI file". name empty -> name = signature. **extract_quoted(v)**: trim; if a `"` exists and a later closing `"` exists return text between; else cut at first `;`, trim, strip surrounding `"`.
Tests: `[MegaTune]\n  MTversion = 2.25\n[TunerStudio]\n  queryCommand = "S"\n  signature= "rusEFI master.2026.02.20.uaefi.4211171972" ; comment\n` -> sig `rusEFI master.2026.02.20.uaefi.4211171972`, name==sig. Import `[MegaTune]\nsignature = "TestECU 1.0"\nnEmu = "Test Engine Control Unit"` -> id `TestECU_1.0`, name "Test Engine Control Unit".

---
## 4. Restore points (existing, TS-compatible) 
Files `restorePoints/<SafeName>_<YYYY-MM-DD_HH.MM.SS>.msq`, SafeName = config.name keeping only alphanumeric, `-`, `_` (spaces dropped; may be empty -> `_2026...`). Timestamp = current UTC. Two restores in the same second overwrite each other (add `-N` suffix guard in C if file exists: not in Rust).
```c
int lt_restore_create(const lt_project*, char **path_out);         /* LT_ENOENT if no tune loaded; mkdir -p restorePoints; tune save */
int lt_restore_list(const lt_project*, lt_restore_info **out, size_t *n); /* *.msq only (ext "msq" exact), newest first (strcmp on created desc); missing dir -> empty */
int lt_restore_path(const lt_project*, const char *filename, char **path_out); /* validated, see below */
int lt_restore_load(lt_project*, const char *filename);            /* tune = load(path); dirty=1 */
int lt_restore_delete(const lt_project*, const char *filename);
int lt_restore_prune(const lt_project*, size_t keep, size_t *deleted); /* list; delete all after first `keep`; count successes */
```
**Timestamp parse** (`created` field): strip `.msq`; `p = rfind('_')`; `date_part = s[..p]`; `q = rfind('_', in date_part)`; ts = s[q+1..] (must be len>=19; exactly `YYYY-MM-DD_HH.MM.SS`); date = ts[0..10] (digits with '-' at idx 4,7); time = ts[11..] with '.'->':' (len 8, ':' at 2,5, digits elsewhere); result `"<date>T<time>Z"`. Anything else -> not parseable -> fallback = file mtime as RFC3339 UTC, or `1970-01-01T00:00:00Z` if unavailable (NEVER "now": unparseable names must sort oldest-ish and be stable, so prune doesn't keep junk and delete real backups). NB: comparing mtime strings with `...T..Z` strings; use the same `YYYY-MM-DDTHH:MM:SSZ` format everywhere so string compare is valid.
**Path security (mandatory)**: `filename` must be a single plain component: reject (LT_EINVAL) if empty, `.`, `..`, contains `/` or `\`, or is absolute (drive letter `X:` too). Then path = restoreDir/filename must be a regular file else LT_ENOENT. Then resolve real paths (`realpath`/`_fullpath`) of file and dir; parent of file must equal dir (blocks symlink escape) else LT_EINVAL. Tests: reject `<abs path>`, `../CurrentTune.msq`, `..\CurrentTune.msq`, `sub/x.msq`, `sub\x.msq`, `..`, `.`, `""` all as EINVAL and no file touched; `missing.msq` -> ENOENT; valid name deletes only that file.
Tests for timestamp: `NA6_SPEEDUINO_2026-08-11_08.47.31.msq` -> `2026-08-11T08:47:31Z`; `My_Turbo_Build_2025-01-02_03.04.05.msq` -> `2025-01-02T03:04:05Z`; `backup_before_turbo.msq` -> none; `proj_xxxxxxxxxx_yy.zz.ww.msq` -> none; unparseable mtime fallback identical across two calls.
`max_restore_points` (default 20, 0=unlimited) is stored; the core does not auto-call prune - UI should call `lt_restore_prune(p, max)` after create when max>0.

---
## 5. Snapshot-directory replacement for git version control (NEW design; no git)
Rust semantics being replaced: `commit(message)` stages everything except ignores (`*.tmp`, `*.bak`, `datalogs/`, `.cache/`), returns 7-char sha; history (newest first, max_count) with `CommitInfo{sha_short, sha, message, annotation, author, timestamp "YYYY-MM-DD HH:MM:SS", is_head}`; `has_changes`; diff between two commits (list of changed constants/tune values + files_changed); checkout (refuses if dirty; restores exact bytes, autocrlf off); branches. Annotation convention: message + `"\n\n" + "LT-Note: " + note` when trimmed note non-empty (else message unchanged); extraction: first line whose trimmed text starts with `LT-Note:` -> rest trimmed, None if empty. `TuneChange{name, old_value?, new_value?, change_type}`; `CommitDiff{from_sha,to_sha,changes,files_changed}`. Branches: DROP (not supported; document as out of scope).

Replacement layout:
```
<project>/snapshots/
  index.txt                      line-oriented, newest LAST: "<id>\t<unix_secs>\t<author>\t<msg-escaped>\n"  (id = 8 hex lowercase = FNV-1a/CRC32 of (content hashes + time))
  <id>/CurrentTune.msq
  <id>/project.json
  <id>/projectCfg/definition.ini
  <id>/projectCfg/pcVariableValues.msq   (if present)
  <id>/note.txt                  optional annotation (the LT-Note body)
```
Included set = CurrentTune.msq, project.json, projectCfg/** , dashboards/**. Excluded: datalogs/, restorePoints/, snapshots/, *.tmp, *.bak, .cache/, hidden files. msg escaping in index: `\\`, `\t`, `\n`.
```c
int  lt_snap_commit(const char *proj, const char *message, const char *annotation, const char *author, char id_out[9]);
int  lt_snap_has_changes(const char *proj);      /* compare included files byte-wise vs newest snapshot; no snapshot => 1 if any file */
int  lt_snap_history(const char *proj, size_t max, lt_snap_info **out, size_t *n); /* newest first, is_head on first */
int  lt_snap_diff(const char *proj, const char *from, const char *to /*NULL=working copy*/, lt_snap_diff_t *out); /* files_changed = relative paths whose bytes differ (or added/removed: change_type "added"/"removed"/"modified"); constants-level changes come from MSQ page/constant parse (tune spec) comparing name->value text */
int  lt_snap_restore(const char *proj, const char *id, int force); /* if !force and has_changes -> LT_EBUSY; copies files back byte-exact (binary mode, no EOL conversion); removes included files absent in snapshot */
int  lt_snap_prune(const char *proj, size_t keep);
```
Unique id: if collision, rehash with counter. Copy files in binary mode ("rb"/"wb") - critical for Windows. Test cases to port (from Rust tests): init+commit then has_changes false; modify a file then has_changes true; restore returns exact bytes (incl. CRLF content) and can be repeated; history lists commit messages newest first; format_commit_message("msg", Some("  note ")) == "msg\n\nLT-Note: note", with None/"  " -> "msg".

---
## 6. Calibration (sensor curves)
Constants: `ADC_COUNTS=1024`, `ADC_REFERENCE_VOLTS=5.0`. `adc_to_volts(adc) = adc*5.0/1023.0` (0->0, 1023->5.0 exactly).
Errors: `LT_CAL_PARSE_FAILED, LT_CAL_EVAL_FAILED, LT_CAL_NOT_AN_EXPRESSION(generator name), LT_CAL_MISSING_INCLUDE(file name), LT_CAL_DEGENERATE(msg)`. Messages: "calibration formula parse failed: %s", "calibration formula failed: %s", "this preset uses the '%s' generator and has no formula to evaluate", "preset needs the lookup file '%s', which is not available".
INI types consumed (defined in INI spec): `ReferenceTable{name,label,topic_help,identifiers[{id u8,label,limits?(min,max,default)}],adc_count,bytes_per_adc,scale,generators,therm_options[{name,bias_resistor,points[3]{tempC,ohms}}],solutions_label,solutions[(label, Expression{expression}|Generator{generator})]}`; wire_bytes = adc_count*bytes_per_adc.

```c
int lt_cal_evaluate_solution(const char *expr, lt_inc_cache *cache /*may be empty*/, double out[1024]);
int lt_cal_solution_curve(const lt_reference_table*, const char *label, lt_inc_cache*, double out[1024]); /* label match case-insens; unknown label -> LT_ENOENT (Rust: None); generator -> NOT_AN_EXPRESSION */
```
**evaluate_solution**: (1) find first `table(` in expression; if found, take the first `"..."` after it as filename; load via inc cache (search paths; try name as-is, and with `.inc` appended if not already ending in `.inc` case-insens) - failure -> MISSING_INCLUDE(name) (NEVER evaluate to 0: all-zero AFR table is dangerous). (2) parse expression with the INI expression engine (spec 02/evaluator); error -> PARSE_FAILED("<err> (in `<expr>`)"). (3) for adc 0..1023: set variable `adcValue`=adc, evaluate; error -> EVAL_FAILED("<err> (at adcValue=<adc>)"); out[adc]=value as f64. If table() used, the evaluator's `table(volts, "file")` callback = inc_table.lookup(volts) (ignoring filename).
Tests: `"10.0001 + ( adcValue * 0.0097752 )"` -> out[0]≈10.0001 (1e-6), out[512]*10 ≈150 (±1), bytes `round(a*10)` as u8 monotone non-decreasing, bytes[0]>=100, bytes[1023]<=200. `table(adcValue*5/1023 , "nb.inc")` with empty cache -> MISSING_INCLUDE("nb.inc").

**AfrCorrection** `{gain, offset, npoints, pts[2]{measured,expected}}`:
`int lt_afr_corr_from_points(lt_afr_corr*, const double (*pts)[2], int n)`:
- n==1: gain=1, offset=expected-measured.
- n==2: if |m1-m2|<1e-6 -> DEGENERATE "the two measured readings must differ (both were %.2f AFR) - a gain cannot be solved from one point". gain=(e2-e1)/(m2-m1); if gain<=0 -> DEGENERATE "these points give a gain of %.3f, which would reverse the curve - check the expected values are paired with the right measured readings"; offset=e1-gain*m1.
- else DEGENERATE "a correction takes one or two points, got %d".
`apply(curve,n)`: v = gain*v+offset. `describe`: 1 pt: `corrected %+.2f AFR against a reference (%.2f read as %.2f)` (offset,m,e); else `corrected against a reference (gain %.4f, offset %+.2f)`.
Tests: (14.2,14.7): gain 1, offset 0.5; curve {10,14.2,19.5} -> {10.5,14.7,20.0}. (12.5,12.08),(19.0,18.25): both map exactly (1e-9), 14.7 maps into (14.0,14.7). (14.7,14.5),(14.7,15.0) -> error text contains "must differ". (12,18),(18,12) -> contains "reverse". 0 or 3 points -> error.

**LinearWideband** `{p1:(v,afr), p2:(v,afr)}`: new() fails DEGENERATE if |v1-v2|<1e-9 ("the two calibration points must be at different voltages (both were %.3f V)"). slope=(afr2-afr1)/(v2-v1); intercept=afr1-slope*v1; afr_at_volts(v)=intercept+slope*v; curve[adc]=afr_at_volts(adc_to_volts(adc)); describe = `%.2f–%.2f AFR over 0–%.0f V (%.4f AFR/V)` with afr at 0V, afr at 5V, 5, slope (en-dash U+2013 UTF-8; fine to use ASCII '-' in C but keep consistent). Tests: ((0,9.69),(5,19.80)): afr_at(0)=9.69, afr_at(5)=19.80, curve[0]=9.69, curve[1023]=19.80 (1e-9); ((2.5,14.7),(2.5,15.0)) -> DEGENERATE.

**ThermistorFit** `{a,b,c,bias_resistor}` (Steinhart-Hart, 1/T = A + B*ln R + C*(ln R)^3, T kelvin):
`from_points(bias, pts[3]{tempC,ohms})`: each ohms<=0 -> DEGENERATE "thermistor resistance must be positive (point %d was %g ohm)". l[i]=ln(R_i); y[i]=1/(T_i+273.15). g2=(y1-y0)/(l1-l0); g3=(y2-y0)/(l2-l0); denom=(l2-l1)*(l0+l1+l2); if !finite or |denom|<1e-12 -> DEGENERATE "the three thermistor points are collinear in log-resistance and cannot be fitted"; c=(g3-g2)/denom; b=g2-c*(l0*l0+l0*l1+l1*l1); a=y0-(b+l0*l0*c)*l0. `from_option` uses option.bias_resistor/points.
`temperature_at_resistance(R)`: lnR=ln(max(R,1e-6)); invT=a+b*lnR+c*lnR^3; return 1/invT-273.15.
`resistance_at_adc(adc)`: ratio=clamp(adc_to_volts(adc)/5.0, 1e-6, 1-1e-6); return bias*ratio/(1-ratio) (divider Vout=5*Rtherm/(Rbias+Rtherm)).
`temperature_at_adc(adc)=temperature_at_resistance(resistance_at_adc(adc))`; `sample_at_bins(bins[32] u16)` -> double[32].
Tests: bias 50000, points (-40,2022088),(21,68273),(99,3715) reproduce each point within 0.01 C. bias 2490 points (-40,100700),(30,2238),(99,177): temperature_at_adc(100) > temperature_at_adc(900). INI test fixtures (calibration_ini_parsing.rs, Speeduino INI): blocks `std_ms2geno2`,`std_ms2gentherm`; therm: label "Calibrate Thermistor Tables.", adc_count 32, bytes_per_adc 2, scale 10, wire_bytes 64, ids 0 "Coolant Temperature Sensor" limits (-40,350,180), 1 "Air Temperature Sensor" limits (-40,350,70), 12 therm_options (Mazda bias 50000; VW bias 1100, first point (-13.888,11600)); o2: label "Calibrate AFR Table...", adc 1024, bytes 1, scale 10, wire 1024, id 2 no limits, solutions_label "EGO Sensor", >20 solutions; linearGenerator "Custom Linear WB" units Volts/AFR bounds (1,4,9.7,18.7). Wire encoding (scale, byte packing) belongs to protocol spec, not here.

---
## 7. Pin conflict detection
Inputs from ECU definition: `constants` map name->Constant {data_type, bit_options[] (strings, index = raw option value)}. Iteration order of the map is irrelevant (results sorted). Callback: `int value_index(void *ctx, const char *name, const lt_constant *c, size_t *idx)` returns 0 if value unknown (constant skipped).
```c
typedef struct { char *pin_label; char **constants; size_t n; } lt_pin_conflict;   /* constants sorted ascending (bytewise) */
typedef struct { lt_pin_conflict *c; size_t n; } lt_pin_report;                     /* c sorted by pin_label bytewise */
int lt_pin_is_assignment(const lt_constant*);
int lt_pin_is_unassigned_label(const char*);
int lt_pin_detect(const lt_ecudef*, lt_pin_value_fn, void *ctx, lt_pin_report *out);
int lt_pin_conflict_if_assigning(const lt_ecudef*, const char *name, size_t new_index, lt_pin_value_fn, void*, lt_pin_conflict *out); /* 1 conflict, 0 none */
char *lt_pin_summary(const lt_pin_report*);   /* "" if none */
```
**is_pin_assignment(c)**: false unless data_type==Bits and bit_options.len>=3. lname=ascii-lowercase(name); false if lname contains `pinmode`, or ends with `mode`, or contains `invert`. name_pin = lname contains `pin`; has_none = any option is_unassigned; pinish = any option looks_like_pin_label. Result = (name_pin && has_none) || (has_none && pinish && len>=8).
**is_unassigned(label)**: t=trim; empty -> true; lowercase t in {`none`,`invalid`,`off`,`disabled`,`not used`,`unused`,`n/a`,`na`,`default`,`board default`}. ("Board Default" must NOT conflict: Speeduino outputs share it.)
**looks_like_pin_label(l)**: t=trim; len<2 -> false; STM32 style: t[0],t[1] ASCII letters and t[2..] non-empty all ASCII digits (PA0, PD13); or lowercase contains any of `output`,`injector`,`ignition`,`digital input`,`low side`,`high side`.
**detect**: for every assignment constant with known index whose option at idx exists and is not unassigned: group by exact label (case-sensitive, untrimmed) -> list of names. Keep groups with >1 names; sort names, sort groups by label.
**conflict_if_assigning(name,new_index)**: constant must exist and be assignment; label=options[new_index] (out of range -> none); unassigned -> none; others = all other assignment constants whose current option label == label exactly; empty -> none; else constants = [name] + sorted(others) (the assigning constant FIRST, rest sorted).
**summary** (lines joined by "\n", em-dash U+2014 and bullet U+2022 in UTF-8):
```
Pin assignment conflict(s) detected — the same pin is used by multiple outputs:
• Pin '<label>' used by: <a>, <b>
Clear or reassign one of the functions before burning (or confirm to burn anyway).
```
Tests: opts ["NONE","INVALID","PA0","PD13","PE1"], constants fuelPumpPin=3, gppwm1_pin=3, fanPin=2 -> one conflict PD13 [fuelPumpPin,gppwm1_pin]. Constant `fuelPumpPinMode` with options ["default","default inverted","open collector"] ignored; NONE values ignored -> no conflict. waterPumpPin assign idx2 ("PD13") while gppwm1_pin=2 -> conflict PD13 containing gppwm1_pin. Board Default opts ["Board Default","1","3","30"] x3 all idx0 -> no conflict; assigning idx0 -> none. Assigning NONE (idx0) when other holds PD13 -> none. (Test constants: Bits, bit_position 0, bit_size 8.)

---
## 8. Action scripting (record/replay)
### 8.1 JSON format (serde adjacently tagged)
ActionSet file = pretty JSON (2-space indent), `serialize_action_set`:
```
{ "id":"<uuid v4 lowercase 8-4-4-4-12>", "name":"", "description":"", "version":"1.0",
  "actions":[ {"type":"<Variant>","data":{...}}, ... ],
  "metadata":{ "created_by":"", "created_at":"RFC3339", "modified_at":"RFC3339", "tags":[], "compatible_ecus":[] } }
```
Action variants (`type` -> `data` fields):
- `TableEdit`: `table_name` str, `x_index` u16, `y_index` u16, `new_value` f64, `old_value` f64|null (x=column, y=row).
- `ConstantChange`: `constant_name`, `new_value`, `old_value`|null.
- `BulkOperation`: `operation` str ("scale","smooth","interpolate","set_equal","rebin"), `table_name`, `cells` array of `[x,y]` u16 pairs, `parameters` object str->f64 (key order unspecified), `old_values` array f64|null.
- `ExecuteLuaScript`: `script`, `description` (stored only; never executed - Lua OUT OF SCOPE).
- `Pause`: `duration_ms` u32.
- `SendCommand`: `command` str.
Deserialize: all fields required except `old_value`/`old_values` (null or absent -> none, serde Option); unknown `type` or malformed -> error "Failed to deserialize action set: <msg>". NaN/Inf cannot be written in JSON (Rust serde_json errors "Failed to serialize action set: ..."); C writer should return LT_EDATA on non-finite numbers. Doubles: print shortest round-trip (`%.17g` fallback; ensure ".0" optional). Need only a minimal JSON reader/writer (shared with project.json, index.json).
```c
typedef enum { LT_ACT_TABLE_EDIT, LT_ACT_CONSTANT_CHANGE, LT_ACT_BULK, LT_ACT_LUA, LT_ACT_PAUSE, LT_ACT_SEND_CMD } lt_act_type;
typedef struct { lt_act_type type; char *table_name, *constant_name, *operation, *script, *description, *command;
  unsigned short x_index, y_index; double new_value, old_value; int has_old; unsigned duration_ms;
  struct { unsigned short x,y; } *cells; size_t ncells; char **param_keys; double *param_vals; size_t nparams;
  double *old_values; size_t nold; int has_old_values; } lt_action;
typedef struct { char *created_by,*created_at,*modified_at,**tags,**compatible_ecus; size_t ntags,necus; } lt_action_meta;
typedef struct { char *id,*name,*description,*version; lt_action *actions; size_t n; lt_action_meta meta; } lt_action_set;
typedef struct { lt_action_set *cur; } lt_recorder;  /* cur NULL = idle */
char *lt_recorder_start(lt_recorder*, const char *name, const char *desc, const char *created_by); /* returns new id; replaces any existing recording; version "1.0"; created_at=modified_at=now; empty tags/ecus */
lt_action_set *lt_recorder_stop(lt_recorder*);              /* returns ownership, NULL if none */
int  lt_recorder_record(lt_recorder*, const lt_action*);    /* LT_ERR "No recording in progress" if idle; appends (does NOT touch modified_at) */
const lt_action_set *lt_recorder_current(const lt_recorder*);
int  lt_recorder_is_recording(const lt_recorder*);
int  lt_recorder_add_tag(lt_recorder*, const char*);        /* appends, no dedupe */
int  lt_recorder_set_compatible_ecus(lt_recorder*, char **, size_t); /* replaces */
char *lt_action_set_serialize(const lt_action_set*, char *err, size_t errlen);
int  lt_action_set_deserialize(const char *json, lt_action_set*, char *err, size_t errlen);
```
Error string for idle recorder exactly `No recording in progress`. UUID v4: 16 random bytes (seed from `/dev/urandom` if present else time+pid+clock mix), byte6=(b&0x0F)|0x40, byte8=(b&0x3F)|0x80.

### 8.2 Validation (`lt_action_validate(set, def_or_NULL, warnings, errors)`)
Returns Ok(warnings[]) if errors empty else Err(errors[]) (warnings dropped). Iterate with idx (0-based); messages exact:
- TableEdit: empty table_name -> `Action {i}: Empty table name`; new_value NaN/Inf -> `Action {i}: Invalid value {v}`. With def: table = `get_table_by_name_or_map(name)` (by table name or its map name); none -> `Action {i}: Table '{t}' not found in ECU definition`; else x_index >= table.x_size -> `Action {i}: x_index {x} out of bounds for table '{t}' (width {x_size})`; y_index >= y_size -> `... y_index {y} out of bounds for table '{t}' (height {y_size})`.
- ConstantChange: empty name -> `Action {i}: Empty constant name`; non-finite -> `Action {i}: Invalid value {v}`. With def: constant missing -> `Action {i}: Constant '{n}' not found in ECU definition`; else (a) v < c.min -> `Action {i}: Value {v} for constant '{n}' below minimum {min} ({units})`; v > c.max -> `... above maximum {max} ({units})`; (b) if data_type raw range exists (U08/Bits 0..255, S08 -128..127, U16 0..65535, S16 -32768..32767, U32 0..4294967295, S32 -2147483648..2147483647; F32/F64/String none) and |scale|>DBL_MIN: raw=(v-translate)/scale; if raw<lo||raw>hi -> `Action {i}: Value {v} ({units}) for constant '{n}' outside storable range [raw {lo:.0}..{hi:.0}] for {DataType:?}`; (c) if bit_options non-empty and v not exactly equal to one of 0..len-1 (as doubles) -> `Action {i}: Value {v} for bits constant '{n}' not one of {len} ({options list})`. Note: NaN also yields min/max comparisons false; checks (a)(b)(c) run even when v non-finite (extra errors) - fine.
- BulkOperation: empty table_name -> `Action {i}: Empty table name`; cells empty -> WARNING `Action {i}: No cells selected for {operation}`. With def: table missing -> not-found error (same text); else for each (cx,cy): cx>=x_size -> `Action {i}: cell x={cx} out of bounds for table '{t}' (width {x_size})`; cy>=y_size -> `... cell y={cy} out of bounds ... (height {y_size})`. Then regardless of def: operation=="scale" and no "factor" param -> `Action {i}: scale missing 'factor' parameter`; operation=="rebin" and (no "x_bins" or no "y_bins") -> `Action {i}: rebin missing bin parameters`. Other operations unchecked.
- ExecuteLuaScript: empty script -> `Action {i}: Empty Lua script`.
- Pause: duration_ms==0 -> WARNING `Action {i}: Zero-duration pause`.
- SendCommand: empty -> `Action {i}: Empty command`; else with def and command not a key of `controller_commands` -> `Action {i}: Command '{c}' not supported by ECU definition`.
Number formatting `{v}` = Rust f64 Display (shortest round-trip, no exponent for typical values, `NaN`, `inf`); use `%.15g`-style trimmed.
### 8.3 Summary (`lt_action_summarize`)
`"Action Set: {name}\nActions: {n}\n"` then one line per type present `"  - {TypeName}: {count}\n"` (TypeName in {TableEdit, ConstantChange, BulkOperation, ExecuteLuaScript, Pause, SendCommand}); order unspecified in Rust (hash order) - use declaration order above in C.
### 8.4 Replay
Rust has NO executor (validation only; "actual execution requires ECU layer"). C replay design: for each action in order: TableEdit/ConstantChange/BulkOperation apply to the in-memory tune via the table-ops/tune APIs; Pause sleeps duration_ms; SendCommand sends controller command; Lua -> skip with warning. Always run validate first and refuse on errors.
### 8.5 Tests to port
recorder start/stop (is_recording toggles; id non-empty); record ConstantChange{AFR_Target,14.5,Some 14.0} -> current().actions.len()==1; record without start -> error `No recording in progress`; validate empty set with no def -> Ok; ConstantChange new_value NaN -> Err; serialize/deserialize roundtrip preserves name, description, actions.len, tags.len, compatible_ecus; add_tag x2 -> tags.len 2; set_compatible_ecus ["Speeduino","rusEFI"] -> len 2; summarize contains name and counts; scale op with `factor`=1.05 on cells [(0,0),(1,1),(2,2)] validates Ok (no def); rebin without bins -> Err; 4-action sequence recorded in order; TableEdit example veTable1 x=5 y=3 new 75.5 old 74.2; BulkOperation old_values [75,76,77].
