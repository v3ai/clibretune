/* proto.h - ECU communication protocol (Speeduino / rusEFI / FOME / epicEFI / MegaSquirt).
 *
 * Spec: docs/spec/03-protocol.md (all section numbers below refer to it).
 *
 * Layering
 *   lt_stream (plat.h, serial/TCP)  --lt_proto_io_from_stream-->  lt_proto_io
 *   lt_ecusim (sim.h)               --lt_ecusim_io------------->  lt_proto_io
 *   any test double                 --------------------------->  lt_proto_io
 *   lt_proto_io  ->  lt_proto (this file: framing, handshake, pages, realtime)
 *
 * lt_proto is SINGLE THREADED and BLOCKING: every call runs to completion or
 * timeout. Long operations (reading 40 KB pages, writing a tune) call the
 * progress callback regularly; the GUI uses it to pump events / redraw and may
 * return non-zero to cancel (LT_ECANCEL). A cancel flag (volatile int) can also
 * be raised from a signal handler. Timing (sleep + clock) can be replaced for
 * tests so unit tests do not really wait.
 *
 * MEMORY: lt_proto does not own the lt_ini (must outlive it) nor the io's
 * underlying device unless lt_proto_close() is used (calls io.close). The ECU
 * memory shadow (lt_ecu_mem) is separate and owned by the caller.
 *
 * Errors: functions return 0 or a negative LT_E* (lt_codes.h); details of the
 * last failure are in lt_proto_errinfo(p) and lt_err_get().
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PROTO_H
#define LT_PROTO_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../plat/plat.h"
#include "../ini/ini.h"

/* ====================================================================== */
/* Constants (spec 1)                                                      */
/* ====================================================================== */
#define LT_DEFAULT_BAUD          115200L
#define LT_DEFAULT_TIMEOUT_MS    2000
#define LT_MAX_PACKET_SIZE       65535UL
#define LT_BURN_SETTLE_MS        600
#define LT_LEGACY_WRITE_SETTLE_MS 30
#define LT_DEFAULT_TCP_PORT      29001
#define LT_DEFAULT_TCP_HOST      "localhost"

/* ====================================================================== */
/* Byte transport                                                          */
/* ====================================================================== */
/* The protocol talks to this vtable only. Same semantics as plat.h streams. */
typedef struct lt_proto_io {
    void *ud;
    /* Read up to n bytes, waiting at most timeout_ms for the FIRST byte
     * (0 = non blocking). Returns bytes read (>= 0, 0 = nothing yet) or -1 on
     * error/closed. Required. */
    int  (*read)(void *ud, void *buf, int n, int timeout_ms);
    /* Write all n bytes. Returns n or -1. Required. */
    int  (*write)(void *ud, const void *buf, int n);
    /* Push buffered output to the wire (may be NULL). */
    void (*flush)(void *ud);
    /* Discard pending input (may be NULL: then proto drains with read(0)). */
    void (*clear_input)(void *ud);
    /* Release the device (may be NULL). Called by lt_proto_close(). */
    void (*close)(void *ud);
} lt_proto_io;

/* Adapter over an open lt_stream (serial or TCP). own: close the stream on
 * lt_proto_close. The returned io borrows `s`; keep it alive. */
lt_proto_io lt_proto_io_from_stream(lt_stream *s, lt_bool own);

/* ====================================================================== */
/* Wire primitives (spec 3, 4, 5) - pure functions, directly unit-testable */
/* ====================================================================== */
/* CRC-32 (IEEE, reflected): lt_proto_crc32(data,n) == lt_crc32(0,data,n);
 * check value for "123456789" is 0xCBF43926. */
lt_u32 lt_proto_crc32(const void *data, size_t n);

typedef enum lt_env_order { LT_ENV_BIG = 0, LT_ENV_LITTLE = 1 } lt_env_order;   /* length+CRC byte order */

