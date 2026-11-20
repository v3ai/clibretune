# 03 - Protocol (ECU communication) spec

Source: `libretune-core/src/protocol/*`, `src/ecu/*`, plus `ini/types.rs` (`ProtocolSettings`, `AdaptiveTiming`, `EcuType`, `Endianness`) and `tests/calibration_protocol.rs`, `tests/protocol.rs`.
Target: C89, libc/POSIX only (termios/select on unix, Win32 serial/winsock via src/plat/win32; abstract behind `lt_chan`).

OUT OF SCOPE: tracing/logging macros, serde, Tauri state, Arc/Mutex threading (use a single volatile int `cancel` flag), the in-process simulator ("demo mode" `connect_with_channel`, can be reused for tests), INI parsing itself (spec 01/02; this doc lists the fields consumed).

---
## 1. Constants

```c
#define LT_DEFAULT_BAUD        115200u
#define LT_DEFAULT_TIMEOUT_MS  2000u      /* ConnectionConfig.timeout_ms (used for TCP connect deadline, and as timeout only if no INI settings) */
#define LT_MAX_PACKET_SIZE     65535u     /* payload max (2-byte length field) */
#define LT_BURN_SETTLE_MS      600u       /* added to page_activation_delay after burn */
#define LT_LEGACY_WRITE_SETTLE_MS 30u     /* min quiet time after legacy write frame */
#define LT_DEFAULT_TCP_PORT    29001
#define LT_DEFAULT_TCP_HOST    "localhost"
```

## 2. Error enum (`lt_err`)

OK=0, SERIAL_ERROR(msg), TIMEOUT, NOT_CONNECTED, CONNECTION_FAILED(msg), CONNECTION_CLOSED (cancelled), ALREADY_CONNECTED, INVALID_RESPONSE, CRC_MISMATCH{expected,actual u32}, SIGNATURE_MISMATCH{expected,actual str}, ECU_ERROR(u8), ECU_STATUS_ERROR{code u8, message}, BUFFER_OVERFLOW, PROTOCOL_ERROR(msg), WRITE_VERIFICATION_FAILED{page,offset,expected u8,actual u8}, PAGE_CRC_MISMATCH{page,expected u32,actual u32}, WRITE_VERIFICATION_UNAVAILABLE{page,offset,reason}, PORT_NOT_FOUND, IO_ERROR. Keep an `lt_errinfo` struct {code, msg[256], aux fields}. Display formats of note: `"CRC mismatch: expected 0x%08x, got 0x%08x"`, `"ECU error 0x%02x: %s"`.

## 3. Response codes (msEnvelope_1.0 section 15.2)

First payload byte of every enveloped reply.

| byte | name | default message |
|---|---|---|
|00|Ok|"OK"|
|01|OkTable|"OK (table response)"|
|02|OkBurn|"Burn complete"|
|03|SettingsError (carries payload msg)|"Controller refused settings (power cycle required to clear)"|
|04,05,06|OkAux/2/3|"OK"|
|80|Underrun|"Controller Reported an Under-run"|
|81|Overrun|"Controller Reported an Over-run"|
|82|CrcMismatch|"Controller Reported a CRC Mismatch"|
|83|UnrecognizedCommand|"Controller Reported a Unrecognized Command"|
|84|OutOfRange|"Controller Reported a Out of Range"|
|85|Busy|"Controller reporting BUSY"|
|86|FlashLocked|"Controller Reported Flash Locked"|
|87|SeqFailure1|"Controller Reported Sequence Failure 1"|
|88|SeqFailure2|"Controller Reported Sequence Failure 2"|
|89|CanQueueFull|"Controller Reported CAN Queue full"|
|8A|CanTimeout|"Controller Reported CAN Timeout"|
|8B|CanFailure|"Controller Reported CAN Failure"|
|8C|ParityError|"Controller Reported Parity Error"|
|8D|FramingError|"Controller Reported Framing Error"|
|8E|SerialNoise|"Controller Reported Serial Noise"|
|8F|TxmodeRange|"Controller Reported txmode range error"|
|90|UnknownSerialError|"Controller Reported Unknown Serial Error"|
|91|TooManyBadCan|"Too Many Bad Requests for unavailable CAN ID"|
|92|CanDeviceUnavailable|"A Controller is responding, but not at the project assigned CAN ID"|
|93|HighSpeedTableUnset|"High speed runtime table not set"|
|94|GenericError (carries payload msg)|"Controller reported a generic error"|
|95|CriticalError (carries payload msg)|"Controller reported a critical error"|
|other|UndefinedError(byte)|"Controller reported an undefined error code"|

Predicates: `is_ok` = {00..02,04..06}; `is_error` = (byte & 0x80) || byte==0x03 (so 0x03 is NOT ok and IS error); `carries_payload_message` = {03,94,95}. Unknown bytes < 0x80 (e.g. 0x07) are UndefinedError, is_error=false, is_ok=false. Round-trip byte<->code must be lossless (test).

When building EcuStatusError: if code carries payload msg and payload.len>1, message = payload[1..] lossy-UTF8 (NOT trimmed), else default message.

## 4. Framing

### 4.1 Legacy (no envelope)
Raw bytes: command bytes out, reply bytes in; no length, no CRC, no status byte. Reply end detected by expected length or inter-character silence (section 6.2).

