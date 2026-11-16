# 06 - Datalog (recorder, playback, MLG, LTLOG, CSV, dyno, math channels)

Source: `libretune-core/src/datalog/{mod,format,mlg,ltlog,recorder,playback,dyno}.rs`, `project/math_channels.rs`.
Out of scope: serde/JSON derive machinery, tracing, chrono, tempfile (use libc `time`, `fopen`). zstd is OUT-OF-SCOPE-AS-DEPENDENCY: see 3.9 for the C89 decision.

IMPORTANT FACTS ABOUT THE REFERENCE (no surprises):
- MLG is **read-only** in the reference (version 2 only; no v1, no writer, no info-text parse). A writer is optional; the layout below is symmetric and the Rust test helpers (`build_log`) are the writer recipe.
- The recorder has **no markers, no channel selection** (channel list fixed at construction) and **no CSV streaming**. It records only to `.ltlog`. Markers exist only as MLG kind-1 records which the reader skips.
- No HP-from-torque / RPM calc exists in dyno.rs; HP/torque are imported columns only.
- Numbers: all timestamps/values are `double`; time stored as microseconds (`unsigned long long` emulation needed in C89: use two `unsigned long` or `double` seconds; recommend `double` seconds + `long` micros where exactness matters; MLG tests need exact 10us ticks, which are exactly representable as integer micros < 2^53).

## 1. Common types

```c
typedef struct { double t;        /* seconds from start of log (>=0) */
                 double *v; }     /* n_channels values, order of channel list */
        lt_log_entry;
typedef struct { char **names; int n_ch; lt_log_entry *e; long n; } lt_log; /* in-memory log */
typedef enum { LT_FMT_CSV, LT_FMT_MLG, LT_FMT_LTLOG, LT_FMT_NONE } lt_log_format;
lt_log_format lt_log_format_from_ext(const char *path); /* case-insensitive ext: csv|mlg|ltlog else NONE */
const char *lt_log_format_ext(lt_log_format f);          /* "csv","mlg","ltlog" */
int lt_log_read(const char *path, lt_log *out, char *err, size_t errlen); /* dispatch by extension */
```
`lt_log_read`: ext NONE -> error "<path> is not a log format LibreTune reads" (InvalidInput; test checks substring "not a log format", and content is NOT sniffed: CSV text in `log.msl` rejected). `.mlg` with CSV text -> error containing "signature" (bad file signature).
`lt_log_visit(path, callback)`: ltlog streams block-by-block; csv/mlg load fully then iterate. Returns channel names. Callback error aborts.

## 2. CSV log format

### Writer (`write_csv`)
Line 1: `Time` then `,<channel>` for each channel, `\n`. Each row: timestamp seconds `%.3f`, then `,%.4f` per value, `\n`. (Rust `{:.3}` / `{:.4}`; locale-independent: force '.' decimal in C.) No quoting, no escaping of channel names. Note the streaming recorder no longer writes CSV; this is the export/legacy form.

### Reader (`read_csv`), tolerant
1. Read whole file. Strip UTF-8 BOM (EF BB BF) if first bytes.
2. Split into lines (`\n`, strip trailing `\r`: Rust `lines()` does).
3. No first line -> error "log file has no header row".
4. Header split on `,`. First field trimmed; lowercase must START WITH "time" else error `unexpected first column "<x>"; expected 'Time'`. If lowercase first field CONTAINS "ms" (e.g. "Time (ms)") timestamps are divided by 1000.
5. Remaining fields: trimmed channel names; drop trailing empty names (trailing comma). If none -> error "log header has no channel columns".
6. Each later line: trim whole line; empty -> skip (not counted). Split on `,`; first field trimmed parsed as double, else skipped++. Apply ms/1000. Remaining fields each trimmed and parsed as double (strict full-string parse; "abc" fails the row); row accepted only if value count == channel count, else skipped++. Timestamp clamped `max(0.0)`.
7. Zero accepted rows -> error "log has no parsable data rows (N skipped)".
Parse note: Rust `f64::parse` accepts "inf","nan","1e3","+5",".5","5."; reject empty and trailing junk. Use strtod and require end==string end after trim.
Tests: `"﻿Time,rpm,map\n0.0,900,50\n\nnot-a-number,1,2\n1.0,1000\n1.5,1100,60\n"` -> channels [rpm,map], 2 entries, entries[1].values=[1100,60]. Empty file -> error; `"Time,rpm\n"` -> error. Round trip of rpm/map with 0.0/0.5s exact. `"Time,rpm\n0.0,900\n"` via read_log -> ch [rpm], values [900].