/* Response codes: first payload byte of every enveloped reply (spec 3). */
#define LT_RC_OK            0x00
#define LT_RC_OK_TABLE      0x01
#define LT_RC_OK_BURN       0x02
#define LT_RC_SETTINGS_ERR  0x03
#define LT_RC_UNDERRUN      0x80
#define LT_RC_OVERRUN       0x81
#define LT_RC_CRC_MISMATCH  0x82
#define LT_RC_UNKNOWN_CMD   0x83
#define LT_RC_OUT_OF_RANGE  0x84
#define LT_RC_BUSY          0x85
#define LT_RC_GENERIC_ERR   0x94
#define LT_RC_CRITICAL_ERR  0x95
lt_bool     lt_rc_is_ok(lt_u8 code);                /* 00..02, 04..06 */
lt_bool     lt_rc_is_error(lt_u8 code);             /* high bit set or 0x03 */
lt_bool     lt_rc_carries_message(lt_u8 code);      /* 03, 94, 95 */
const char *lt_rc_message(lt_u8 code);              /* default text, e.g. "Controller Reported a Out of Range" */

/* msEnvelope_1.0 frame: u16 length, payload, u32 CRC of PAYLOAD ONLY, both in `order`. */
void lt_env_encode(lt_env_order order, const lt_u8 *payload, size_t n, lt_buf *out);
/* Decode one frame from data[0..len). On success returns 0 and sets *payload
 * (points INTO data), *plen, *consumed (= 2+plen+4). Errors: LT_EDATA (frame
 * shorter than header says: "invalid response"), LT_ECRC (strict mismatch).
 * permissive accepts the alternative CRC scopes of spec 4.3 step 5. */
int  lt_env_decode(lt_env_order order, lt_bool permissive, const lt_u8 *data, size_t len,
                   const lt_u8 **payload, size_t *plen, size_t *consumed);

/* Parse a command string with escapes (spec 5.1: \xNN \n \r \t \\ \0). */
void lt_cmd_parse_string(const char *s, size_t n, lt_buf *out);
/* Build a command from an INI format (spec 5.2): %2i page id, %2o offset, %2c
 * count (u16 in command endianness), %v data bytes, escapes as above, anything
 * else literal. `fmt` may contain NUL bytes. The INI layer already substituted
 * $tsCanId. */
void lt_cmd_build(lt_buf *out, const char *fmt, size_t fmt_len, lt_u32 page_id, lt_u32 offset,
                  lt_u32 count, const lt_u8 *data, size_t ndata, lt_bool little_endian);

/* Speeduino sensor calibration encoders (spec 11), pure. */
#define LT_CAL_CLT 0
#define LT_CAL_IAT 1
#define LT_CAL_O2  2
/* 32 temperatures (degC) -> 64 bytes (32 x i16 LE, degF*10, firmware round-trip optimised). */
void lt_cal_encode_temperature(const double temps_c[32], lt_u8 out[64]);
/* 1024 AFR values -> 1024 bytes (round(afr*10) clamped 0..255). */
void lt_cal_encode_o2(const double afr[1024], lt_u8 out[1024]);
lt_s32 lt_cal_firmware_stored_temperature(lt_s32 raw_f10);   /* spec 11 formula, degC+40 */
void lt_cal_temperature_bins(lt_bool modern, double bins[32]);  /* adc counts of the 32 slots */

/* rusEFI console text: strips wave_chart/outpin noise, joins msg lines (spec 10). malloc'd. */
char *lt_parse_rusefi_text_output(const char *raw);


/* ---- testable helpers (additive, pure) ---- */
/* Spec 7.1 signature_from_payload: copies the trimmed signature into sig and
 * returns LT_TRUE, or LT_FALSE (empty / error status / only whitespace). */
lt_bool lt_proto_signature_from_payload(const lt_u8 *p, size_t n, char *sig, size_t cap);
/* Spec 9 check_write_response_status: 0 if empty or not an error code, else
 * LT_EPROTO with msg = default text or payload text for 03/94/95. code gets the byte. */
int  lt_proto_check_write_status(const lt_u8 *p, size_t n, char *msg, size_t cap, int *code);
/* Spec 8 strip_status_byte. Returns offset of the data inside the payload
 * (0 or 1) and sets *dlen, or LT_EPROTO with a message in lt_err. */
int  lt_proto_strip_status(const lt_u8 *p, size_t n, size_t want, const char *label,
                           size_t *dlen);
/* Spec 6.4 response_is_complete. expected 0 = unknown. */
lt_bool lt_proto_response_complete(size_t received, size_t expected);