### 4.2 msEnvelope_1.0 ("modern", CRC) frame
```
offset 0  : u16 length N         (payload length incl. response/status byte for replies)
offset 2  : N bytes payload      (request: command bytes; reply: [status][data...])
offset 2+N: u32 CRC32            (over PAYLOAD ONLY - not the length bytes)
total = 2 + N + 4
```
- CRC32: standard reflected CRC-32/ISO-HDLC (poly 0x04C11DB7 reflected = 0xEDB88320, init 0xFFFFFFFF, refin/refout, xorout 0xFFFFFFFF; check("123456789") = 0xCBF43926). Table-driven, 256-entry.
- Byte order of BOTH length and CRC = "envelope order": `BIG` (msEnvelope_1.0 / rusEFI, default) or `LITTLE` (Speeduino: "TS comms is little-endian"). Example: payload `51` BIG -> `00 01 51 crc_be[4]`; LITTLE -> `01 00 51 crc_le[4]`.
- GOTCHA: `Packet::new` stores a `crc` field computed over length(BE)+payload - this stored field is unused and WRONG vs the wire; always compute wire CRC over payload only. Do not port the stored field.
- Max N = 65535 (`BUFFER_OVERFLOW` if length field larger; unreachable with u16 except via limit test, keep check).

### 4.3 Decode (`lt_packet_decode(data,len,order,permissive,&pkt)`)
1. `len < 6` -> INVALID_RESPONSE.
2. N = read_u16(order, data[0..2]); N > 65535 -> BUFFER_OVERFLOW; `len < 2+N+4` -> INVALID_RESPONSE (e.g. BE frame `00 01 51 ...` read as LE gives N=256 -> INVALID_RESPONSE; test `cross_order_read_misparses_length`).
3. payload = data[2..2+N]; recv_crc = read_u32(order, data[2+N..+4]); crc_a = crc32(payload). If equal -> OK.
4. Strict mode (permissive=0): else CRC_MISMATCH{expected=crc_a, actual=recv_crc}.
5. Permissive mode (config `permissive_crc`, default false): accept if ANY of: A-LE (recv bytes read little-endian == crc_a); B = crc32(payload[1..]) (payload.len>1 else 0) with recv BE or LE; C = crc32(len_bytes||payload) BE or LE; D = crc32(len_bytes||payload[1..]) (payload.len>1 else 0) BE or LE. (len_bytes = the raw 2 header bytes.) Returned pkt.crc = BE value if B/C/D-BE matched else LE value. If none: CRC_MISMATCH{expected=crc_a, actual=recv_crc_BE}. (Quirk: with len 1 B/D crc is 0.)
6. Encode: write_u16(order,N) || payload || write_u32(order,crc32(payload)).

Tests: roundtrip `[0x51]`; flipped byte -> error; strict rejects scope-C frame built from payload `00 42 43` (BE len + crc over len+payload), permissive accepts; LE roundtrip: encoded[0..2]=`01 00`, encoded[2]=0x51, encoded[3..7]=crc32([0x51]).to_le_bytes; LE rc frame `[0x82]` decodes; PacketBuilder `R,0,0,u16be(0),u16be(128)` = 7 bytes payload[0]=='R'.

PacketBuilder helpers: `command(b)`, `byte(b)`, `u16_be`, `u32_be`, `bytes(p,n)` -> append to payload.

## 5. Command strings and builder

### 5.1 `parse_command_string(s)` (used for query command, handshake)
Scan bytes: on `\` followed by:
- `x`/`X`: needs `i+3 < len` (i.e. at least 2 chars after x AND one more char beyond - NOTE strict `<`: string `\x0f` of len 4 has i=0,i+3=3<4 OK). If the 2 chars parse as hex -> push byte, i+=4. Else push `\` literal, i+=1.
- `n`->0x0A, `r`->0x0D, `t`->0x09, `\\`->0x5C, `0`->0x00 (all i+=2).
- anything else: push `\` literal, i+=1.
Other bytes pushed as-is. Tests: `\x0f`->0F; `\x00\x0f\x14`->00 0F 14; `r\x00\x0f\x00\x00\x00\x14` -> 7 bytes `72 00 0F 00 00 00 14`; `\n \r \t \\ \0`; "Q","S","Hello" unchanged.
Caveat: `\x` at the very end (e.g. string exactly `\x41` is 4 chars: i=0 -> 3<4 ok). Edge: `i+3 < len` means `\x4` + nothing fails fine.

### 5.2 `CommandBuilder` (`lt_cmd_build(fmt, page, offset, count, data, ndata, little_endian, out)`)
`little_endian` = INI endianness==Little (rusEFI/FOME/epicEFI), Big for Speeduino/MS2/MS3. It governs %2i/%2o/%2c ONLY (not envelope order). Walk the format char by char (chars cast to u8; non-ASCII UTF-8 chars truncate to low byte - port as bytes):
1. `%` with next=`2` and next-next in {i,o,c} (requires `i+2 < len`): emit 2 bytes (u16) of page / offset / count in command endianness; i+=3. `%2` followed by other letter falls through (emits `%` literally, then continues scanning from next char).
2. `%v` (requires i+1<len): append data bytes verbatim; i+=2. (`%v` is checked only when the `%2x` branch did not apply; i.e. `%2` with a non-matching 3rd char does NOT try `%v`.)
3. `\` escapes (same set as 5.1, but written separately): `n r t 0 \\` as 5.1; `\x`/`\X` requires `i+3 < len` AND 2 hex digits -> byte, i+=4; otherwise falls to step 4 and `\` is emitted literally.
4. Anything else: emit byte.
`%2i` page value is the page IDENTIFIER (section 9), not the index. `%2c` for write = data length. Nothing else (no `$tsCanId` handling) - the INI parser must already have substituted `$tsCanId` (canId byte, default 0x00) and other `$var`s with literal bytes; the test passes a raw NUL byte in the string for it (`"r\0\\x30%2o%2c"` where `\0` there is a real 0 byte, then the 4-char `\x30`).
Wrappers: read(fmt,page,off,count); write(fmt,page,off,data) count=len; burn(fmt,page) off=0,count=0; crc(fmt,page,off,count); och(fmt,count) page=0,off=0.

Test vectors:
- `r\0\x30%2o%2c` LE, och count 130 -> `72 00 30 00 00 82 00`.
- `r\x00\x06%2o%2c` BE read off=0x0204 cnt=16 -> `72 00 06 02 04 00 10`.
- `R%2i%2o%2c` BE page0 off100 cnt256 -> `52 00 00 00 64 01 00`.
- same LE page1 off0x100 cnt0x200 -> `52 01 00 00 01 00 02`.
- `C%2i%2o%2c%v` BE page0 off50 data AA BB CC -> `43 00 00 00 32 00 03 AA BB CC`.
- `B%2i` page0 -> `42 00 00`. Empty format -> empty command.

Console command: bytes = text + `\n`, default timeout 1000 ms. Calibration/console command structs: `ReadMemoryParams{page u8,offset u16,length u16,can_id u8=0}`, `WriteMemoryParams{page,offset,data,can_id}`, `BurnParams{page,can_id}`. (can_id is carried but never used by the builder.)

## 6. Transports and timing

### 6.1 Channel abstraction
```c
typedef struct lt_chan lt_chan; /* vtable: read(buf,n) nonblock-capable, write(buf,n), flush, set_timeout(ms),
   clear_input(), clear_output(), bytes_to_read(), close() */
