/* plat.h - OS abstraction: the universal interface to everything that C89
 * cannot do (serial ports, sockets, directories, raw terminal, clocks).
 *
 * The interface is universal; the implementations are per-platform files
 * chosen by the BUILD SYSTEM (tools/target.sh), never by #ifdef:
 *   src/plat/plat_common.c   universal parts shared by every platform
 *   src/plat/posix/          Linux / BSD / macOS (termios, BSD sockets, select)
 *     os_linux.c os_bsd.c os_macos.c  - exactly one is built (OS identity,
 *                                       config dirs, exe path, device names)
 *   src/plat/win32/          Windows (Win32 serial, winsock, console)
 * Single-threaded design: everything is polled with timeouts.
 * See docs/UNIVERSAL_C.md section 11.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PLAT_H
#define LT_PLAT_H

#include "../lt.h"

LT_BEGIN_DECLS

/* ---- OS identity (fixed per build) ---- */
typedef enum lt_os { LT_OS_UNIX = 0, LT_OS_LINUX, LT_OS_MACOS, LT_OS_BSD, LT_OS_WINDOWS } lt_os;
lt_os lt_plat_os(void);
const char *lt_plat_os_name(void);

/* ---- time ---- */
/* monotonic milliseconds since an arbitrary epoch (wraps never in practice) */
double lt_plat_now_ms(void);
void   lt_plat_sleep_ms(int ms);
/* Virtual clock for reproducible offscreen renders (screenshot harness, tests): while on,
 * lt_plat_now_ms returns a counter that starts at the current time, moves 10 us per read
 * (so deadline loops still end) and lt_plat_sleep_ms advances it instead of sleeping. */
void   lt_plat_clock_virtual(int on);
lt_bool lt_plat_clock_is_virtual(void);
/* wall clock "YYYY-MM-DD HH:MM:SS" local time into buf (>= 20 bytes) */
void   lt_plat_timestamp(char *buf, size_t cap);

/* ---- filesystem ---- */
int   lt_plat_mkdir(const char *path);      /* 0 ok or already exists */
int   lt_plat_mkdirs(const char *path);     /* recursive */
lt_bool lt_plat_is_dir(const char *path);
int   lt_plat_remove(const char *path);     /* file or empty dir */
int   lt_plat_rename(const char *from, const char *to);
/* restrict a file to the current user (POSIX mode 0600; no-op where the OS has no such mode) */
int   lt_plat_chmod_private(const char *path);
int   lt_plat_copy_file(const char *from, const char *to);
/* Replace the file at `path` with len bytes, atomically and durably: the data is written to a
 * new temporary file in the same directory, flushed to the disk (fsync / FlushFileBuffers) and
 * renamed over `path` (the directory entry is synced too where the OS allows). Readers and a
 * crash or power loss at any point see either the complete old file or the complete new one,
 * never a truncated mix; on failure the temporary file is removed and `path` is untouched.
 * The existing file's permission bits are kept. 0 ok, -1 with lt_err. */
int   lt_plat_write_atomic(const char *path, const void *data, size_t len);
/* Fault injection for tests: lt_plat_write_atomic fails like a full disk once `bytes` more
 * bytes have been written in total. Negative (the default) = no limit. */
void  lt_plat_fault_write_limit(long bytes);
/* list directory entries (excluding . and ..): returns count, *names is a
 * malloc'd array of malloc'd strings, sorted ascii; free with lt_plat_free_list */
int   lt_plat_list_dir(const char *path, char ***names);
void  lt_plat_free_list(char **names, int n);
/* user config dir (e.g. ~/.config/libretune, %APPDATA%\LibreTune,
 * ~/Library/Application Support/LibreTune); malloc'd, created if missing */
char *lt_plat_config_dir(void);
/* LIBRETUNE_CONFIG_DIR, when set and non-empty, replaces the per-OS config dir on every
 * platform (portable installs, tests). Returns a malloc'd, created dir or NULL if unset;
 * each lt_plat_config_dir implementation checks it first. */
char *lt_plat_config_override(void);
/* user documents dir for projects (e.g. ~/LibreTune Projects); malloc'd */
char *lt_plat_projects_dir(void);
char *lt_plat_getenv(const char *name);    /* malloc'd copy or NULL */
/* set an environment variable for this process (value copied); 0 ok */
int   lt_plat_setenv(const char *name, const char *value);
/* user home directory (never NULL; falls back to "."); malloc'd */
char *lt_plat_home_dir(void);
/* fill buf with n bytes from the operating system's secure random source
 * (/dev/urandom, RtlGenRandom); 0 ok, -1 if unavailable */