/* ====================================================================== */
/* ECU memory shadow with per-page state and dirty tracking                */
/* ====================================================================== */
/* Local copy of the ECU's pages (spec 03 section 13 EcuMemory+ShadowMemory and
 * spec 04 section 2 TuneCache; this is THE implementation of both). The GUI
 * edits this; lt_proto_write_dirty() sends the dirty ranges; burn marks clean.
 * Not thread safe. */
typedef enum lt_page_state {
    LT_PAGE_NOT_LOADED = 0, LT_PAGE_LOADING, LT_PAGE_CLEAN, LT_PAGE_DIRTY,
    LT_PAGE_PENDING,        /* written to ECU RAM, not yet burned to flash */
    LT_PAGE_ERROR
} lt_page_state;

typedef struct lt_range { size_t start, len; } lt_range;

typedef struct lt_ecu_mem lt_ecu_mem;
/* Allocate state for pages 0..n-1 of the given sizes (no buffers are invented:
 * pages are NOT_LOADED until lt_ecu_mem_load_page()). sizes may be NULL when n==0. */
lt_ecu_mem *lt_ecu_mem_new(const unsigned long *page_sizes, size_t n_pages);
lt_ecu_mem *lt_ecu_mem_from_ini(const lt_ini *ini);          /* sizes from ini->proto.page_sizes */
void        lt_ecu_mem_free(lt_ecu_mem *m);
size_t      lt_ecu_mem_page_count(const lt_ecu_mem *m);
size_t      lt_ecu_mem_page_size(const lt_ecu_mem *m, int page);     /* declared size, 0 unknown */
lt_page_state lt_ecu_mem_state(const lt_ecu_mem *m, int page);
void        lt_ecu_mem_set_state(lt_ecu_mem *m, int page, lt_page_state s);   /* LOADING/ERROR */
/* Replace the image (copies); state CLEAN; dirty bits untouched. */
void        lt_ecu_mem_load_page(lt_ecu_mem *m, int page, const lt_u8 *data, size_t len);
/* Image view or NULL unless state is CLEAN/DIRTY/PENDING. */
const lt_u8 *lt_ecu_mem_page(const lt_ecu_mem *m, int page, size_t *len);
/* Bounds-checked read view (NULL on failure). */
const lt_u8 *lt_ecu_mem_read(const lt_ecu_mem *m, int page, size_t off, size_t len);
/* Copy data in (never grows the page), mark bytes dirty, state DIRTY. 0 or LT_ERANGE/LT_ENOENT. */
int         lt_ecu_mem_write(lt_ecu_mem *m, int page, size_t off, const lt_u8 *data, size_t n);
lt_bool     lt_ecu_mem_fully_loaded(const lt_ecu_mem *m);
size_t      lt_ecu_mem_pages_to_load(const lt_ecu_mem *m, int *out, size_t cap);   /* NOT_LOADED pages */
lt_bool     lt_ecu_mem_has_dirty(const lt_ecu_mem *m);
size_t      lt_ecu_mem_dirty_bytes(const lt_ecu_mem *m);
size_t      lt_ecu_mem_dirty_pages(const lt_ecu_mem *m, int *out, size_t cap);      /* ascending */
lt_bool     lt_ecu_mem_is_dirty(const lt_ecu_mem *m, int page, size_t off);
/* Maximal contiguous dirty runs of a page (scans the declared page size). */
size_t      lt_ecu_mem_dirty_ranges(const lt_ecu_mem *m, int page, lt_range *out, size_t cap);
void        lt_ecu_mem_mark_pending(lt_ecu_mem *m);   /* after write to ECU RAM: dirty pages -> PENDING, clear bits */
void        lt_ecu_mem_mark_burned(lt_ecu_mem *m);    /* PENDING -> CLEAN */
lt_bool     lt_ecu_mem_has_pending_burn(const lt_ecu_mem *m);
void        lt_ecu_mem_revert(lt_ecu_mem *m);         /* clear dirty bits; DIRTY pages -> NOT_LOADED */
/* View for lt_ini_* value codec / tableops / basemap (writes mark dirty). */
lt_pagesrc  lt_ecu_mem_pagesrc(lt_ecu_mem *m);