```
- Serial: open at baud (default 115200), 8N1, no flow control, read timeout 100 ms (open_port), then `configure_port`: set DTR high and RTS high (failure ignored; DTR high is to avoid Arduino reset), then clear ALL buffers. Errors -> SERIAL_ERROR.
- TCP: host default "localhost", port default 29001 (INI `default_ip_port`). Resolve first (failure -> CONNECTION_FAILED "cannot resolve host:port: err" / "no address found for host:port"), `connect` with deadline = config.timeout_ms (nonblocking connect+select), TCP_NODELAY on. set_timeout sets both read & write timeouts. clear_input: set nonblocking, read-and-discard until 0/EAGAIN, restore blocking. clear_output = flush. bytes_to_read: nonblocking `MSG_PEEK` into 8192 buf, returns n (EAGAIN->0), restore blocking.
- `list_ports`: enumerate OS ports; linux: also scan /dev for names starting `ttyACM`/`ttyUSB` and add if missing (vid/pid unknown). Windows: probe-open each (50 ms) and drop failures (ghost COM entries); `list_ports_unprobed` skips probe. Sort key: basename (after last `/`): `ttyACM<n>` -> (0,n), `ttyUSB<n>` -> (1,n), others (2,0,basename); numeric n, unparsable = max. Test: [USB1,ACM1,USB0,ACM0,someport,ACM10] -> ACM0,ACM1,ACM10,USB0,USB1,someport. PortInfo{name,vid,pid,manufacturer,product,serial}.

### 6.2 write_and_wait (unix)
write_all(data); then sleep `wait_ms = max(min_wait_ms, transmit_ms + 5)` where `transmit_ms = (len*10 * (1e9 / baud)) / 1e6` using integer math (baud 0 -> 115200), `min_wait_ms` default 10. (Avoids blocking tcdrain.) Non-unix: write_all + flush, no sleep.

### 6.3 Effective timing (all in ms)
- `effective_timeout` = adaptive.timeout if adaptive enabled else `protocol.block_read_timeout` (INI, default 1000) else `config.timeout_ms`.
- `effective_inter_char` = adaptive.inter_char else clamp(block_read_timeout/4, 25, 100) (base 1000 if no INI).
- `effective_min_wait` = adaptive.min_wait else max(inter_write_delay, 10) (10 if no INI).
- poll interval: 1 ms if adaptive enabled else 2 ms.

AdaptiveTiming (optional, default off): config {min_timeout 10, max_timeout 500, sample_count 20, multiplier 2.5}. state: ring of last `sample_count` response times (us), running sum, current_timeout initial = (min+max)/2 ms. `record(elapsed)`: ignored if disabled; push, drop oldest at capacity; avg_us = sum/n; timeout_ms = (u64)((float)avg_us*mult)/1000, clamped [min,max]. `get_timeout`: current if enabled&&samples>0 else max_timeout. inter_char = max(5, timeout/4). min_wait = max(5, timeout/3). `reset_on_error`: clear samples, sum=0, current=(max*3)/4. `set_enabled(false)` clears samples. Recorded only after successful full responses; reset on errors/timeouts/cancel in read paths.

### 6.4 Legacy raw exchange `send_raw_command_expecting(cmd, expected_len)`
1. Compute timing params; channel must exist else NOT_CONNECTED.
2. clear_input & clear_output; start timer; `write_and_wait(cmd)` (errors -> SERIAL_ERROR).
3. Loop (poll): if elapsed since start > timeout -> break. If cancel flag -> reset adaptive, return CONNECTION_CLOSED. `avail = bytes_to_read` (error -> SERIAL_ERROR). If avail>0: read min(avail,512); 0 bytes -> break (EOF); append; last_data=now; if `expected_len>0 && total>=expected_len` break. Timeout/WouldBlock errors ignored; other errors -> reset adaptive, SERIAL_ERROR. Else if response empty: sleep poll; else if now-last_data > inter_char -> break else sleep poll.
4. Empty response -> reset adaptive, TIMEOUT. Else rx_bytes+=n, rx_packets++, record response time, return bytes (may be longer than expected; NOT truncated here).
`expected_len` of Some(0) means unknown (never early-exit). Tests `response_is_complete(received, expected)`: (0,None)=F,(130,None)=F,(0,Some0)=F,(50,Some0)=F,(129,Some130)=F,(130,Some130)=T,(131,Some130)=T,(1,Some130)=F.
`send_raw_command_no_response(cmd)`: write_and_wait only.

### 6.5 Enveloped exchange `send_packet(pkt) -> pkt`
1. bytes = encode(envelope_order); tx counters++; write_and_wait(bytes) (min_wait). Do NOT clear input first (would eat an already-arrived reply on fast TCP).
2. ONE deadline = now + effective_timeout shared by header and body.
3. `read_exact_by_deadline(buf)`: loop: if now>=deadline -> TIMEOUT; if cancel -> CONNECTION_CLOSED; avail=bytes_to_read (err->SERIAL_ERROR); if 0 sleep poll, continue; read min(avail,remaining); n==0 -> TIMEOUT(EOF); timeout/wouldblock -> continue; else SERIAL_ERROR.
4. Read 2-byte header; on failure: clear_input, reset adaptive, return err. N = read_u16(envelope_order); N>65535 -> clear_input, BUFFER_OVERFLOW.
5. Read N+4 bytes with same deadline; on failure: drain (read & discard up to N+4 bytes for up to 500 ms polling bytes_to_read, then clear_input), reset adaptive, return err. (Test: a stalled payload must cost ONE timeout, not two.)
6. record response time, rx counters += 2+N+4, decode with envelope_order and `permissive_crc`. On CRC failure no drain needed (frame was fully consumed). `send_packet` does NOT inspect the status byte; callers do.
`send_packet_no_response`: encode, write_all, flush (no settle sleep), tx counters.

## 7. Connection lifecycle

Config: {type SERIAL|TCP, port_name, baud=115200, tcp_host, tcp_port, use_modern_protocol=true, timeout_ms=2000, runtime_packet_mode AUTO|FORCE_BURST|FORCE_OCH|DISABLED, permissive_crc=false, auto_burn_on_page_change=true, auto_burn_on_close_dialog=true (unused)}.
State: DISCONNECTED, CONNECTING, CONNECTED, ERROR. Session state: signature, use_modern (bool), envelope_order (default BIG), protocol settings, command endianness, ecu_type, last_written_page (opt), dirty_pages (sorted set u8), failed_pages (sorted set u8), cancel flag, counters tx_bytes/rx_bytes/tx_packets/rx_packets.

`with_protocol/set_protocol(settings, endianness)`: use_modern = `message_envelope_format` contains substring "msEnvelope"; command_builder LE iff endianness==Little; envelope_order reset to BIG. A bare `Connection::new` has use_modern=true, builder BE, endianness Little, no settings.

`connect()`:
1. state CONNECTED -> ALREADY_CONNECTED. Clear cancel flag (cleared HERE, not in disconnect). state=CONNECTING.
2. Open channel (6.1). Sleep `delay_after_port_open` ms (INI; default 1000 if no INI; ProtocolSettings default value is 0), `clear_input`, sleep 20 ms. (Skipped for pre-opened channels.)
3. handshake; success -> signature, CONNECTED; failure -> state ERROR, channel dropped, return err.
`disconnect()`: set cancel flag = 1 and LEAVE it raised, drop channel, signature=none, DISCONNECTED. (`Drop` calls disconnect.) Tests: flag is raised after disconnect; connect clears stale flag.

### 7.1 Handshake
`query_cmd` = INI `query_command` (default "S" if no INI; ProtocolSettings default "Q"); `cmd_bytes = parse_command_string(query_cmd)`; `cmd_byte` = first byte or 'Q'. `ini_modern` = settings.uses_modern (false without INI).
A. If ini_modern: clear_input. For order in [declared_order, flipped]: set envelope_order=order; if attempt>0: sleep 450 ms, clear_input (firmware ~400 ms SERIAL_TIMEOUT eats bytes). Send Packet(cmd_bytes) via send_packet (errors ignored); on Ok, `signature_from_payload(payload)`; if Some -> use_modern=true, return sig. After both fail: restore envelope_order=declared, fall through.
B. Legacy: clear_input; `send_raw_command(&[cmd_byte])`. On Ok: use_modern=false; signature = lossy-UTF8(response).trim(). (Not validated, may be empty-trim string.) On Err(e): if !ini_modern: clear_input, sleep 50 ms, try one CRC attempt with CURRENT envelope_order (no flip); success -> use_modern=true, sig. Return original legacy Err otherwise.
`signature_from_payload(p)`: empty -> None; first byte's ResponseCode is_error (incl. 0x03, >=0x80) -> None; bytes = p[1..] if p[0]==0 else p; trim ASCII/Unicode whitespace; empty -> None. Tests: `[0x80]`,`[0x84,'x']`,`[0x03,'n','o']`,`[]`,`[0x00]`,`"   "` -> None; `00 "speeduino 202501.4 "` -> "speeduino 202501.4"; `"rusEFI master.2024 "` -> "rusEFI master.2024".

### 7.2 EcuType detection (from INI signature + filename; case-insensitive)
Order: contains "fome" (sig or filename) -> FOME; contains "epicefi"/"epicecu" (sig or filename) -> EpicEFI; sig contains "rusefi" -> RusEFI; sig contains "speeduino" -> Speeduino; sig startswith "ms3" -> MS3; sig startswith "ms2" or contains "ms2extra" -> MS2; else Unknown. `supports_console` = RusEFI|FOME|EpicEFI. Display names: Speeduino, rusEFI, FOME, epicEFI, "MegaSquirt 2", "MegaSquirt 3", Unknown. Envelope order choice by dialect (Speeduino=LE) is made by the caller/INI layer; handshake flip-retry corrects mistakes.

## 8. Realtime (OCH / burst) polling

`choose_runtime_command() -> (kind BURST|OCH, cmd string, reason)`:
burst_cmd = settings.burst_get_command or "A"; och = settings.och_get_command (optional).
1. FORCE_BURST: if use_modern && och present -> OCH (reason "force: ForceBurst (burst is a no-op in new-comms; using OCH)"); else BURST.
2. FORCE_OCH: och present -> OCH else BURST.
3. DISABLED -> BURST.
4. AUTO: `burst_ecu` = ecu_type in {Speeduino, MS2, MS3, Unknown}. If use_modern && burst_ecu && och -> OCH ("auto: OCH (modern protocol negotiated)"). If burst_ecu -> BURST ("auto: Burst (ecu=<display>)"). Else (rusEFI family): max_unused_runtime_range>0 && och -> OCH; slow link && och -> OCH; adaptive enabled with avg>50 ms && och -> OCH; else BURST ("default: burst").
`is_slow_link`: lowercase port_name contains rfcomm|bluetooth|tcp|telnet|wifi, or baud < 57600.
Tests: rfcomm0+RusEFI+och "O"-> OCH; Speeduino/MS2/MS3/Unknown with rfcomm, max_unused=999, slow adaptive (200,180 ms) -> Burst "A" (reason contains "Burst"); ForceOCH overrides Speeduino; RusEFI + adaptive avg 190 ms -> OCH reason starts "adaptive".

`get_realtime_data() -> bytes`:
- If chosen OCH but `och_block_size==0` -> switch to BURST (burst cmd).
- BURST modern: Packet(cmd.bytes) -> send_packet -> `strip_status_byte(payload, och_block_size, "Burst")`. BURST legacy: send_raw_command_expecting([first byte of cmd or 'A'], och_block_size>0 ? Some : None) - raw result returned as-is.
- OCH: if cmd contains `%`: build via `build_och_command(cmd, och_block_size or 256 if 0)`; else raw bytes of the string (NOT escape-parsed). Modern: send_packet then `strip_status_byte(payload, och_block_size,"OCH")`. Legacy: multi-byte cmd -> send_raw_command(all); else single byte (default 'A'); no expected length.
`strip_status_byte(payload, want, label)`:
 - want>0: len==want+1 && p[0]==0 -> p[1..]; len==want -> p; len==want+1 && p[0]!=0 -> p[1..] (warn only); else PROTOCOL_ERROR("{label} response: {n} bytes, expected {want} (or {want+1} with a status byte) - channel offsets cannot be trusted").
 - want==0: empty -> empty; p[0]==0 -> p[1..]; else whole payload.
Tests: want 130: 128x0xAB err; `00`+100 err; empty err; `00`+130x0x11 ok->130; 130x0x11 ok. want 0: `00 01 02`->`01 02`; `09 01 02`->`09 01 02`.

## 9. Page read / write / burn

Page identifier `page_id(idx)`: `settings.page_identifiers[idx]` raw bytes: >=2 bytes -> u16 LE of first two; 1 byte -> that byte; empty/missing -> idx. (The builder then re-encodes with command endianness; for LE-declared INIs the two inversions cancel so bytes reach the wire in INI order. Do not "fix".)

`read_memory(page, offset, length)`:
- fmt = settings.page_read_commands[page] (default "R%2i%2o%2c" when index missing); empty string -> PROTOCOL_ERROR "No read command for page N". cmd = build_read(fmt, page_id, offset, length).
- Modern: send_packet; empty payload -> INVALID_RESPONSE; payload[0]!=0 -> PROTOCOL_ERROR("Read error, status: N"); data = payload[1..]. Legacy: `send_raw_command_expecting(cmd, Some(length))`.
- Then require data.len()==length else PROTOCOL_ERROR("wrong-length read on page P offset O: got X of Y bytes") (short OR long).
`read_page(page)`: size = page_sizes[page] (0/missing -> PROTOCOL_ERROR "Unknown page size for page N"); bf = blocking_factor (default 256); loop chunk=min(remaining,bf), read_memory, append; offset advances by chunk (u16 offset!).

`effective_write_chunk` = max(1, blocking_factor.max(1) - 8). Speeduino bf=251 -> 243. (Frame = 7B header + data <= 250 <= 257 Mega RX ring.)

`inter_frame_delay(frame_len)`: wire_ms = ceil(frame_len*10*1000/baud); settle = max(effective_min_wait, floor) with floor = 5 if modern else 30 (LEGACY_WRITE_SETTLE_MS); unix: max(wire_ms, settle); non-unix: wire_ms + settle. Tests (legacy, BE, 115200): delay(250)>=30ms; delay(20)>=30ms; at 9600 baud delay(250)>=260 ms; with inter_write_delay=100, delay(8) in [100,105]; modern with default settings delay(20) < 30 ms.

`write_memory(page, offset, data)`:
1. If data.len > effective_write_chunk: split sequentially (offset advances), recursing; return.
2. Auto-burn safety: if config.auto_burn_on_page_change and last_written_page=Some(prev) and prev != page: if failed_pages non-empty -> skip (warn); else `burn(prev)` (error propagates).
3. fmt = settings.page_chunk_write_commands[page], default "C%2i%2o%2c%v"; empty -> PROTOCOL_ERROR "No write command for page N". cmd = build_write(fmt, page_id, offset, data).
4. Modern: send_packet, then `check_write_response_status`: empty payload OK; first byte code !is_error OK (extra bytes ignored); else ECU_STATUS_ERROR{code,msg per section 3}. Legacy: `send_raw_command_no_response(cmd)` (writes are silent; waiting would time out ~2 s).
5. On success: last_written_page=page; dirty_pages.insert(page); sleep `inter_frame_delay(len(cmd))`.
Tests: ack `[0x84]` -> ECU_STATUS_ERROR code 0x84 msg "Controller Reported a Out of Range"; `[0x94]+"flash write failed"` -> msg "flash write failed"; `[]`,`[0]`,`[0,AB,CD]` ok.

`write_page(page, data)`: chunk = effective_write_chunk; for each chunk call write_memory (retry up to 3 attempts total if error text contains "121", "timeout" (case-ins) or "semaphore": clear_rx, sleep 50*(attempt+1) ms; non-transient or 3rd failure returns error). After all chunks: `verify_page_crc(page,data)`: PAGE_CRC_MISMATCH -> fail; any other error (no command declared, no answer) -> only warn. On any failure page added to `failed_pages`; on success removed. Test (Speeduino sim, bf=251, 288-byte page 3): wire bursts exactly [250, 52] (data+7B header), 0 dropped, page intact.

`verify_page_crc(page, expected)`: fmt = crc32_check_commands[page] non-empty else PROTOCOL_ERROR "no crc32CheckCommand declared for page N". cmd = build_crc(fmt, page_id(page), 0, expected.len). clear_rx; `send_raw_bytes_with_response(cmd, effective_timeout)`; reply.len<4 -> PROTOCOL_ERROR; reported = u32 BIG-endian of reply[0..4] (after status strip on modern); local = crc32(expected); mismatch -> PAGE_CRC_MISMATCH{page, expected=local, actual=reported}. (Speeduino INI: `d%2i`; ECU returns return-code then BE CRC32 on modern path; legacy raw returns 4 BE bytes.)

`write_memory_verified(page, offset, data)`: write_memory; return OK if modern or empty data (acks suffice). Legacy: read back in chunks of effective_write_chunk via read_memory; any read error -> WRITE_VERIFICATION_UNAVAILABLE{page, offset_at, reason}; short part also UNAVAILABLE; then first differing index i -> WRITE_VERIFICATION_FAILED{page, offset+i (saturating u16), expected[i], actual[i]}.

`burn(page)`:
1. failed_pages non-empty -> PROTOCOL_ERROR "refusing to burn: page(s) {list} failed to write or verify; write the tune again or reconnect first".
2. fmt = burn_commands[page], default "B%2i" (index missing); empty string -> return OK (not burnable), no state change. cmd = build_burn(fmt, page_id).
3. Send WITHOUT waiting: modern -> send_packet_no_response; legacy -> send_raw_command_no_response.
4. Sleep `max(page_activation_delay,2000) + 600` ms (page_activation_delay default 500; 2000 if no INI). Then drain what arrived (LibreTune-C addition): a read error (link lost) -> SERIAL_ERROR "connection lost while burning page N", a modern reply whose status has the high bit set -> ECU_STATUS; in both cases the page stays in dirty_pages (burn not confirmed, may be partial). Silence is accepted. Then clear_rx.
5. If last_written_page==page -> None; dirty_pages.remove(page).
`send_burn_command()`: pages = dirty_pages ascending, or [0] if empty; burn each (stop at first error). `dirty_pages()` accessor. Test logic: dirty {2,3}, burn 3 leaves {2}; empty -> [0]; {7,2,15,3,0} burns 0,2,3,7,15. A failed write_page blocks send_burn_command and burn() ("refusing to burn"); write_memory to other page after failed page skips auto-burn instead of failing.

`send_raw_bytes(bytes)` (controller buttons): empty -> OK; modern -> Packet(bytes) no response; legacy -> raw no response.
`send_raw_bytes_with_response(bytes, timeout)`: modern: send_packet, `strip_status_byte(payload,0,"raw command")`. Legacy: clear_input, write_and_wait, read until: elapsed>timeout, or (have data && quiet >50 ms); poll 1 ms; bytes_to_read error with data -> stop else SERIAL_ERROR; buffer 4096. Returns possibly-empty bytes (no error on empty).
`clear_rx_buffer()`: channel.clear_input if channel.

## 10. Console (rusEFI/FOME/epicEFI only)
Modern (`send_console_command_modern`): (1) drain: up to 3x send Packet(`'G'`); data_len = payload.len-1 if len>1 && p[0]==0 else 0; stop when 0 (sleep 30 ms between non-empty); error -> stop. (2) send Packet(`'E'`+text); if payload non-empty and p[0]!=0: UnrecognizedCommand(0x83) -> PROTOCOL_ERROR "ECU does not support console commands (unrecognized 'E' command)"; other is_error -> ECU_STATUS_ERROR (payload msg rules). (3) sleep 50 ms; poll up to 10 times while elapsed <= cmd timeout (default 1000): send Packet(`'G'`); text = p[1..] if len>1&&p[0]==0; empty if len==0||p[0]==0; else whole payload. Empty: empty_polls++, if collected non-empty or empty_polls>=2 break else sleep 50; nonempty: append lossy-UTF8, empty_polls=0, sleep 20. Poll error: if collected non-empty break else return error. (4) result = `parse_rusefi_text_output(collected)`; empty -> "(command accepted, no output)". tx/rx packets += 2.
Legacy: clear in/out, write_and_wait(text+"\n"), read until timeout (cmd timeout) or quiet > inter_char (response empty keeps waiting, 1 ms polls), result lossy-UTF8 trimmed; empty -> TIMEOUT.

`parse_rusefi_text_output(raw)`: "" -> "". Split on backtick; consume pairs (tag=parts[i], content=parts[i+1]), i+=2 while i+1 < parts.len. tag = trim. Skip pair if tag empty and content empty. If tag equals (case-insens) "msg" or "emu": content.trim() non-empty -> line. Else if tag startswith any of [`wave_chart`,`outpin`,`t|d_`,`map|u`,`maf|u`,`maf|d`,`hpfp|d`,`hpfp|u`,`hpfp2|d`,`pfp|u`,`wave`,`VVT|`] -> skip. Else if tag non-empty and trimmed content non-empty, len>2 and all ASCII graphic or space -> line `"[tag] content"`. If no lines and raw.trim non-empty -> raw.trim(). Join with "\n".
Tests: `msg`Hello from ECU`` -> "Hello from ECU"; 3 msgs -> joined "\n"; `msg`RPM=1200`wave_chart`some chart data here`msg`emu: running`` -> "RPM=1200\nemu: running"; `msg`Status OK`t|d_123`456`msg`Done`` -> "Status OK\nDone"; "Some plain text response" returned as-is; `emu`RPM=1200`emu`shape update for ch0`` -> "RPM=1200\nshape update for ch0"; boot sequence sample yields 4 lines (wave_chart dropped).

## 11. Speeduino sensor calibration (`t` command, firmware tag 202501)
Only when ecu_type in {Speeduino, Unknown}, else PROTOCOL_ERROR "sensor calibration write is only implemented for Speeduino (connected ECU type: X)". Table ids: CLT=0, IAT=1, O2=2. Wire lengths: CLT/IAT 64 bytes (32 x i16 little-endian, value = degF*10); O2 1024 bytes (1 byte per 10-bit ADC count, AFR*10). Length mismatch -> PROTOCOL_ERROR.

`temperature_calibration_bins(modern)`: bin[x] = x*(modern?33:32), x=0..31 (legacy 0..992, modern 0..1023).
`firmware_stored_temperature(raw i16)` = max(0, (((raw/10) - 32)*5)/9 + 40) with C truncating integer division (int32), returned as u16 (units degC+40).
`encode_temperature_calibration(temps_c[32])`: for each c: f10=(c*9/5+32)*10; base=round(f10) (round half away from zero) clamped to i16; search delta in -18..=18 (saturating add): raw=base+delta; err=|firmware_stored_temperature(raw)-40-c|; keep the FIRST minimum (strict `<`, delta ascending from -18); emit raw as i16 LE. Guarantees whole degC in -40..210 round-trip exactly and tenths within 0.5 degC.
`encode_o2_calibration(afr[1024])`: clamp(round(a*10),0,255) as u8. Test: afr 9.0->90, 100->255, -1->0, 14.7->147.
`calibration_crc32(wire)` = standard crc32 of wire bytes.

Legacy write (`use_modern==false`): send raw `['t', id]` (no response), then data in 128-byte chunks each via no-response write followed by sleep `max(effective_min_wait,5)` ms; finally sleep `max(page_activation_delay,500)` ms. No ACK, no verification (warn: firmware > 202501 ignores legacy cal writes).
Modern: per chunk (O2 = 256-byte chunks x4; temp = single 64-byte chunk) send Packet payload: `'t', 0x00, table_id, offset_hi, offset_lo, len_hi, len_lo, data...` (offset and length BIG-endian u16 regardless of envelope/command endianness; offset = chunk index*chunk size). Reply status byte != 0 (empty counts as 0xFF) -> PROTOCOL_ERROR "calibration chunk at offset N rejected: 0x%02x (msg)". Then verify: `read_calibration_crc(table)`: modern only (else PROTOCOL_ERROR "calibration CRC ('k') requires the CRC protocol..."); send Packet `['k', 0x00, table_id]`; reply must be >=5 bytes with [0]==0 else PROTOCOL_ERROR "calibration CRC read failed, response: ..."; crc = BE u32 of reply[1..5]. Compare to calibration_crc32(wire); mismatch -> PROTOCOL_ERROR "calibration verify failed for X: ECU stored CRC %08x, expected %08x". `write_temperature_calibration(O2,...)` -> PROTOCOL_ERROR "O2 table takes AFR data; use write_o2_calibration".
Tests: ids 0/1/2; encode(100C)~2120 (+-18), (-40C)~-400 (+-18); round-trip exactness loop c=-40..=210 (`firmware_stored_temperature(raw)==max(c+40,0)`); tenths -400..=2100 within 0.5; bins[31]=992 legacy/1023 modern.

## 12. Discovery / autodetect (`discover_ecu`)
Defaults: baud sweep `[115200, 921600, 460800, 230400, 57600, 9600]`; queries `"QS"` (bytes 'Q','S'); post_open_settle 1500 ms; pre_write_delay 40 ms; read_window 600 ms. Configurable via DiscoveryConfig; open function injectable (for tests use a mock channel).
Per port, per baud in order: open (fail -> push OpenFailed{baud,err}, continue). sleep settle; set_timeout(read_window); clear_input. got_any=false. For each query byte: sleep pre_write; clear_input; write_all([q]) (fail -> continue); flush; `bytes = read_window()`; empty -> continue; got_any=true; classify; push result; if GpsReceiver or candidate -> RETURN for this port immediately. After all queries: if !got_any push Silent{baud}. (Echo/Unknown don't stop the sweep.)
`read_window(window)`: deadline now+window; loop while now<deadline: read(256 tmp); 0 -> break; n -> append, break if total >= 32; TimedOut or any error -> break. (Returns at first quiet/timeout.)
`classify(baud, query, bytes)` in order:
1. len>=3 && starts "$GP" -> GpsReceiver.
2. all bytes == query -> Echo.
3. len>=3 && (b0 & 0xE0)==0xE0 && (b1 & 0xF0)==0 && b2=='>' -> Bootloader.
4. b0==0x01 -> BigStuff3.
5. len>=8 and printable% >= 70 (printable = ASCII graphic 0x21..0x7E or space; integer `printable*100/len >= 70`) -> Signature{lossy-UTF8 trimmed}.
6. Unknown{bytes}.
Candidate = Signature|Bootloader|BigStuff3; `best()` priority Signature > Bootloader > BigStuff3 (first of each in result order).
Tests (mock 1 baud, queries Q,S, 0 delays, 20 ms window): 'S' -> "Speeduino 2024.10  " (Q->empty) -> Signature starts "Speeduino"; reply `E0 05 3E` -> Bootloader; `01 AA BB CC` -> BigStuff3; "$GPGGA,123519,4807.038" -> GpsReceiver present, best()==None; echo (q repeated x4) -> all results Echo, no best; no data -> Silent; open fail -> OpenFailed.
(DiscoveryResult kinds: Signature, Bootloader, BigStuff3, Echo, GpsReceiver, Unknown, Silent, OpenFailed.)

## 13. ECU memory model + dirty tracking (`ecu/`)

`EcuMemory`: map page(u8) -> byte buffer; `from_definition(page_sizes[], n_pages)` allocates zeroed pages of each size (index i -> page i); `page_count`, `page_size(page)` (None if out of range of page_sizes), `read_bytes(page,off,len)` -> slice or NULL if page missing or `off+len > size` (use size_t math), `write_bytes(page,off,data,n)` -> bool (false if page missing or overflow, no partial write), `load_page(page, data)` (replaces/creates, any size), `get_page`. Test: load 256-byte page, write [1,2,3,4] at 10, read back.
```c
int lt_mem_write(lt_mem*, u8 page, u16 off, const u8* d, size_t n);
const u8* lt_mem_read(const lt_mem*, u8 page, u16 off, u16 len); /* NULL on failure */
```
`ShadowMemory`: set of (page, offset) dirty bytes. `mark_dirty(page, off, len)` marks `off..off+len-1` (u16 arithmetic: wraps/overflows in Rust debug; in C clamp or use u32 and cap at 65535). `is_dirty`, `has_changes`, `dirty_pages()` sorted unique ascending, `clear()`, `dirty_count()`. In C use a per-page bitmap (page size bytes/8) or sorted range list instead of a hash set. Test: mark(0,10,4) -> dirty 10 and 13, not 14; clear -> no changes. NOTE: this is distinct from Connection.dirty_pages (pages written to ECU RAM, not yet burned).
`Value` typed enum {Scalar f64, Array1D, Array2D{data,rows,cols}, Bool, String}; `get_2d(r,c)` = data[r*cols+c] or none if out of range; `as_scalar` only for Scalar. Tests: Scalar(14.7); 2x3 grid get_2d(1,2)=6.0.

## 14. Test case checklist to port
1. CRC32 check value 0xCBF43926 and one-bit-flip changes CRC (288-byte pattern `(i % 251)`, flip byte 144 bit 0).
2. All packet/envelope tests in section 4.3; cross-order misparse.
3. Command builder vectors section 5.2; parse_command_string vectors 5.1.
4. ResponseCode round trip over 29 listed codes + 0xAB; is_ok/is_error/payload_message predicates.
5. check_write_response_status, signature_from_payload, strip_status_byte, response_is_complete vectors.
6. Mega2560 legacy-write simulator (build a fake channel): 257-byte RX ring drops overflow; M-frame state machine; verify `[250,52]` bursts, no drops, gap >= 30 ms between frames, verified-write detects complemented byte at (page 1, offset 100) -> WRITE_VERIFICATION_FAILED{1,100,table[100],!table[100]} and passes when honest. Sim: `M` + page(u16 BE) + offset(u16 BE) + count(u16 BE) + data; `r` + same 6 header bytes returns bytes. Config: bf=251, inter_write_delay=0, block_read_timeout=100, write fmt "M%2i%2o%2c%v", read fmt "r%2i%2o%2c", 8 pages, endianness Big, legacy.
7. effective_write_chunk(251)==243; inter_frame_delay vectors.
8. choose_runtime_command vectors; adaptive timing math (avg 190 ms *2.5 -> 475 ms, clamped 10..500; `reset_on_error` -> 375).
9. Stalled payload costs one timeout (< 2x timeout for header at 0.8x timeout; timeout 800 ms).
10. Cancel flag lifecycle; TCP address resolve ("127.0.0.1",29001 ok; ".invalid" host -> CONNECTION_FAILED).
11. Dirty/burn ordering; failed-page burn refusal; `verify_page_crc` with no settings -> error text containing "crc32CheckCommand" and the page number.
12. Calibration tests (section 11) incl. byte-exact `t` frames: O2 chunk headers `74 00 02 00 00 01 00`, `74 00 02 01 00 01 00`, `... 02 00 01 00`, `... 03 00 01 00` (offsets 0,256,512,768 BE, len 256 BE); temp header `74 00 <id> 00 00 00 40`; legacy O2 streams 1024 bytes after `74 02` and the firmware keeps every 32nd.
13. Discovery tests (section 12), port sort order, rusEFI text parser vectors.

## 15. Gotchas summary
- Two independent endianness knobs: command-parameter endianness (INI `endianness`; Speeduino INI = Big for %2i/%2o/%2c even though envelope is LE) vs envelope order (BE default, Speeduino LE, handshake auto-flips). Calibration `t` header is always BE.
- Envelope CRC covers payload only (ignore the buggy `Packet.crc` field); status byte is part of payload; replies may omit it (rusEFI OCH) - detect via declared och_block_size.
- Legacy writes/burns have NO reply: never wait; pace with inter_frame_delay; burn waits max(delay,2000)+600 ms.
- Never write frames > blocking_factor-8 data bytes; never read short/long pages silently; refuse burns after any failed page.
- Use a single deadline per enveloped response; do not clear input before reading a framed reply.
- Do not clear the cancel flag in disconnect; clear it in connect.
- DTR/RTS asserted high after open; wait delay_after_port_open (INI) then flush before handshake.