int   lt_plat_random(void *buf, size_t n);
/* path to the running executable or NULL */
char *lt_plat_exe_path(void);

/* ---- byte streams (serial ports and TCP sockets) ---- */
typedef struct lt_stream lt_stream;
/* serial: name like "/dev/ttyUSB0", "/dev/cu.usbmodem1", "COM3".
 * 8N1, no flow control, raw. */
lt_stream *lt_serial_open(const char *port, long baud);
int  lt_serial_set_baud(lt_stream *s, long baud);
/* Drive the modem control lines of a serial port: dtr/rts 1 = asserted, 0 = cleared
 * (Arduino-style boards reset on the DTR/RTS edge). 0 ok, -1 when the stream is not a
 * serial port or the driver has no modem lines (pseudo-terminals). */
int  lt_serial_set_lines(lt_stream *s, int dtr, int rts);
/* list candidate serial ports; returns count, names malloc'd (free_list) */
int  lt_serial_list(char ***names);
/* tcp client connect with timeout */
lt_stream *lt_tcp_connect(const char *host, int port, int timeout_ms);
/* Read up to n bytes waiting at most timeout_ms (0 = nonblocking).
 * Returns bytes read (>=0) or -1 on error/closed. */
int  lt_stream_read(lt_stream *s, void *buf, int n, int timeout_ms);
/* Read exactly n bytes or fail after total timeout. Returns n or <n on timeout, -1 err. */
int  lt_stream_read_full(lt_stream *s, void *buf, int n, int timeout_ms);
/* Write all bytes; returns n or -1 */
int  lt_stream_write(lt_stream *s, const void *buf, int n);
void lt_stream_flush_input(lt_stream *s);   /* discard pending input */
void lt_stream_close(lt_stream *s);

/* ---- TCP server (used by the web GUI backend) ---- */
typedef struct lt_server lt_server;
/* listen on 127.0.0.1:port (port 0 picks any); returns NULL on failure */
lt_server *lt_server_listen(int port);
int  lt_server_port(lt_server *srv);
/* accept a client if one is pending within timeout_ms; NULL if none */
lt_stream *lt_server_accept(lt_server *srv, int timeout_ms);
void lt_server_close(lt_server *srv);

/* Wait until any of the given streams/servers is readable or timeout.
 * Pass NULL-terminated arrays (either may be NULL). Returns >0 if something
 * is readable, 0 on timeout. Also returns early if extra_fd (>=0) is readable
 * (used by GUI backends that own a socket, e.g. X11 connection, or stdin). */
int  lt_plat_wait(lt_stream **streams, lt_server **servers, int extra_fd, int timeout_ms);
/* raw fd of a stream (POSIX) or -1 */
int  lt_stream_fd(lt_stream *s);

/* unix domain socket connect (X11). NULL if unsupported. */
lt_stream *lt_unix_connect(const char *path);

/* ---- shared memory (X11 MIT-SHM) ---- */
/* A System V shared memory segment of `size` bytes, attached read-write in this process and
 * created with owner-only permissions, so a local X server can attach the same memory and
 * read images from it without a copy through the socket. NULL where the OS has no SysV shared
 * memory (Windows) or on failure (limits, sandbox). */
typedef struct lt_shm lt_shm;
lt_shm *lt_shm_create(size_t size);
void   *lt_shm_addr(lt_shm *m);
size_t  lt_shm_size(lt_shm *m);
long    lt_shm_id(lt_shm *m);          /* the id another process attaches (shmid) */
/* mark the segment for removal once every process has detached (IPC_RMID): call it after the
 * peer attached, so the memory is reclaimed even if this process crashes */
void    lt_shm_unlink(lt_shm *m);
void    lt_shm_destroy(lt_shm *m);     /* detach; removes it too when not unlinked yet */

/* ---- terminal (tty GUI backend) ---- */
lt_bool lt_term_isatty(void);
int  lt_term_raw_begin(void);   /* raw mode, no echo, mouse reporting off (backend enables) */
void lt_term_raw_end(void);
void lt_term_size(int *cols, int *rows);
/* read available stdin bytes with timeout; returns count, 0 timeout, -1 eof */
int  lt_term_read(char *buf, int n, int timeout_ms);
void lt_term_write(const char *buf, int n);
lt_bool lt_term_resized(void);  /* SIGWINCH latch; clears on read */

/* ---- misc ---- */
/* open URL in the default browser (xdg-open / open / start). 0 ok */
int  lt_plat_open_url(const char *url);

LT_END_DECLS

#endif /* LT_PLAT_H */