/* ====================================================================== */
/* Connection                                                              */
/* ====================================================================== */
typedef enum lt_conn_state { LT_CONN_DISCONNECTED = 0, LT_CONN_CONNECTING, LT_CONN_CONNECTED, LT_CONN_ERROR } lt_conn_state;
typedef enum lt_runtime_mode { LT_RT_AUTO = 0, LT_RT_FORCE_BURST, LT_RT_FORCE_OCH, LT_RT_DISABLED } lt_runtime_mode;

typedef struct lt_proto_cfg {
    int          timeout_ms;            /* fallback when INI has no blockReadTimeout (2000) */
    long         baud;                  /* for pacing math (0 -> 115200) */
    const char  *port_name;             /* only for slow-link detection ("rfcomm","tcp",...); may be NULL */
    lt_runtime_mode runtime_mode;       /* AUTO */
    lt_bool      permissive_crc;        /* false */
    lt_bool      auto_burn_on_page_change;   /* true */
    lt_bool      adaptive_timing;       /* false (spec 6.3) */
    int          env_order_hint;        /* -1 = derive from INI (Speeduino LE else BE), or lt_env_order */
    int          legacy_read_min_wait_ms;    /* 10 */
} lt_proto_cfg;
void lt_proto_cfg_default(lt_proto_cfg *c);

typedef struct lt_proto_stats { unsigned long tx_bytes, rx_bytes, tx_packets, rx_packets; } lt_proto_stats;

/* Detailed error of the last failed call (spec 2). */
typedef enum lt_proto_errcode {
    LT_PE_NONE = 0, LT_PE_SERIAL, LT_PE_TIMEOUT, LT_PE_NOT_CONNECTED, LT_PE_CONNECTION_FAILED,
    LT_PE_CANCELLED, LT_PE_ALREADY_CONNECTED, LT_PE_INVALID_RESPONSE, LT_PE_CRC_MISMATCH,
    LT_PE_SIGNATURE_MISMATCH, LT_PE_ECU_STATUS, LT_PE_BUFFER_OVERFLOW, LT_PE_PROTOCOL,
    LT_PE_WRITE_VERIFY_FAILED, LT_PE_PAGE_CRC_MISMATCH, LT_PE_VERIFY_UNAVAILABLE,
    LT_PE_PORT_NOT_FOUND, LT_PE_IO
} lt_proto_errcode;
typedef struct lt_proto_err {
    lt_proto_errcode code;
    char msg[256];
    lt_u32 a, b;            /* CRC expected/actual, page/offset, or status code in `a` */
    int page; int offset; int expected, actual;   /* WRITE_VERIFY_FAILED details */
} lt_proto_err;

/* Progress/cancel callback: called between chunks. phase text e.g. "read page 1".
 * Return non-zero to cancel the operation (-> LT_ECANCEL). */
typedef int (*lt_proto_progress_fn)(void *ud, const char *phase, int page, size_t done,
                                    size_t total);
/* Replaceable time source (tests). now_ms must be monotonic. */
typedef struct lt_proto_clock {
    void *ud;
    double (*now_ms)(void *ud);
    void   (*sleep_ms)(void *ud, int ms);
} lt_proto_clock;

typedef struct lt_proto lt_proto;
/* `ini` may be NULL (then: legacy plain mode, query "S", default timings). The
 * io is copied (struct); the device behind it is not owned unless closed. */
lt_proto *lt_proto_new(const lt_proto_io *io, const lt_ini *ini, const lt_proto_cfg *cfg);
void      lt_proto_free(lt_proto *p);                 /* does NOT close the io */
void      lt_proto_close(lt_proto *p);                /* io.close, state DISCONNECTED, cancel flag raised */
void      lt_proto_set_progress(lt_proto *p, lt_proto_progress_fn fn, void *ud);
void      lt_proto_set_clock(lt_proto *p, const lt_proto_clock *clk);   /* NULL = plat clock */
/* Cancel flag: set non-zero from anywhere (GUI, signal handler) to abort the
 * running call. Cleared by lt_proto_connect(); left raised by disconnect. */
