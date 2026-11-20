/* ports_logic.h - finding the ECU on a serial port, without UI: which ports to try first and
 * what to call them (USB VID:PID and product), the devices that worked before (per USB serial
 * number), and a probe that asks each candidate for its signature without ever blocking (the
 * caller steps it from the UI loop). Used by the Connect dialog, the Connect ECU wizard, the
 * start screen and automatic reconnect; unit-tested in tests/test_ports.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PORTS_LOGIC_H
#define LT_PORTS_LOGIC_H

#include "../lt.h"
#include "../plat/plat.h"
#include "../proto/proto.h"

/* ---- what is behind a port ---- */
enum {
    LT_ECUPORT_STM32 = 0,      /* 0483:5740 STM32 virtual COM port: rusEFI, FOME, epicEFI */
    LT_ECUPORT_ARDUINO,        /* Arduino Mega 2560: Speeduino */
    LT_ECUPORT_TEENSY,         /* Teensy USB serial: Speeduino on Teensy 3.5/3.6/4.1 */
    LT_ECUPORT_ADAPTER,        /* USB-serial bridge (CH340, FTDI, CP210x, PL2303): any ECU */
    LT_ECUPORT_CDC,            /* other USB CDC device (ttyACM, cu.usbmodem) */
    LT_ECUPORT_USB,            /* other USB serial device */
    LT_ECUPORT_OTHER,          /* not known to be USB */
    LT_ECUPORT_LEGACY          /* on-board UART (/dev/ttyS*, COM1 of the Serial driver) */
};

#define LT_ECUPORT_MAX_BAUDS 6

typedef struct lt_ecuport {
    lt_serial_info info;
    int  kind;                 /* LT_ECUPORT_* */
    int  score;                /* higher is tried first */
    int  remembered;           /* the device (or port) worked before */
    char label[96];            /* "STM32 USB (rusEFI, FOME, epicEFI)" */
    char ids[24];              /* "0483:5740" or "" */
    long bauds[LT_ECUPORT_MAX_BAUDS];   /* likeliest first */
    int  n_bauds;
    int  settle_ms;            /* wait after opening (boards that reset on open need ~1.5 s) */
} lt_ecuport;

/* Describe one port (no I/O). */
void lt_ecuport_describe(const lt_serial_info *in, lt_ecuport *out);
/* Short family for a kind: "rusEFI / FOME / epicEFI", "Speeduino", "" (unknown). */
const char *lt_ecuport_family(int kind);
/* The key a device is remembered under: "usb:VVVV:PPPP:<serial>" when it has a serial
 * number, else "port:<name>". */
void lt_ecuport_key(const lt_serial_info *in, char *key, size_t cap);

/* ---- devices that worked before (config dir, devices.conf) ---- */
typedef struct lt_ecuport_seen {
    char key[112];
    long baud;
    char port[128];            /* where it was last */
    char signature[96];
    char project[512];         /* project directory it was last used with, or "" */
} lt_ecuport_seen;
typedef struct lt_ecuport_mem {
    lt_ecuport_seen *e;        /* newest first */
    int n;
} lt_ecuport_mem;
void lt_ecuport_mem_init(lt_ecuport_mem *m);
void lt_ecuport_mem_free(lt_ecuport_mem *m);
int  lt_ecuport_mem_load(lt_ecuport_mem *m, const char *config_dir);   /* missing file = empty */
int  lt_ecuport_mem_save(const lt_ecuport_mem *m, const char *config_dir);
/* entry for this device (by USB serial number, else by port name) or NULL */
const lt_ecuport_seen *lt_ecuport_mem_find(const lt_ecuport_mem *m, const lt_serial_info *in);
/* remember a good connection (moves it to the front; keeps at most 32) */
void lt_ecuport_mem_note(lt_ecuport_mem *m, const lt_serial_info *in, long baud,
                         const char *signature, const char *project);

/* ---- which ports to try ---- */
/* Describe and sort the ports, best first: devices that worked before, known ECU boards,
 * USB-serial bridges and other USB devices, then the rest. Legacy on-board UARTs are left
 * out unless show_all (their count goes to *n_hidden). A remembered device gets its last
 * baud rate first. Returns the count; *out malloc'd (lt_free, NULL when 0). mem may be NULL. */
int  lt_ecuport_rank(const lt_serial_info *in, int n, int show_all, const lt_ecuport_mem *mem,
                     lt_ecuport **out, int *n_hidden);
/* Convenience: lt_serial_list_info + lt_ecuport_rank. */
int  lt_ecuport_list(int show_all, const lt_ecuport_mem *mem, lt_ecuport **out, int *n_hidden);
/* The likeliest ECU among the ports (a known ECU board, or a remembered device), or -1. */
int  lt_ecuport_best_ecu(const lt_ecuport *p, int n);
/* The port that is the same device as `dev` (seen earlier, maybe under another name after a
 * replug): same USB id and serial number; without a serial number the same USB id and port
 * name, or the same name for a non-USB port. -1 when it is not there. */
int  lt_ecuport_find_same(const lt_ecuport *p, int n, const lt_serial_info *dev);

/* ---- probing without blocking ---- */
/* Opens a port (injectable for tests): fills *io and returns 0, or an LT_SERIAL_E* code. */
typedef int (*lt_ecuport_open_fn)(void *ud, const char *port, long baud, lt_proto_io *io);

typedef struct lt_ecuport_scan lt_ecuport_scan;
enum { LT_ECUSCAN_RUNNING = 0, LT_ECUSCAN_FOUND = 1, LT_ECUSCAN_DONE = 2 };
typedef struct lt_ecuport_hit {
    char port[128];
    long baud;
    char signature[96];
    int  index;                /* into the ports the scan was made from */
} lt_ecuport_hit;

/* Probe ports[0..n) in order, each at its baud rates, asking 'Q' then 'S' (spec 12). The
 * ports are copied. */
lt_ecuport_scan *lt_ecuport_scan_new(const lt_ecuport *ports, int n);
void lt_ecuport_scan_free(lt_ecuport_scan *s);
void lt_ecuport_scan_set_open(lt_ecuport_scan *s, lt_ecuport_open_fn fn, void *ud);
/* Do whatever is due at now_ms (open, send, collect a reply) and return at once:
 * LT_ECUSCAN_RUNNING, FOUND (the port is closed again; see _hit) or DONE (nothing found). */
int  lt_ecuport_scan_step(lt_ecuport_scan *s, double now_ms);
/* stop: the open port is closed, the scan reports DONE */
void lt_ecuport_scan_cancel(lt_ecuport_scan *s);
double lt_ecuport_scan_progress(const lt_ecuport_scan *s);           /* 0..1 */
/* "Asking STM32 USB (rusEFI, FOME, epicEFI) on /dev/ttyACM0 at 115200 baud" */
void lt_ecuport_scan_status(const lt_ecuport_scan *s, char *buf, size_t cap);
const lt_ecuport_hit *lt_ecuport_scan_hit(const lt_ecuport_scan *s);   /* NULL unless FOUND */
/* Why nothing was found, in words a non-expert can act on: a short title and a paragraph
 * (port busy, no permission with the exact command, bootloader mode, no answer, wrong baud).
 * n_hidden: legacy ports left out of the scan (mentioned when nothing else was tried). */
void lt_ecuport_scan_report(const lt_ecuport_scan *s, int n_hidden, char *title, size_t tcap,
                            char *msg, size_t mcap);
/* The same advice for a single failed lt_serial_open (code LT_SERIAL_E*). */
void lt_ecuport_open_advice(int code, const char *port, char *msg, size_t cap);

#endif
