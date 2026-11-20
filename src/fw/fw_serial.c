/* fw_serial.c - the flashing transport on top of a plat.h serial port (STK500v2, OpenBLT over a
 * UART or an SLCAN adapter): write / read with a timeout / DTR+RTS, plus the user-facing hint
 * when a port does not open.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw.h"
#include "../plat/plat.h"

struct lt_fw_serial { lt_stream *s; };

static int s_write(void *ctx, const lt_u8 *data, int len)
{
    lt_fw_serial *p = (lt_fw_serial *)ctx;
    if (len <= 0) { return 0; }
    return lt_stream_write(p->s, data, len) == len ? len : LT_USB_EIO;
}

static int s_read(void *ctx, lt_u8 *buf, int cap, int timeout_ms)
{
    lt_fw_serial *p = (lt_fw_serial *)ctx;
    int r = lt_stream_read(p->s, buf, cap, timeout_ms);
    return r < 0 ? LT_USB_EIO : r;
}

static int s_lines(void *ctx, int dtr, int rts)
{
    lt_fw_serial *p = (lt_fw_serial *)ctx;
    return lt_serial_set_lines(p->s, dtr, rts) == 0 ? 0 : LT_USB_ENOTSUP;
}

lt_fw_serial *lt_fw_serial_open(const char *port, long baud, lt_fw_io *io, char *err, size_t errcap)
{
    lt_fw_serial *p;
    lt_stream *s;
    if (port == NULL || port[0] == '\0') {
        if (err != NULL && errcap > 0) { lt_strlcpy(err, "no serial port selected", errcap); }
        return NULL;
    }
    s = lt_serial_open(port, baud);
    if (s == NULL) {
        if (err != NULL && errcap > 0) { lt_strlcpy(err, lt_err_get(), errcap); }
        return NULL;
    }
    p = (lt_fw_serial *)lt_calloc(1, sizeof(*p));
    if (p == NULL) {
        lt_stream_close(s);
        if (err != NULL && errcap > 0) { lt_strlcpy(err, "out of memory", errcap); }
        return NULL;
    }
    p->s = s;
    memset(io, 0, sizeof(*io));
    io->ctx = p;
    io->write = s_write;
    io->read = s_read;
    io->set_lines = s_lines;
    return p;
}

void lt_fw_serial_close(lt_fw_serial *p)
{
    if (p == NULL) { return; }
    lt_stream_close(p->s);
    lt_free(p);
}

static int has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay != '\0'; hay++) {
        if (lt_strnicmp(hay, needle, n) == 0) { return 1; }
    }
    return 0;
}

const char *lt_fw_serial_hint(const char *err, int os_windows)
{
    if (err == NULL) { return NULL; }
    if (has_ci(err, "permission") || has_ci(err, "denied") || has_ci(err, "(5)")) {
        return os_windows
            ? "Windows refused the port: close other programs that use it (Arduino IDE, SpeedyLoader, "
              "TunerStudio) and try again."
            : "No permission to open the serial port. On most Linux systems: sudo usermod -aG dialout $USER "
              "(uucp on Arch), then log out and in again.";
    }
    if (has_ci(err, "busy") || has_ci(err, "(32)")) {
        return "Another program holds the serial port (Arduino IDE, SpeedyLoader, TunerStudio, a serial "
               "monitor): close it and try again.";
    }
    if (has_ci(err, "no such") || has_ci(err, "(2)")) {
        return "The port is gone: check the cable and pick the port again (it may change after a reset).";
    }
    return NULL;
}