volatile int *lt_proto_cancel_flag(lt_proto *p);
void      lt_proto_cancel(lt_proto *p);
const lt_proto_err *lt_proto_errinfo(const lt_proto *p);
lt_conn_state lt_proto_state(const lt_proto *p);
const char   *lt_proto_signature(const lt_proto *p);   /* "" before connect */
lt_bool       lt_proto_is_modern(const lt_proto *p);   /* CRC envelope negotiated */
lt_env_order  lt_proto_envelope_order(const lt_proto *p);
const lt_proto_stats *lt_proto_stats_get(const lt_proto *p);

/* Open phase (after the caller opened the device): wait delay_after_port_open,
 * flush input, run the handshake (spec 7.1: msEnvelope with BE/LE flip retry,
 * else legacy single-byte query with a last CRC attempt). On success state is
 * CONNECTED and the ECU signature is available. LT_ENOTCONN if already...
 * returns LT_EBUSY when already connected. */
int  lt_proto_connect(lt_proto *p);
/* Handshake only (usable standalone by discovery): fills sig. */
int  lt_proto_handshake(lt_proto *p, char *sig, size_t cap);
void lt_proto_disconnect(lt_proto *p);       /* raises cancel flag, state DISCONNECTED (io stays open) */
/* True if the ECU signature differs from the INI's (modulo signature_prefix);
 * the GUI should offer to load a matching INI. */
lt_bool lt_proto_signature_mismatch(const lt_proto *p);

/* ---- timing (spec 6.3, 9) ---- */
int  lt_proto_effective_timeout_ms(const lt_proto *p);
int  lt_proto_effective_write_chunk(const lt_proto *p);        /* max(1, blockingFactor - 8) */
int  lt_proto_inter_frame_delay_ms(const lt_proto *p, size_t frame_len);

/* ---- pages (spec 9) ---- */
/* Read len bytes of `page` (page INDEX; the page identifier from the INI is
 * applied internally). Short or long replies are errors. */
int  lt_proto_read_memory(lt_proto *p, int page, size_t offset, size_t len, lt_buf *out);
/* Read a whole page in blockingFactor chunks into mem (state LOADING -> CLEAN). */
int  lt_proto_read_page(lt_proto *p, int page, lt_ecu_mem *mem);
/* Read every page that is NOT_LOADED (or all if force). Reports progress. */
int  lt_proto_read_all(lt_proto *p, lt_ecu_mem *mem, lt_bool force);
/* Write bytes into ECU RAM in (blockingFactor-8) chunks with pacing; auto-burns
 * the previous page when changing pages (cfg.auto_burn_on_page_change). */
int  lt_proto_write_memory(lt_proto *p, int page, size_t offset, const lt_u8 *data, size_t n);
/* Whole page write + CRC verify (verify_page_crc failures other than a mismatch only warn). */
int  lt_proto_write_page(lt_proto *p, int page, const lt_u8 *data, size_t n);
/* Legacy ECUs: write then read back and compare. Modern: ack suffices. */
int  lt_proto_write_memory_verified(lt_proto *p, int page, size_t offset, const lt_u8 *data,
                                    size_t n);
int  lt_proto_verify_page_crc(lt_proto *p, int page, const lt_u8 *expected, size_t n);
/* Send every dirty range of `mem` (via lt_ecu_mem_dirty_ranges) then
 * lt_ecu_mem_mark_pending(). Does not burn. Returns bytes written or <0. */
int  lt_proto_write_dirty(lt_proto *p, lt_ecu_mem *mem);
/* Burn (spec 9): refuses with LT_EBUSY if a page previously failed to write.
 * page < 0 = all pages written since the last burn (or page 0 if none).
 * Waits max(pageActivationDelay,2000)+600 ms. mem (optional) gets mark_burned(). */
int  lt_proto_burn(lt_proto *p, int page, lt_ecu_mem *mem);
size_t lt_proto_dirty_pages(const lt_proto *p, int *out, size_t cap);   /* written, not burned; ascending */

/* ---- realtime (spec 8) ---- */
typedef enum lt_rt_kind { LT_RT_KIND_BURST = 0, LT_RT_KIND_OCH } lt_rt_kind;
/* Which command lt_proto_get_realtime will use, with the reason (for the status bar). */
lt_rt_kind lt_proto_choose_runtime(const lt_proto *p, const char **reason);
/* One poll: raw output-channel block, status byte stripped, length == ochBlockSize
 * when declared. Feed to lt_ini_rt_update(). */