## 3. LTLOG (native binary, little-endian) - version 1

```
header:
  0  "LTLg"             4 bytes magic
  4  u16 version=1      (other -> error "unsupported ltlog version N")
  6  u16 flags=0        (ignored on read)
  8  u32 schema_len     (> 16*1024*1024 -> error "implausibly large")
 12  schema JSON UTF-8, schema_len bytes
 ..  u32 crc32(schema JSON)   mismatch -> error "ltlog header CRC mismatch"
block* (repeated):
  "LTBK"                4
  u32 raw_len           uncompressed payload size
  u32 zstd_len          compressed size
  u32 crc32(compressed bytes)
  compressed payload    zstd_len bytes (level 3 on write)
  raw payload:
     u16 n_samples      (<=65535; writer flushes at 128)
     u64 t0_us          timestamp of first sample, microseconds
     n_samples times: u32 dt_us  (delta from previous sample; first sample's dt written 0)
                      f32 value[n_channels]
footer (optional, last 28 bytes):
  "LTFT" u64 samples  u32 blocks  u64 duration_us  u32 crc32(the 20 body bytes)
```
CRC32 = standard IEEE 802.3 (poly 0xEDB88320 reflected, init 0xFFFFFFFF, xorout 0xFFFFFFFF; crc32fast::hash). Check value crc32("123456789")=0xCBF43926.

