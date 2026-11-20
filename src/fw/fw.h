/* fw.h - firmware images and the flashing engines (Tools > Firmware Update).
 *
 *   images   raw .bin (at a base address), Intel HEX, Motorola S-record and ST DfuSe .dfu
 *            files parsed into sorted, non-overlapping address/data segments
 *   engines  ST DfuSe 1.1a (STM32 ROM bootloader, 0483:df11) and OpenBLT "XCP on USB bulk"
 *            (1D50:60AC). Both are stepped state machines: lt_fw_step() does at most one USB
 *            transaction (a few during setup) and returns, so the UI loop stays responsive.
 *   transport  a struct of function pointers (lt_fw_io): fw_usb.c maps it onto plat/usb.h,
 *            fw_sim.c onto simulated devices (tests and the demo).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FW_H
#define LT_FW_H

#include "../lt.h"
#include "../plat/usb.h"

LT_BEGIN_DECLS

/* ------------------------------------------------------------------ images */
#define LT_FW_MAX_DATA     (32UL * 1024UL * 1024UL)  /* data bytes one image may hold */
#define LT_FW_STM32_FLASH  ((lt_u32)0x08000000UL)               /* default base of a raw .bin */

enum { LT_FW_FMT_UNKNOWN = 0, LT_FW_FMT_BIN, LT_FW_FMT_IHEX, LT_FW_FMT_SREC, LT_FW_FMT_DFUSE };

typedef struct lt_fw_seg {
    lt_u32 addr;
    lt_u32 len;
    lt_u8 *data;
    size_t cap;
} lt_fw_seg;

typedef struct lt_fw_image {
    int format;                 /* LT_FW_FMT_* the data came from */
    lt_fw_seg *segs;            /* sorted by address, merged, never overlapping (after finish) */
    size_t n_segs, cap_segs;
    lt_u32 goal;                /* data bytes */
    lt_u32 lo, last;            /* lowest and highest (inclusive) address, when n_segs > 0 */
    lt_u32 entry;               /* start address record (hex 05/03, S7/S8/S9) */
    int has_entry;
    /* DfuSe files */
    int dfu_alt;                /* alternate setting of the target used */
    lt_u16 dfu_vid, dfu_pid;    /* suffix ids (0xFFFF = any) */
    char dfu_target[64];
} lt_fw_image;

void lt_fw_image_init(lt_fw_image *img);
void lt_fw_image_free(lt_fw_image *img);
/* append n bytes at addr (contiguous data extends the previous segment); 0 or -1 with err */
int  lt_fw_image_add(lt_fw_image *img, lt_u32 addr, const lt_u8 *data, size_t n, char *err, size_t errcap);
/* sort, reject overlaps, merge touching segments, compute lo/last/goal; 0 or -1 with err */
int  lt_fw_image_finish(lt_fw_image *img, char *err, size_t errcap);
/* parse a file image; format LT_FW_FMT_UNKNOWN = detect from the content. bin_base is the
 * address of a raw binary. Returns 0, or -1 with a one-line reason in err. img is (re)initialised. */
int  lt_fw_image_parse(lt_fw_image *img, const lt_u8 *data, size_t len, int format, lt_u32 bin_base,
                       char *err, size_t errcap);
int  lt_fw_image_load(lt_fw_image *img, const char *path, lt_u32 bin_base, char *err, size_t errcap);
/* format implied by a file name (.bin .hex .ihex .srec .s19 .s28 .s37 .mot .dfu), else UNKNOWN */
int  lt_fw_format_from_name(const char *path);
const char *lt_fw_format_name(int format);
/* copy n bytes at addr into out, gaps filled with `fill`; returns the number of image bytes */
lt_u32 lt_fw_image_read(const lt_fw_image *img, lt_u32 addr, lt_u8 *out, lt_u32 n, lt_u8 fill);
/* 1 if any image byte lies in [addr, addr + len) */
int  lt_fw_image_touches(const lt_fw_image *img, lt_u32 addr, lt_u32 len);

/* writers (tests, round trips): append the encoded file to out */
void lt_fw_write_ihex(const lt_fw_image *img, lt_buf *out);
void lt_fw_write_srec(const lt_fw_image *img, lt_buf *out);
void lt_fw_write_dfuse(const lt_fw_image *img, lt_u16 vid, lt_u16 pid, int alt, const char *target,
                       lt_buf *out);
/* DfuSe suffix CRC (CRC-32, init 0xFFFFFFFF, no final xor) over n bytes */
lt_u32 lt_fw_dfu_crc(const lt_u8 *p, size_t n);