int  lt_proto_get_realtime(lt_proto *p, lt_buf *out);

/* ---- misc commands ---- */
int  lt_proto_send_raw(lt_proto *p, const lt_u8 *bytes, size_t n);                 /* controller commands, no reply */
int  lt_proto_send_raw_response(lt_proto *p, const lt_u8 *bytes, size_t n, int timeout_ms,
                                lt_buf *reply);
/* Execute a [ControllerCommands] entry (parts resolved/decoded, references followed, in order). */
int  lt_proto_run_command(lt_proto *p, const lt_ini_cmd *cmd);
/* rusEFI/FOME/epicEFI console line; reply text malloc'd into *out. LT_EUNSUPPORTED otherwise. */
int  lt_proto_console(lt_proto *p, const char *text, int timeout_ms, char **out);
/* Speeduino calibration upload (spec 11). table: LT_CAL_*; verifies via 'k' CRC when modern. */
int  lt_proto_write_calibration(lt_proto *p, int table, const lt_u8 *wire, size_t n);
int  lt_proto_read_calibration_crc(lt_proto *p, int table, lt_u32 *crc);
/* Tooth/composite logger commands (INI [LoggerDefinition]): start, read one block, stop. */
int  lt_proto_logger_start(lt_proto *p, const lt_ini_logger *lg);
int  lt_proto_logger_read(lt_proto *p, const lt_ini_logger *lg, lt_buf *out);
int  lt_proto_logger_stop(lt_proto *p, const lt_ini_logger *lg);

/* ====================================================================== */
/* Discovery / autodetect (spec 12)                                        */
/* ====================================================================== */
typedef enum lt_disc_kind {
    LT_DISC_SIGNATURE = 0, LT_DISC_BOOTLOADER, LT_DISC_BIGSTUFF3, LT_DISC_ECHO,
    LT_DISC_GPS, LT_DISC_UNKNOWN, LT_DISC_SILENT, LT_DISC_OPEN_FAILED
} lt_disc_kind;
typedef struct lt_disc_result {
    char port[128]; long baud; lt_disc_kind kind; char query;
    char signature[96];        /* SIGNATURE */
    char error[128];           /* OPEN_FAILED */
} lt_disc_result;
/* Opens a port for the sweep (injectable for tests). Fills *io; returns 0 or <0. */
typedef int (*lt_disc_open_fn)(void *ud, const char *port, long baud, lt_proto_io *io);
typedef struct lt_disc_cfg {
    const long *bauds; size_t n_bauds;     /* default {115200,921600,460800,230400,57600,9600} */
    const char *queries;                   /* default "QS" */
    int post_open_settle_ms;               /* 1500 */
    int pre_write_delay_ms;                /* 40 */
    int read_window_ms;                    /* 600 */
    lt_disc_open_fn open; void *open_ud;   /* NULL = lt_serial_open adapter */
    lt_proto_clock clock;                  /* zeroed = plat */
    volatile int *cancel;                  /* optional */
} lt_disc_cfg;
void lt_disc_cfg_default(lt_disc_cfg *c);
/* Classify a raw reply (spec 12). */
lt_disc_kind lt_disc_classify(const lt_u8 *bytes, size_t n, char query, char *sig, size_t sigcap);
/* Probe ports (in list order, stopping at the first Signature/Bootloader/BigStuff3 per port).
 * results: malloc'd array of *n entries (free with lt_free). Returns 0 or <0. */
int  lt_disc_run(const lt_disc_cfg *cfg, const char *const *ports, size_t n_ports,
                 lt_disc_result **results, size_t *n);
/* Best candidate (Signature > Bootloader > BigStuff3) or NULL. */
const lt_disc_result *lt_disc_best(const lt_disc_result *r, size_t n);
/* Serial port enumeration sorted ACM<n>, USB<n>, others (spec 6.1); names malloc'd (lt_plat_free_list). */
int  lt_proto_list_ports(char ***names);
void lt_ports_sort(char **names, int n);

#endif /* LT_PROTO_H */