### 3.1 Schema JSON (serde_json compact, field order as listed)
```
{"created_utc":"<RFC3339>","ini_signature":<string|null>,"sample_rate_hz":<num>,
 "channels":[{"name":"..","unit":"..","ini_type":".."},...]}
```
`ini_signature`, `unit`, `ini_type` optional on read (default null/""). `created_utc`, `sample_rate_hz`, `channels`, channel `name` required. Need a minimal JSON reader/writer in C (strings with escapes \" \\ \/ \b \f \n \r \t \uXXXX; numbers; null; ignore unknown keys). `lt_schema { char created_utc[40]; char *ini_signature; double sample_rate_hz; int n; struct {char *name,*unit,*ini_type;} *ch; }`.

### 3.2 Sample packing
`pack`: n = count (error if >65535 "ltlog block has too many samples"); t0_us = first sample micros (truncating `as_micros`); for i>0, dt = saturating(us_i - prev) clamped to u32 max, `prev += dt`; i==0 dt=0. Values stored as f32 (`(float)v` -> precision loss; read back via float->double, test tolerance 0.01).
`unpack`: read n, t0; per row need `4+4*n_ch` bytes remaining else stop early (silently truncate); sample0 time=t0 (dt ignored), i>0 time = prev + dt.

### 3.3 Writer (`lt_ltlog_writer`)
- `create(path, schema)`: create parent dirs (best effort), truncate file, write header, flush. n_channels = schema channel count.
- `push(entry)`: if finished -> error; if values count != n_channels -> error "ltlog sample has X values, schema has Y channels"; remember last_duration_us = entry micros; append to pending; if pending >= 128 (BLOCK_SAMPLES) OR >=1 s wall time since last flush -> flush_block.
- flush_block: empty -> no-op; write block; samples_written += n; blocks_written++; clear; reset timer; fflush.
- `flush_pending`: no-op if finished else flush_block (so concurrent readers see data).
- `finish`: idempotent; flush_block; write footer (samples_written, blocks_written, last_duration_us); fflush; finished=1. Rust also auto-finishes on drop; C: callers must call finish/close.
- `write_ltlog(path, schema, entries)`: create, push all, finish.
```c
int lt_ltlog_create(lt_ltlog_writer **w, const char *path, const lt_schema *s);
int lt_ltlog_push(lt_ltlog_writer *w, double t_sec, const double *v);
int lt_ltlog_flush(lt_ltlog_writer *w);
int lt_ltlog_finish(lt_ltlog_writer *w);   /* and free */
int lt_ltlog_read_schema(const char *path, lt_schema *out);
int lt_ltlog_visit(const char *path, int (*cb)(const lt_log_entry*, void*), void *ud, lt_schema *out);
int lt_ltlog_read(const char *path, lt_log *out);
int lt_ltlog_downsample(const char *path, size_t max_points, lt_schema*, lt_log *preview, unsigned long long *true_n);
int lt_ltlog_extract_columns(const char *path, const char **names, int n, lt_schema*, double **time_ms, double ***cols);
#define LT_UI_SAMPLE_CAP 4096
```

### 3.4 Reader algorithm (crash tolerant) `read_next_block`
loop:
1. `find_sync`: read 4 bytes; fewer than 4 (0..3) -> EOF. Then slide a byte at a time until window == "LTBK" (Block) or "LTFT" (Footer, ends reading) or EOF. (This skips junk between blocks.)
2. remember `after_sync` = position after the sync. Read u32 raw_len, u32 zlen, u32 crc; any EOF -> end of log (no error).
3. If zlen > 64 MiB or raw_len > 128 MiB: seek to `after_sync - 3` (one byte past the false sync's first byte) and retry from 1.
4. Read zlen bytes; short read -> end of log. CRC mismatch -> `continue` (resumes scanning from the position AFTER the payload that was just read; Rust does not rewind here).
5. Decompress; if fails or length != raw_len -> `continue`. Else unpack and return samples.
Errors other than UnexpectedEof propagate. A header-only file yields channels and zero samples (valid). Truncated last block silently dropped.
Gotcha: because step 4 skips past the whole claimed payload on CRC failure, a corrupt block loses only that block (tested). If its length field is corrupt but < 64 MiB it may swallow following blocks (accepted behavior).

### 3.5 downsample
n = footer sample count if valid footer (read last 28 bytes: len>=28, sync "LTFT", crc32(buf[4..24]) == u32 at buf[24..28]) else count by visiting. n==0 -> schema, empty, 0. n <= max_points -> all samples. Else stride = n/max_points (double); iterate samples index i; `if (double)i >= next && out.len < max_points { push; next += stride }`. max_points = max(1,arg). Test: 500 samples, max 50 -> 40..50 points, first value 0, last value >=450, n==500.

### 3.6 extract_columns
Map each wanted name to first matching schema channel index (None if absent). Per sample push `t*1000.0` ms into time vector; per found column push value (NaN if index out of range). Missing names -> empty column. Test: names [map,nope] -> time len 2, cols[0]=[40,55], cols[1] empty.

### 3.7 Test cases to port
- round trip 3 samples (t 0,20,40 ms; rpm 800/1200/3000; map 40/55.5/100) channel names [rpm,map], read[1].v0~1200, read[2].t == 40 ms.
- header-only file.
- 200 samples (t=i*10ms, v=i): truncate file by 40 bytes -> read.len >= 128 and < 200, read[0]==0.
- same, flip byte at (second "LTBK" offset+16) -> exactly 128 samples read, [127]==127.
- Writer constants: 200 samples -> blocks of 128 + 72 then footer.
- size ratio vs CSV >= 8x (100 channels, 500 samples @50Hz) - only valid WITH compression (see 3.9).

### 3.8 Footer semantics
Optional; absent footer still valid (reader stops at EOF). Reader stops scanning on encountering "LTFT" sync.

### 3.9 C89 / zero-dependency decision (REQUIRED)
Reference payload is zstd level 3. A C89 no-dep port cannot link zstd. Options: (a) implement a zstd DEcoder in C (~1.5-2k lines, FSE+Huffman) for reading Rust-written files and write blocks as zstd "raw/RLE" blocks only (valid zstd frame: magic 28 B5 2F FD, frame header, raw block type 0, no compression) - interoperable with the real decoder and trivially writable; or (b) bump header `flags` bit0 = 1 meaning "payload stored uncompressed / own LZ" and keep `zstd_len` = stored length. Recommended: (a)-lite: writer emits zstd raw-block frames (header: magic 0xFD2FB528 LE bytes `28 B5 2F FD`; frame header descriptor with single-segment flag + content size; each raw block 3-byte header `last<<0 | type0<<1 | size<<3`, blocks <=128 KiB), reader implements full zstd decode if cross-compat with Rust files is wanted, else rejects compressed blocks with a clear error. Either way keep CRC and raw_len checks (raw_len == decompressed length).

## 4. MLG (MegaLogViewer binary, all BIG-endian) - read version 2

```
header (24 bytes)
  0  "MLVLG\0"      6 bytes magic (4D 4C 56 4C 47 00)
  6  u16 version    must be 2 else error "MLG format version N is not supported, expected 2"
  8  u32 capture time (unix secs; ignored)
 12  u32 info offset (ignored)
 16  u32 data offset   (start of records)
 20  u16 record length (sum of field widths, bytes, payload excl. checksum)
 22  u16 field count
 file shorter than 24 -> "file is too short to be an MLG log"; bad magic -> "not an MLG log (bad file signature)"
field descriptor, 89 bytes each, field_count of them directly after header:
  0  u8  type  (0 u8,1 i8,2 u16,3 i16,4 u32,5 i32,6 i64,7 f32; other -> error "field N has unknown storage type T")
  1  [34] name   NUL-padded UTF-8
 35  [10] units  NUL-padded
 45  u8  display style (ignored)
 46  f32 scale
 50  f32 transform
 54  u8  digits (ignored)
 55  [34] category (ignored)          -> total 89
info text (MSQ tune) between descriptors end and data offset (skipped by seeking to data offset)
record:
  u8 kind; u8 counter; u16 clock (10 microsecond ticks, wraps at 65536)
  kind 0 (data):   payload[record_len] then u8 checksum = (sum of payload bytes) mod 256
  kind 1 (marker): 50 bytes text (NUL-padded), no checksum; record total 54 bytes
  other kind -> hard error "record kind K at record I is not one this reader knows" (cannot resync)
```
Field offsets inside payload accumulate in declaration order by type width (1,1,2,2,4,4,8,4). Sum must equal header record length else "header declares N-byte records but the fields need M".
Validation: scale/transform must be finite (else "field N has a non-finite scale"); data offset < 24+89*count -> "log data would start inside its own header"; descriptors cut short -> "log ends inside its field descriptors".
Names: bytes up to first NUL (or full width), lossy UTF-8, trimmed of whitespace. Units same.

Value decode: `value = (raw_as_double + (double)transform) * (double)scale` (transform ADDED then scale MULTIPLIED; scale/transform are f32 widened to double). i64 -> double conversion; f32 -> double.
Channels list = all field names in order (including Time).

### 4.1 Timestamps
- If a field exists with name equal ASCII-case-insensitively to "time" AND units == "s" (first such), timestamp = value(rec) - value(first accepted rec) seconds; non-finite value -> record dropped (counted `unusable_timestamp`), baseline set only from a finite value; negative/NaN elapsed -> 0 (`Duration::try_from_secs_f64` failure -> zero).
- Else timestamp = ticks*10 us, where ticks from `Clock`:
```
struct Clock { have_start; u64 start; u16 previous; u64 wraps; }
advance(raw): if have_start && saturating_sub_u16(previous, raw) > 32768 { wraps++ }   /* previous>raw by more than 0x8000 */
              previous = raw; absolute = wraps*65536 + raw;
              if !have_start {start=absolute; have_start=1}; return saturating(absolute - start)
```
Clock advances for EVERY record including markers and checksum-failed records. First record of any kind sets the start. Backward step <=32768 is NOT a wrap (out-of-order), result saturates at 0 if before start.

### 4.2 Read algorithm
read header, fields, seek data_start, loop: read 4-byte prefix with `fill` (retry on EINTR; 0 bytes -> clean EOF; 1..3 -> error "log ends part-way through record I"); advance clock; switch kind: data -> read record_len+1 bytes (short -> cut_short error), checksum mismatch -> failed_checksum++, skip (still index++); marker -> read 50 bytes (short -> error), no entry; unknown kind -> error. `index` counts all records. After loop: warn (stderr) if drops. Zero entries -> error "MLG log contains no readable records". Entries hold ALL channels incl. Time.

### 4.3 MLG writer (optional, derived from test helper)
Header as above with version 2, capture time = now, info_start = 24+89*n, data_start = info_start+len(info) (info may be empty), record_len = sum widths, field count. Descriptors: type code, name[34], units[10], style 0, scale f32 BE, transform f32 BE, digits 3, category 34 zeros. Records: `[0,counter++,clock_be16][payload BE][checksum]`; markers `[1,counter,clock][text padded to 50]`. For physical value v with u16 storage: raw = round(v/scale - transform) (inverse of decode). Name > 33 chars must be truncated.

### 4.4 Tests to port (all payloads big-endian)
Fields: TIME{f32,"Time","s",scale1,tr0}, CLT{u16,"CLT","deg C",0.01,0}, STFT{f32,"STFT: Bank 1","%",100,-1}.
- Records `(ctr0,clk1000,[t=0.0,clt=8741,stft=0.9352285])`, `(1,2000,[0.01,8747,1.0])`: channels ["Time","CLT","STFT: Bank 1"], 2 entries, CLT=87.41 (tol 1e-4), STFT[1]=0 (neutral trim), STFT[0] ~ (0.9352285-1)*100 = -6.47715.
- No Time channel (fields CLT,STFT), clocks 1000,2000: ts[0]=0, ts[1]=10_000us.
- version 1 file -> error; non-MLG bytes -> error.
- Marker(clk 1000, "MARK 000 - Going Online") then data clk 3000 -> first entry ts = 20_000 us (clock starts at first record of any kind).
- Truncated mid-record -> error; unknown kind byte -> error mentioning it; bad checksum record skipped (1 entry left, CLT 87.47); marker between data kept both (2 entries).
- File with zero records -> error. Time channel: t=10 then 4010 -> ts 0 and 4000 s (clock ignored). NaN time first, then 10,12 -> 2 entries ts 0 and 2 s.
- Wrap: clocks 65000 then 200 -> ts[1]=7_360 us. Small backward 1000,900,1100 -> 3 entries, ts[2]=1000 us (900 gives 0 via saturation, no wrap). Corrupt record at clk 60000 between 1000 and 200 -> 2 entries, ts[1]=647_360 us. Marker at 60000 same result.
- Info text between descriptors and data is skipped via data offset.
- Every scalar type decodes (u8,i8,u16,i16,u32,i32,i64,f32, with sign cases, big-endian).
- Real TunerStudio log fixture test is optional (needs fixture file).
MLG v1 is not supported by the reference (version != 2 rejected).

## 5. Recorder (`DataLogger`)
State: channels[], tail ring buffer (cap LIVE_TAIL=2048 entries, for live graph only), start_time, is_recording, sample_rate (default 10 Hz), last_sample time, last_timestamp, optional LtlogWriter, stream_path, stream_dir, finished_path, rows_written, malformed, ini_signature, channel_units[], channel_ini_types[].
Ops:
```c
lt_logger *lt_logger_new(char **channels, int n);
int  lt_logger_start_streaming(lt_logger*, const char *path); /* finish prev writer (finished_path=old); stream_dir=dirname(path); create writer with capture_schema(); rows=0; tail clear; last_timestamp=0 */
void lt_logger_set_capture_meta(lt_logger*, const char *sig /*"" -> none*/, char **units, char **ini_types); /* arrays applied only if length == n channels */
lt_schema lt_logger_capture_schema(const lt_logger*); /* created_utc=now RFC3339 UTC, sample_rate_hz */
void lt_logger_set_sample_rate(lt_logger*, double hz); /* clamp [1.0, 200.0] */
void lt_logger_start(lt_logger*);  /* start_time=now, recording=1, last_sample=none, last_ts=0, rows=0, tail clear */
void lt_logger_stop(lt_logger*);   /* recording=0; writer finish(); finished_path = stream_path */
void lt_logger_record(lt_logger*, const double *v, int n);
void lt_logger_clear(lt_logger*);
```
`record`: no-op if not recording. If channels non-empty and n != n_channels: malformed++ (warn once on first), drop, return (guard runs BEFORE rate limiter). Rate limit: min_interval = 1/sample_rate s; if last_sample set and now-last < min_interval -> drop. timestamp = now - start_time. If writer: push; on failure -> drop writer, finished_path=stream_path, recording=0, return (disk failure stops recording). Then rows_written++, last_timestamp=ts, tail push (pop_front when len>=2048), last_sample=now. Use monotonic clock (clock() or platform `clock_gettime`/QueryPerformanceCounter behind a tiny HAL).
`clear`: tail empty, rows=0, last_ts=0, malformed=0. If recording and stream_dir known: new file `<dir>/%Y-%m-%d_%H.%M.%S.ltlog` (local time), start_streaming; on success start_time=now,last_sample=none, return; else/no dir: start_time=now,last_sample=none. If not recording: start_time=none.
Queries: `entry_count()` = rows_written (not RAM); `entries()` tail iterator; `live_window(start_index,count)`: total=rows; origin = total - tail.len (saturating); start=min(start,total); end=min(start+count,total); from=max(start,origin); skip=from-origin; take=end-from(saturating); yield tail[skip..skip+take]. `malformed_count`, `duration()` = last_timestamp, `log_path()` = stream_path else finished_path, `stream_path()`, `flush_stream()` = writer.flush_pending, `channels()`.
Tests: basic start/record/count=1/stop; malformed rows [1000] and [1,2,3] vs 2 channels -> count 1, malformed 2; restart begins new timeline: start,record,stop (count1), start again, sleep 10ms, record (count 1), clear -> 0, start, record -> 1 (rate 200); live tail bounded: 2080 unthrottled records -> entry_count 2080, tail count 2048, file read has 2080 entries; streaming 2 records 7 ms apart at 200 Hz -> file has >=2 entries, values within 0.01. (`record_unthrottled` test helper resets last_sample before record.)
Markers/channel selection: NOT in reference; if added, markers would map to MLG kind-1 records (text <=50 bytes).

## 6. Playback (`LogPlayer`)
Fields: entries[], channels[], position (index, starts 0).
```c
size_t lt_player_len(p); int is_empty;
double lt_player_duration(p);   /* last entry's t, 0 if empty */
void lt_player_seek(p, size_t pos);          /* position = min(pos, len-1 saturating; 0 if empty) */
void lt_player_seek_time(p, double t);       /* first index with e.t >= t, else last index (0 if empty) */
const lt_log_entry *lt_player_current(p);    /* NULL if empty */
const lt_log_entry *lt_player_next(p);       /* if position+1 < len: position++, return current; else NULL, no move */
const lt_log_entry *lt_player_prev(p);       /* if position>0: position--, return; else NULL */
int lt_player_channel_index(p, const char *name);   /* -1 if absent; exact match, first */
double *lt_player_channel_values(p, name, size_t *n); /* empty if unknown; skips entries missing the index */
iterator entries_in_range(start,end): e.t >= start && e.t <= end (inclusive)
```
Tests: 3 entries t=0,1,2s rpm 1000/2000/3000: pos0 current=1000; next->pos1; seek_time(1.5s)->pos 2; channel_values(rpm)=[1000,2000,3000]. Empty player: current/next/prev NULL, position stays 0 after seek(5), seek_time(1s), next. Single entry: next NULL, pos 0. seek(huge) -> pos 2, next NULL stays 2.

## 7. Dyno (`dyno.rs`)
```c
typedef struct { double rpm; int has_hp,has_tq,has_afr,has_boost,has_time; double hp,torque,afr,boost,time; } lt_dyno_point;
typedef struct { char *name; lt_dyno_point *data; size_t n; int has_peak_hp; double peak_hp,peak_hp_rpm; int has_peak_tq; double peak_tq,peak_tq_rpm; char color[16]; char *source_file; } lt_dyno_run;  /* default color "#4fc3f7" for imports */
typedef struct { size_t rpm_col; int hp_col,torque_col,afr_col,boost_col,time_col; /* -1 = none */ } lt_dyno_colmap;
typedef struct { size_t row,col; int has_hp,has_tq,has_afr; double hp,torque,afr; size_t sample_count; } lt_dyno_cell;
typedef struct { lt_dyno_run a,b; double *hp_diff /*pairs rpm,diff*/; size_t n_hp; ... torque_diff,n_tq; int has_total_hp; double total_hp_change; int has_total_tq; double total_torque_change; } lt_dyno_cmp;
```
### 7.1 CSV line parser `parse_csv_line`
Split on `,` OR `\t` outside quotes; `"` toggles in_quotes and is dropped (no `""` escape); always pushes final field. `"1000,50.5,\"quoted,value\",14.7"` -> 4 fields, [2]=`quoted,value`.
### 7.2 `from_csv(path, name)`
Empty file -> error "Empty CSV file". Header = parse_csv_line(line0), each field lowercased and trimmed. `auto_detect_columns`: for each header index i, with h lowercase, an else-if chain (first branch matched wins; each column assigned at most once; each slot taken only while unset), in this order:
1. rpm (unset) if h contains "rpm" or h=="speed" or h=="engine speed"
2. hp if contains "hp"|"horsepower"|"power"|"whp"|"bhp"
3. torque if contains "torque"|"tq"|"ft-lb"|"ftlb"|"nm"
4. afr if contains "afr"|"lambda"|"air fuel"|"a/f"
5. boost if contains "boost"|"manifold"|"map"
6. time if contains "time"|"elapsed"|"sec"
Missing rpm -> error "Could not find RPM column in CSV headers". Note order gotchas: "rpm" matched before hp; a header "horsepower" contains "power" for hp, "nm" matches anything containing "nm" (e.g. "manifold"? no: 'manifold' has no "nm"; but "engine"... contains no "nm"; beware "unmatched"). Test: headers [time,rpm,hp,torque,afr] -> rpm1 hp2 tq3 afr4 time0; [hp,torque] -> error.
Rows (lines 1..): trim; skip empty or starting `#`; fields = parse_csv_line(line); skip if fields.len <= rpm_col; rpm = trimmed parse f64 else skip; skip if rpm not in [100.0, 20000.0] inclusive; optional columns: `fields.get(col)` trimmed parse else None. Sort by rpm ascending (stable sort not specified; use stable, NaN equal). compute_peaks. source_file = path. Color "#4fc3f7".
`from_csv_with_map(path,name,map)`: same but header line is just skipped (no lowercasing), no empty-file error, rpm range check inline; same sort/peaks.
`compute_peaks`: peak_hp = (hp,rpm) with max hp among points having hp (`max_by`: LAST of equal maxima wins, Rust `max_by` returns last max); same for torque. Test: points (3000,100/175),(4000,150/197),(5000,200/210),(6000,220/192) -> peak_hp (220,6000), peak_torque (210,5000).
`rpm_range`: (first.rpm,last.rpm) or none. `detect_csv_headers(path)`: first line parse_csv_line (NOT lowercased); empty -> error "Empty file".
### 7.3 `map_to_table(x_bins, y_bins, boost_to_load?)`
rows = len(y_bins), cols = len(x_bins). Per cell arrays of hp/tq/afr samples. For each point: col = nearest(x_bins, rpm) (ties -> first; `col >= cols` skip, only when bins empty... note nearest returns 0 for empty bins then idx check `col>=cols` skips); row = if point.boost && factor present: nearest(y_bins, boost*factor) else `rows-1` (saturating, "WOT assumption"); skip if row>=rows. Append hp/tq/afr when present. Output cells in row-major order where count = max(len hp, len tq, len afr) > 0, with averages (None when no samples), sample_count=count. `find_nearest_bin`: linear scan, strictly-less distance replaces, so first of equal distances wins; empty -> 0. Tests: bins [500,1000,1500,2000]: 800->1, 500->0, 1999->3, 1250->1. map_to_table with no boost: all cells row==4 (y_bins len 5).
### 7.4 Compare (`DynoComparison::compare(a,b)`)
For each point in a (a's order): if a.hp present and interpolate(b, a.rpm, hp) exists -> push (a.rpm, hp_b - hp_a); likewise torque. totals = arithmetic mean of diffs (None if empty).
`interpolate_at_rpm(data, rpm, getter)`: empty->None. Scan ALL points having the value: `lower` = the LAST such point with point.rpm <= rpm; `upper` = the FIRST with point.rpm >= rpm (set once). Both required (else None: no extrapolation / no endpoint hold). If |r2-r1| < 1e-9 return v1 else v1 + (rpm-r1)/(r2-r1)*(v2-v1). Assumes data sorted by rpm. Tests: data (1000,50),(2000,100),(3000,150): at 1500 -> 75, 2500 -> 125; out-of-range 2999 and 6001 with data 3000/6000 -> None, exact endpoints 3000->100, 6000->200. Compare A(3000:100,5000:200) B(3000:110,5000:215) -> hp_diff [(3000,10),(5000,15)]. Range test: A rpm 2000(80),3000(100),6000(200),7000(210); B 3000(110),6000(215) -> hp_diff rpms [3000,6000], mean 12.5.
JSON serialization of DynoRun/Comparison (serde) is for IPC only; not needed.

## 8. Math channels (`project/math_channels.rs`)
```c
typedef struct { char *name, *units, *expression; expr_node *cached_ast; /* not persisted */ } lt_math_channel;
int lt_math_compile(lt_math_channel*, char *err, size_t n); /* parse expression via INI expression parser; cache AST; error string on failure */
size_t *lt_math_eval_order(lt_math_channel *ch, size_t n);  /* returns permutation of 0..n-1 */
int lt_math_save(const char *path, ch, n);   /* pretty JSON array */
int lt_math_load(const char *path, lt_math_channel **out, size_t *n); /* missing file -> empty list OK */
```
The expression language (Expr: Literal, Variable, Binary, Unary, Ternary `?:`, FunctionCall; evaluate_simple(expr, var_map)->f64) is implemented in the INI expression subsystem (`ini/expression`), not here; this module depends on it. Test expressions: `(map - 100) * 0.1` compiles; `map + ` fails to compile; `Boost_PSI > 5 ? 1 : 0`; `rpm / 100`; `1+1`.
Persistence: JSON array of `{"name":..,"units":..,"expression":..}` (serde_json::to_string_pretty: 2-space indent, `"key": value`, `[]` for empty). Load: file absent -> `Ok(empty)`; parse JSON (error returned as string); compile each, IGNORING compile errors (invalid channels load, and fail at runtime / flagged in UI). Unknown JSON keys ignored.
Evaluation order (Kahn's, stable): 
1. n<=1 -> [0..n). 2. Compile any channel lacking an AST (errors ignored -> no deps). 3. name->index map (duplicate names: last wins in a HashMap). 4. deps[i] = set of j != i where j's name equals any Variable referenced anywhere in i's AST (walk Literal/Variable/Binary/Ternary/Unary/FunctionCall args; function NAMES are not variables). 5. Repeat rounds: scan i=0..n-1; if not placed and all deps placed -> place immediately (so within a round, later channels can see earlier ones placed in the same round) and append; stop when a full pass places none. 6. Remaining (cycle members) appended in stored order.
Evaluation of each channel per sample: in order, `value = eval(ast, vars)`; `vars[name] = value` (shared map so later channels see earlier results).
Tests: [Boost_Warn:"Boost_PSI > 5 ? 1 : 0", Boost_PSI:"(map - 100) * 0.145"] -> order [1,0]; with map=150: Boost_PSI=7.25 (tol 1e-9), Boost_Warn=1. Chain [c:"b + 1", x:"map * 2", b:"a + 1", a:"rpm / 100"]: pos(a)<pos(b)<pos(c) and pos(x)<pos(b) (exact result: round1 places x(1), a(3); round 2 places c? no: c needs b; scanning i=0 c not ready, x placed, b needs a (placed in round1? a is index 3 scanned after b in round 1) -> round1 = [x(1), a(3)], round2 = [c? no, b(2) after c scanned]: round 2: c not ready, b ready -> place b; round3: c. Final [1,3,2,0]). Cycle [p:"q + 1", q:"p + 1", solo:"rpm"] -> [2,0,1]. Invalid expr "map + " + "rpm * 2" -> both indices present. Persistence round trip: A/"u"/"1+1", B/"v"/"2*2": loaded[0].name "A", expression "1+1", AST present.

## 9. Checklist of gotchas
- MLG big-endian; LTLOG little-endian; CSV `%.3f`/`%.4f`, '.' decimal regardless of locale.
- MLG value = (raw + transform) * scale; marker record is 54 bytes, no checksum; clock advances on every record; checksum failures skip not abort.
- MLG time channel ("time"/"s") overrides clock; baseline from first finite value.
- LTLOG f32 precision loss; first dt 0; CRC32 IEEE; footer optional; resync scanning after bad block; zstd decision (3.9).
- Recorder rate clamp [1,200] Hz default 10; guards: malformed-before-rate-limit; disk failure stops recording.
- Dyno: rpm accepted range [100,20000]; no extrapolation in compare; last-wins on peak ties; header detection is an else-if chain.
- Math channels: Kahn with in-round placement; cycles appended in stored order.