/* cheap facts about STM32 / rusEFI images (vector tables, OpenBLT app header) */
#define LT_FW_RUSEFI_APP  ((lt_u32)0x08008000UL)   /* rusEFI app behind OpenBLT */
#define LT_FW_RUSEFI_CFG  ((lt_u32)0x080C0000UL)   /* rusEFI F4 settings: flash sectors 10 and 11 */
#define LT_FW_RUSEFI_CFG_LEN ((lt_u32)0x40000UL)
typedef struct lt_fw_facts {
    int vectors;                /* -1 no data at the lowest address, 0 implausible, 1 plausible */
    lt_u32 sp, reset;           /* first vector table of the image */
    int merged_openblt;         /* bootloader at 0x08000000 and an app at 0x08008000 */
    int app_vectors;            /* app vector table: -1 none, 0 bad, 1 ok */
    int app_crc;                /* rusEFI OpenBLT app CRC header: -1 n/a, 0 mismatch, 1 ok */
    lt_u32 app_size;
    int n_warn;
    char warn[4][160];
} lt_fw_facts;
void lt_fw_image_facts(const lt_fw_image *img, lt_fw_facts *f);
/* one-line summary "Intel HEX, 2 segments, 712.5 KB, 0x08000000-0x080AE1FF" */
void lt_fw_image_summary(const lt_fw_image *img, char *out, size_t cap);

/* ------------------------------------------------------------------ board presets */
typedef struct lt_fw_range { lt_u32 addr, len; const char *what; } lt_fw_range;
typedef struct lt_fw_preset {
    const char *name;
    lt_u32 bin_base;            /* address of a raw .bin */
    lt_u32 app_base;            /* OpenBLT: application start (image bytes below = bootloader), 0 none */
    const lt_fw_range *protect; /* kept while "keep ECU settings" is on */
    int n_protect;
    int dfu, openblt, wideband; /* what the screen offers */
    const char *url;            /* where to get images, or NULL */
} lt_fw_preset;
enum { LT_FW_PRESET_MIATA = 0, LT_FW_PRESET_RUSEFI, LT_FW_PRESET_GENERIC, LT_FW_PRESET_COUNT };
const lt_fw_preset *lt_fw_preset_get(int idx);
/* preset matching an ECU signature (miata-ecu -> MIATA, rusEFI/FOME/epicEFI -> RUSEFI, else GENERIC) */
int lt_fw_preset_for_signature(const char *sig);

/* ------------------------------------------------------------------ transport */
/* All return a byte count >= 0 or an LT_USB_E* code (plat/usb.h). */
typedef struct lt_fw_io {
    void *ctx;
    int (*control)(void *ctx, lt_u8 rtype, lt_u8 req, lt_u16 value, lt_u16 index, lt_u8 *data,
                   lt_u16 len, int timeout_ms);
    int (*bulk)(void *ctx, lt_u8 ep, lt_u8 *data, int len, int timeout_ms);
    int (*get_config)(void *ctx, lt_u8 *buf, int cap);
    int (*get_string)(void *ctx, int index, char *buf, size_t cap);
    int (*claim)(void *ctx, int iface, int alt);   /* claim; alt >= 0 also selects it */
} lt_fw_io;

/* ------------------------------------------------------------------ DfuSe helpers */
#define LT_FW_DFU_VID 0x0483u
#define LT_FW_DFU_PID 0xDF11u
#define LT_FW_MAX_REGIONS 16
typedef struct lt_fw_region {
    lt_u32 addr;                /* first sector */
    lt_u32 count, size;         /* sectors of `size` bytes */
    int type;                   /* bit0 readable, bit1 erasable, bit2 writable */
} lt_fw_region;
/* "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg": regions (count) or -1 */
int lt_fw_dfuse_layout(const char *s, lt_fw_region *out, int cap, char *name, size_t namecap);
typedef struct lt_fw_dfu_alt {
    int iface, alt, iface_str;       /* interface number, alternate setting, iInterface */
    int xfer;                   /* wTransferSize of the functional descriptor (0 = none seen) */
    int dfuse;                  /* bcdDFUVersion 0x011A */
} lt_fw_dfu_alt;
/* DFU-mode interfaces (class 0xFE subclass 1) of a configuration descriptor; count */
int lt_fw_dfuse_alts(const lt_u8 *cfg, int len, lt_fw_dfu_alt *out, int cap);
/* Sectors to erase for img (start addresses and sizes into addr/size, count returned), or -1
 * with err when the image leaves the map, hits a read-only area or a protected range. */
int lt_fw_dfuse_plan(const lt_fw_region *rg, int n_rg, const lt_fw_image *img,
                     const lt_fw_range *protect, int n_protect, lt_u32 *addr, lt_u32 *size, int cap,
                     char *err, size_t errcap);

/* ------------------------------------------------------------------ OpenBLT */
#define LT_FW_OPENBLT_VID 0x1D50u
#define LT_FW_OPENBLT_PID 0x60ACu

/* ------------------------------------------------------------------ flashing jobs */
enum { LT_FW_DFUSE = 0, LT_FW_OPENBLT = 1 };
enum { LT_FW_PH_IDLE = 0, LT_FW_PH_CONNECT, LT_FW_PH_ERASE, LT_FW_PH_WRITE, LT_FW_PH_VERIFY,
       LT_FW_PH_LEAVE, LT_FW_PH_DONE, LT_FW_PH_FAILED, LT_FW_PH_CANCELLED };

typedef struct lt_fw_opts {
    int verify;                 /* read back and compare */
    int leave;                  /* start the firmware when done */
    int keep_settings;          /* refuse to touch `protect` ranges */
    int mass_erase;             /* DfuSe: mass erase instead of sectors (refused with protect) */
    lt_u32 app_base;            /* OpenBLT: skip image bytes below this (bootloader), 0 = none */
    const lt_fw_range *protect;
    int n_protect;
} lt_fw_opts;

typedef struct lt_fw_progress {
    int phase;                  /* LT_FW_PH_* */
    lt_u32 done, goal;          /* bytes of the current phase */
    double frac;                /* whole job 0..1 */
    int usb_err;                /* last LT_USB_E* seen (0 none): permission/driver hints */
    char msg[200];              /* current activity or the final result */
} lt_fw_progress;

/* level: 0 info, 1 success, 2 warning, 3 error */
typedef void (*lt_fw_log_fn)(void *user, int level, const char *msg);
typedef struct lt_fw_job lt_fw_job;

/* The job keeps pointers to io and img: both must outlive it. NULL on bad arguments. */
lt_fw_job *lt_fw_job_new(int method, const lt_fw_io *io, const lt_fw_image *img, const lt_fw_opts *o,
                         lt_fw_log_fn log, void *user);
void lt_fw_job_free(lt_fw_job *j);
/* one step; returns 1 while running, 0 when finished (phase DONE, FAILED or CANCELLED) */
int  lt_fw_step(lt_fw_job *j, double now_ms);
/* earliest time the job wants the next step (<= now: step right away) */
double lt_fw_next_ms(const lt_fw_job *j);
/* stop at the next safe point (never in the middle of a flash operation) */
void lt_fw_cancel(lt_fw_job *j);
void lt_fw_job_progress(const lt_fw_job *j, lt_fw_progress *p);
const char *lt_fw_phase_name(int phase);

/* ------------------------------------------------------------------ USB adapter (fw_usb.c) */
typedef struct lt_fw_found {
    int method;                 /* LT_FW_DFUSE / LT_FW_OPENBLT */
    int sim;                    /* 1 = simulated device (demo) */
    lt_u16 vid, pid;
    char path[256];
    char label[128];            /* "STM32 BOOTLOADER 0483:df11" */
} lt_fw_found;
/* Bootloader devices attached (DfuSe and OpenBLT); count or LT_USB_E* (ENOTSUP: no USB in
 * this build). */
int lt_fw_scan(lt_fw_found *out, int cap);
typedef struct lt_fw_usb lt_fw_usb;
/* open a device and fill io; NULL with a reason in err */
lt_fw_usb *lt_fw_usb_open(const char *path, lt_fw_io *io, char *err, size_t errcap);
void lt_fw_usb_close(lt_fw_usb *u);
/* what to tell the user about a USB error code (udev rule, WinUSB/Zadig, ...) */
const char *lt_fw_usb_hint(int code, int os_windows);
extern const char lt_fw_udev_rule[];

/* ------------------------------------------------------------------ simulated devices (fw_sim.c) */
enum { LT_FW_SIM_DFUSE = 0, LT_FW_SIM_OPENBLT = 1 };
typedef struct lt_fw_sim_opts {
    int kind;
    lt_u32 erase_ms_16k;        /* DfuSe poll timeout per 16 KB of sector erased */
    lt_u32 write_ms;            /* poll timeout per data block */
    int stall_dnload;           /* the Nth DNLOAD stalls (EPIPE, then dfuERROR), 0 never */
    int err_status;             /* the Nth busy-ending GETSTATUS reports errPROG, 0 never */
    int err_repeat;             /* ... and keeps failing that many times in a row (>= 1) */
    int pgm_protected;          /* OpenBLT: programming needs seed/key */
    int connect_ignore;         /* OpenBLT: first N CONNECTs get no answer */
    int busy_ms;                /* OpenBLT: response delay of PROGRAM_CLEAR */
    int max_cto;                /* OpenBLT: 0 = 64 */
} lt_fw_sim_opts;
typedef struct lt_fw_sim lt_fw_sim;
typedef double (*lt_fw_clock_fn)(void *user);
lt_fw_sim *lt_fw_sim_new(const lt_fw_sim_opts *o, lt_fw_clock_fn clock, void *clock_user);
void lt_fw_sim_free(lt_fw_sim *s);
void lt_fw_sim_io(lt_fw_sim *s, lt_fw_io *io);
/* simulated flash (1 MB at 0x08000000, STM32F407 sectors) */
lt_u8 *lt_fw_sim_flash(lt_fw_sim *s, lt_u32 *base, lt_u32 *size);
/* counters for tests: early GETSTATUS polls (bwPollTimeout not honoured), erased sectors,
 * device left the bootloader */
int lt_fw_sim_early_polls(const lt_fw_sim *s);
int lt_fw_sim_erases(const lt_fw_sim *s);
int lt_fw_sim_left(const lt_fw_sim *s);
/* deterministic demo/test image in the rusEFI layout: OpenBLT-like loader at 0x08000000
 * (when with_loader) and an app with a valid CRC header at 0x08008000 of app_kb kilobytes */
void lt_fw_sim_demo_image(lt_fw_image *img, int with_loader, lt_u32 app_kb);

LT_END_DECLS

#endif /* LT_FW_H */
