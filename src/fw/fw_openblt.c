/* fw_openblt.c - OpenBLT bootloader host, XCP 1.0 over three transports:
 *   USB   (1D50:60AC) interface 0, bulk OUT 0x01 / IN 0x81; every packet is one length byte
 *         followed by the XCP command or response;
 *   UART  "XCP on RS232": the same length byte + packet framing (no checksum byte, the
 *         OpenBLT default) over a serial port;
 *   CAN   one XCP packet per CAN frame (DLC = packet length, at most 8 bytes) through an
 *         SLCAN / Lawicel ASCII adapter on a serial port ("Sn" bit rate, "O" open,
 *         "tIIILDD..<CR>" frames, 29-bit ids as "T"); default ids 0x667 host -> target and
 *         0x7E1 target -> host.
 * Responses start with 0xFF = positive or 0xFE = error + code.
 * Sequence: CONNECT, GET_STATUS (programming must not need a seed/key unlock), PROGRAM_START;
 * per segment SET_MTA + PROGRAM_CLEAR (erase); per segment SET_MTA + PROGRAM_MAX / PROGRAM
 * chunks; PROGRAM with length 0 ends programming; optional read back with SET_MTA + UPLOAD;
 * PROGRAM_RESET starts the firmware. Image bytes below the application start (the OpenBLT
 * bootloader of a merged rusEFI image) are never sent.
 * Responses are polled with short bulk reads so a long erase does not block the UI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw_priv.h"

#define CMD_CONNECT       0xFF
#define CMD_GET_STATUS    0xFD
#define CMD_SET_MTA       0xF6
#define CMD_UPLOAD        0xF5
#define CMD_PROGRAM_START 0xD2
#define CMD_PROGRAM_CLEAR 0xD1
#define CMD_PROGRAM       0xD0
#define CMD_PROGRAM_RESET 0xCF
#define CMD_PROGRAM_MAX   0xC9
#define PID_RES 0xFF
#define PID_ERR 0xFE
#define RES_PGM 0x10

#define XCP_EP_OUT 0x01
#define XCP_EP_IN  0x81
#define POLL_MS 40
#define SER_POLL_MS 10
#define CONNECT_TRIES 25

typedef struct xws { lt_u32 addr, len; const lt_u8 *data; } xws;

typedef struct xcp {
    int tp;                         /* transport: LT_FW_OPENBLT, _UART or _CAN */
    int sub;                        /* SLCAN adapter setup step */
    lt_u32 can_tx, can_rx;
    int can_kbps;
    char line[48];                  /* SLCAN: the line being received */
    int line_len;
    xws *ws;
    int n_ws, wi;
    lt_u32 off;
    int cstep, tries;
    int cto, dto, pgm_cto, motorola;
    int mta_ok, ended;
    /* the command in flight */
    int waiting;
    lt_u8 cmd;
    lt_u32 n;                       /* data bytes carried / requested */
    double deadline;
    lt_u8 rx[300];
    int rx_len;
} xcp;

static const char *xcp_err_text(int code)
{
    switch (code) {
    case 0x00: return "command processor synchronization";
    case 0x10: return "busy";
    case 0x20: return "unknown command";
    case 0x21: return "command syntax";
    case 0x22: return "out of range";
    case 0x24: return "access denied";
    case 0x25: return "access locked (seed/key unlock required)";
    case 0x29: return "sequence error";
    case 0x31: return "generic error (flash operation failed)";
    default: return "error";
    }
}

static const char *cmd_name(int c)
{
    switch (c) {
    case CMD_CONNECT: return "CONNECT";
    case CMD_GET_STATUS: return "GET_STATUS";
    case CMD_SET_MTA: return "SET_MTA";
    case CMD_UPLOAD: return "UPLOAD";
    case CMD_PROGRAM_START: return "PROGRAM_START";
    case CMD_PROGRAM_CLEAR: return "PROGRAM_CLEAR";
    case CMD_PROGRAM: return "PROGRAM";
    case CMD_PROGRAM_RESET: return "PROGRAM_RESET";
    default: return "PROGRAM_MAX";
    }
}

static void put32(const xcp *x, lt_u8 *p, lt_u32 v)
{
    if (x->motorola) { lt_store_u32be(p, v); }
    else { lt_store_u32le(p, v); }
}

/* ------------------------------------------------------------------ transports */
/* drop whatever the serial port still holds (late answers, adapter acks) */
static void drain(lt_fw_job *j)
{
    lt_u8 b[64];
    int k;
    for (k = 0; k < 32; k++) {
        if (j->io->read(j->io->ctx, b, (int)sizeof(b), 0) <= 0) { break; }
    }
}

static int slcan_cmd(lt_fw_job *j, const char *t)
{
    return j->io->write(j->io->ctx, (const lt_u8 *)t, (int)strlen(t));
}

/* 1 when the adapter answered BELL (refused the last command), 0 otherwise */
static int slcan_bell(lt_fw_job *j)
{
    lt_u8 b[64];
    int k, r, i;
    for (k = 0; k < 32; k++) {
        r = j->io->read(j->io->ctx, b, (int)sizeof(b), 0);
        if (r <= 0) { break; }
        for (i = 0; i < r; i++) {
            if (b[i] == 7) { return 1; }
        }
    }
    return 0;
}

static int tp_send(lt_fw_job *j, const lt_u8 *pkt, int len)
{
    xcp *x = (xcp *)j->m;
    lt_u8 out[300];
    if (x->tp == LT_FW_OPENBLT_CAN) {
        static const char hx[17] = "0123456789ABCDEF";
        lt_u32 id = x->can_tx & ~LT_FW_CAN_EXT;
        int k = 0, i, digits = (x->can_tx & LT_FW_CAN_EXT) != 0 ? 8 : 3;
        if (len > 8) { return LT_USB_EINVAL; }
        out[k++] = (lt_u8)(digits == 8 ? 'T' : 't');
        for (i = digits - 1; i >= 0; i--) { out[k++] = (lt_u8)hx[(id >> (4 * i)) & 15u]; }
        out[k++] = (lt_u8)('0' + len);
        for (i = 0; i < len; i++) {
            out[k++] = (lt_u8)hx[pkt[i] >> 4];
            out[k++] = (lt_u8)hx[pkt[i] & 15];
        }
        out[k++] = '\r';
        drain(j);
        x->line_len = 0;
        return j->io->write(j->io->ctx, out, k);
    }
    out[0] = (lt_u8)len;
    memcpy(out + 1, pkt, (size_t)len);
    if (x->tp == LT_FW_OPENBLT_UART) {
        drain(j);
        return j->io->write(j->io->ctx, out, len + 1);
    }
    return j->io->bulk(j->io->ctx, XCP_EP_OUT, out, len + 1, 1000);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

/* an SLCAN line: 1 and the packet (length byte first) in x->rx when it is a frame on can_rx */
static int slcan_parse(xcp *x)
{
    const char *l = x->line;
    int n = x->line_len, idn, i, dlc;
    lt_u32 id = 0;
    if (n < 1 || (l[0] != 't' && l[0] != 'T')) { return 0; }      /* acks (z, Z), empty lines */
    idn = l[0] == 't' ? 3 : 8;
    if (n < idn + 2) { return 0; }
    for (i = 1; i <= idn; i++) {
        int v = hexval(l[i]);
        if (v < 0) { return 0; }
        id = (id << 4) | (lt_u32)v;
    }
    if (l[0] == 'T') { id |= LT_FW_CAN_EXT; }
    dlc = l[idn + 1] - '0';
    /* a timestamp (Z1) may follow the data */
    if (id != x->can_rx || dlc < 1 || dlc > 8 || n < idn + 2 + 2 * dlc) { return 0; }
    for (i = 0; i < dlc; i++) {
        int hi = hexval(l[idn + 2 + 2 * i]), lo = hexval(l[idn + 3 + 2 * i]);
        if (hi < 0 || lo < 0) { return 0; }
        x->rx[1 + i] = (lt_u8)(hi * 16 + lo);
    }
    x->rx[0] = (lt_u8)dlc;
    x->rx_len = dlc + 1;
    return 1;
}

/* collect answer bytes: 1 = a whole packet sits in x->rx (length byte first), 0 = not yet,
 * LT_USB_E* on errors (LT_USB_EPIPE: the SLCAN adapter answered BELL) */
static int tp_poll(lt_fw_job *j)
{
    xcp *x = (xcp *)j->m;
    lt_u8 b[64];
    int r, i;
    if (x->tp == LT_FW_OPENBLT) { r = j->io->bulk(j->io->ctx, XCP_EP_IN, b, 64, POLL_MS); }
    else { r = j->io->read(j->io->ctx, b, 64, SER_POLL_MS); }
    if (r == LT_USB_ETIMEOUT || r == 0) { return 0; }
    if (r < 0) { return r; }
    if (x->tp == LT_FW_OPENBLT_CAN) {
        for (i = 0; i < r; i++) {
            if (b[i] == 7) { return LT_USB_EPIPE; }
            if (b[i] == '\r') {
                if (slcan_parse(x)) { return 1; }
                x->line_len = 0;
            } else if (x->line_len < (int)sizeof(x->line) - 1) {
                x->line[x->line_len++] = (char)b[i];
            } else {
                x->line_len = 0;                                /* garbage: start over */
            }
        }
        return 0;
    }
    if (x->rx_len + r > (int)sizeof(x->rx)) { return LT_USB_EINVAL; }
    memcpy(x->rx + x->rx_len, b, (size_t)r);
    x->rx_len += r;
    return x->rx_len >= 1 && x->rx_len >= x->rx[0] + 1 ? 1 : 0;
}

/* ------------------------------------------------------------------ commands */
/* send one command packet (pkt[0..len)) and start waiting for its response */
static void send_cmd(lt_fw_job *j, const lt_u8 *pkt, int len, lt_u32 n, int timeout_ms)
{
    xcp *x = (xcp *)j->m;
    int r = tp_send(j, pkt, len);
    if (r < 0) {
        if (pkt[0] == CMD_PROGRAM_RESET && r == LT_USB_ENODEV) {
            lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: OpenBLT started the new firmware.");
            return;
        }
        lt_fwj_fail(j, "sending %s failed: %s", cmd_name(pkt[0]), lt_fwj_usb_err(j, r));
        return;
    }
    x->waiting = 1;
    x->cmd = pkt[0];
    x->n = n;
    x->rx_len = 0;
    x->deadline = j->now + (double)timeout_ms;
}

static void set_mta(lt_fw_job *j, lt_u32 addr)
{
    xcp *x = (xcp *)j->m;
    lt_u8 p[8];
    p[0] = CMD_SET_MTA; p[1] = 0; p[2] = 0; p[3] = 0;
    put32(x, p + 4, addr);
    send_cmd(j, p, 8, addr, 1000);
}

static void after_write(lt_fw_job *j);

static void handle(lt_fw_job *j, const lt_u8 *p, int len)
{
    xcp *x = (xcp *)j->m;
    if (len < 1) { lt_fwj_fail(j, "empty response to %s", cmd_name(x->cmd)); return; }
    if (p[0] == PID_ERR) {
        int code = len > 1 ? p[1] : -1;
        if (x->cmd == CMD_PROGRAM_START && code == 0x25) {
            lt_fwj_fail(j, "OpenBLT wants a seed/key unlock before programming - not supported");
            return;
        }
        lt_fwj_fail(j, "OpenBLT refused %s: %s (0x%02X)", cmd_name(x->cmd), xcp_err_text(code), (unsigned)(code & 0xFF));
        return;
    }
    if (p[0] != PID_RES) { lt_fwj_fail(j, "unexpected response 0x%02X to %s", (unsigned)p[0], cmd_name(x->cmd)); return; }
    switch (x->cmd) {
    case CMD_CONNECT:
        if (len < 8) { lt_fwj_fail(j, "short CONNECT response"); return; }
        x->motorola = (p[2] & 1) != 0;
        x->cto = p[3];
        x->dto = x->motorola ? (p[4] << 8) | p[5] : (p[5] << 8) | p[4];
        if (x->tp == LT_FW_OPENBLT_CAN) {                       /* one CAN frame per packet */
            if (x->cto > 8) { x->cto = 8; }
            if (x->dto > 8) { x->dto = 8; }
        }
        if (x->cto < 8 || x->dto < 8) { lt_fwj_fail(j, "OpenBLT reports unusable packet sizes (CTO %d, DTO %d)", x->cto, x->dto); return; }
        if (x->dto > 255) { x->dto = 255; }
        x->pgm_cto = x->cto;
        lt_fwj_log(j, 0, "OpenBLT connected: %s byte order, packets %d/%d bytes",
                   x->motorola ? "Motorola" : "Intel", x->cto, x->dto);
        x->cstep = 2;
        break;
    case CMD_GET_STATUS:
        if (len < 3) { lt_fwj_fail(j, "short GET_STATUS response"); return; }
        if ((p[2] & RES_PGM) != 0) {
            lt_fwj_fail(j, "OpenBLT wants a seed/key unlock before programming - not supported");
            return;
        }
        x->cstep = 3;
        break;
    case CMD_PROGRAM_START:
        if (len >= 4 && p[3] >= 8) { x->pgm_cto = p[3]; }
        if (x->tp == LT_FW_OPENBLT_CAN && x->pgm_cto > 8) { x->pgm_cto = 8; }
        lt_fwj_phase(j, LT_FW_PH_ERASE, j->w_total[LT_FW_PH_ERASE], "Erasing the application area");
        x->wi = 0;
        x->mta_ok = 0;
        break;
    case CMD_SET_MTA: x->mta_ok = 1; break;
    case CMD_PROGRAM_CLEAR:
        j->done += x->n;
        x->wi++;
        x->mta_ok = 0;
        break;
    case CMD_PROGRAM:
    case CMD_PROGRAM_MAX:
        if (x->n == 0) { x->ended = 1; after_write(j); break; }
        x->off += x->n;
        j->done += x->n;
        break;
    case CMD_UPLOAD:
        if ((lt_u32)len < x->n + 1u) { lt_fwj_fail(j, "short UPLOAD response"); return; }
        if (memcmp(p + 1, x->ws[x->wi].data + x->off, (size_t)x->n) != 0) {
            lt_u32 k = 0;
            while (k < x->n && p[1 + k] == x->ws[x->wi].data[x->off + k]) { k++; }
            lt_fwj_fail(j, "Verify failed at 0x%08lX: flash holds 0x%02X, the image 0x%02X",
                        (unsigned long)(x->ws[x->wi].addr + x->off + k), (unsigned)p[1 + k],
                        (unsigned)x->ws[x->wi].data[x->off + k]);
            return;
        }
        x->off += x->n;
        j->done += x->n;
        break;
    default:   /* PROGRAM_RESET */
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: OpenBLT started the new firmware.");
        break;
    }
}

static void on_timeout(lt_fw_job *j)
{
    xcp *x = (xcp *)j->m;
    x->waiting = 0;
    if (x->cmd == CMD_CONNECT && ++x->tries < CONNECT_TRIES) { return; }   /* try again */
    if (x->cmd == CMD_PROGRAM_RESET) {
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: OpenBLT started the new firmware.");
        return;
    }
    if (x->cmd != CMD_CONNECT) { lt_fwj_fail(j, "no answer to %s", cmd_name(x->cmd)); }
    else if (x->tp == LT_FW_OPENBLT_UART) {
        lt_fwj_fail(j, "OpenBLT does not answer (is the ECU in its bootloader, the port and the baud rate right?)");
    } else if (x->tp == LT_FW_OPENBLT_CAN) {
        lt_fwj_fail(j, "OpenBLT does not answer on CAN id 0x%03lX (is the ECU in its bootloader? check the ids, "
                    "the bit rate and the bus termination)", (unsigned long)(x->can_rx & ~LT_FW_CAN_EXT));
    } else {
        lt_fwj_fail(j, "OpenBLT does not answer (is the ECU in its bootloader?)");
    }
}

static void recv_step(lt_fw_job *j)
{
    xcp *x = (xcp *)j->m;
    int r = tp_poll(j);
    if (r == 0) {
        if (j->now >= x->deadline) { on_timeout(j); }
        return;
    }
    if (r < 0) {
        if (x->cmd == CMD_PROGRAM_RESET) {
            lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: OpenBLT started the new firmware.");
            return;
        }
        if (r == LT_USB_EINVAL) { lt_fwj_fail(j, "oversized response to %s", cmd_name(x->cmd)); return; }
        if (r == LT_USB_EPIPE && x->tp == LT_FW_OPENBLT_CAN) {
            lt_fwj_fail(j, "the CAN adapter refused to send %s (BELL): is the CAN channel open and the bus "
                        "connected?", cmd_name(x->cmd));
            return;
        }
        lt_fwj_fail(j, "waiting for %s: %s", cmd_name(x->cmd), lt_fwj_usb_err(j, r));
        return;
    }
    x->waiting = 0;
    handle(j, x->rx + 1, x->rx[0]);
}

/* connect step 0: claim the USB interface, empty the UART, or set up the SLCAN adapter */
static void tp_open(lt_fw_job *j)
{
    xcp *x = (xcp *)j->m;
    char t[16];
    int r, code;
    if (x->tp == LT_FW_OPENBLT) {
        r = j->io->claim(j->io->ctx, 0, -1);
        if (r < 0) { lt_fwj_fail(j, "cannot claim the OpenBLT interface: %s", lt_fwj_usb_err(j, r)); return; }
        x->cstep = 1;
        return;
    }
    if (x->tp == LT_FW_OPENBLT_UART) {
        drain(j);
        x->cstep = 1;
        return;
    }
    code = lt_fw_slcan_speed(x->can_kbps);
    switch (x->sub) {
    case 0:                                     /* flush the adapter's line buffer, close the channel */
        r = slcan_cmd(j, "\r\r\rC\r");
        break;
    case 1:
        drain(j);
        lt_snprintf(t, sizeof(t), "S%d\r", code);
        r = slcan_cmd(j, t);
        break;
    case 2:
        if (slcan_bell(j)) {
            lt_fwj_fail(j, "the CAN adapter refused the bit rate %d kbit/s (S%d): is it an SLCAN adapter?",
                        x->can_kbps, code);
            return;
        }
        r = slcan_cmd(j, "O\r");
        break;
    default:
        if (slcan_bell(j)) { lt_fwj_fail(j, "the CAN adapter refused to open the CAN channel (O)"); return; }
        lt_fwj_log(j, 0, "CAN adapter open at %d kbit/s; XCP on ids 0x%03lX (to the ECU) / 0x%03lX (from it)",
                   x->can_kbps, (unsigned long)(x->can_tx & ~LT_FW_CAN_EXT),
                   (unsigned long)(x->can_rx & ~LT_FW_CAN_EXT));
        x->cstep = 1;
        return;
    }
    if (r < 0) { lt_fwj_fail(j, "writing to the CAN adapter failed: %s", lt_fwj_usb_err(j, r)); return; }
    x->sub++;
    j->next_ms = j->now + 30.0;                 /* let the adapter answer */
}

static void after_write(lt_fw_job *j)
{
    xcp *x = (xcp *)j->m;
    x->wi = 0;
    x->off = 0;
    x->mta_ok = 0;
    if (j->o.verify) {
        lt_fwj_phase(j, LT_FW_PH_VERIFY, j->w_total[LT_FW_PH_VERIFY], "Verifying (reading the flash back)");
    } else {
        lt_fwj_phase(j, LT_FW_PH_LEAVE, 0, j->o.leave ? "Starting the firmware" : "Finishing");
    }
}

static void xcp_step(lt_fw_job *j)
{
    xcp *x = (xcp *)j->m;
    lt_u8 p[300];
    if (x->waiting) { recv_step(j); return; }
    if (j->cancel) {
        lt_fwj_phase(j, LT_FW_PH_CANCELLED, 0, NULL);
        lt_strlcpy(j->msg, j->phase > LT_FW_PH_CONNECT && j->w_total[LT_FW_PH_ERASE] > 0 && j->done > 0
                   ? "Canceled: the application is incomplete; OpenBLT stays active - flash again."
                   : "Canceled.", sizeof(j->msg));
        lt_fwj_log(j, 2, "%s", j->msg);
        return;
    }
    switch (j->phase) {
    case LT_FW_PH_CONNECT:
        if (x->cstep == 0) { tp_open(j); return; }
        if (x->cstep == 1) { p[0] = CMD_CONNECT; p[1] = 0; send_cmd(j, p, 2, 0, 200); return; }
        if (x->cstep == 2) { p[0] = CMD_GET_STATUS; send_cmd(j, p, 1, 0, 1000); return; }
        p[0] = CMD_PROGRAM_START;
        send_cmd(j, p, 1, 0, 1000);
        return;
    case LT_FW_PH_ERASE:
        if (x->wi >= x->n_ws) {
            char m[80];
            x->wi = 0;
            x->off = 0;
            x->mta_ok = 0;
            lt_snprintf(m, sizeof(m), "Writing %lu bytes", (unsigned long)j->w_total[LT_FW_PH_WRITE]);
            lt_fwj_phase(j, LT_FW_PH_WRITE, j->w_total[LT_FW_PH_WRITE], m);
            return;
        }
        if (!x->mta_ok) { set_mta(j, x->ws[x->wi].addr); return; }
        p[0] = CMD_PROGRAM_CLEAR; p[1] = 0; p[2] = 0; p[3] = 0;
        put32(x, p + 4, x->ws[x->wi].len);
        send_cmd(j, p, 8, x->ws[x->wi].len, 30000);
        return;
    case LT_FW_PH_WRITE:
    case LT_FW_PH_VERIFY:
        while (x->wi < x->n_ws && x->off >= x->ws[x->wi].len) { x->wi++; x->off = 0; x->mta_ok = 0; }
        if (x->wi >= x->n_ws) {
            if (j->phase == LT_FW_PH_WRITE) { p[0] = CMD_PROGRAM; p[1] = 0; send_cmd(j, p, 2, 0, 5000); }
            else {
                lt_fwj_log(j, 1, "Verify OK: the flash matches the image");
                lt_fwj_phase(j, LT_FW_PH_LEAVE, 0, j->o.leave ? "Starting the firmware" : "Finishing");
            }
            return;
        }
        if (!x->mta_ok) { set_mta(j, x->ws[x->wi].addr + x->off); return; }
        {
            lt_u32 rem = x->ws[x->wi].len - x->off, n;
            const lt_u8 *src = x->ws[x->wi].data + x->off;
            if (j->phase == LT_FW_PH_VERIFY) {
                n = rem < (lt_u32)(x->dto - 1) ? rem : (lt_u32)(x->dto - 1);
                p[0] = CMD_UPLOAD;
                p[1] = (lt_u8)n;
                send_cmd(j, p, 2, n, 2000);
            } else if (rem >= (lt_u32)(x->pgm_cto - 1)) {
                n = (lt_u32)(x->pgm_cto - 1);
                p[0] = CMD_PROGRAM_MAX;
                memcpy(p + 1, src, (size_t)n);
                send_cmd(j, p, (int)n + 1, n, 2000);
            } else {
                n = rem < (lt_u32)(x->pgm_cto - 2) ? rem : (lt_u32)(x->pgm_cto - 2);
                p[0] = CMD_PROGRAM;
                p[1] = (lt_u8)n;
                memcpy(p + 2, src, (size_t)n);
                send_cmd(j, p, (int)n + 2, n, 2000);
            }
        }
        return;
    case LT_FW_PH_LEAVE:
        if (!j->o.leave) {
            lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: firmware written; OpenBLT stays active until the ECU is reset.");
            return;
        }
        p[0] = CMD_PROGRAM_RESET;
        send_cmd(j, p, 1, 0, 500);
        return;
    default:
        return;
    }
}

static void xcp_free(lt_fw_job *j)
{
    xcp *x = (xcp *)j->m;
    if (x == NULL) { return; }
    lt_free(x->ws);
    lt_free(x);
    j->m = NULL;
}

int lt_fwj_xcp_init(lt_fw_job *j)
{
    xcp *x = (xcp *)lt_calloc(1, sizeof(xcp));
    const lt_fw_image *img = j->img;
    lt_u32 skipped = 0, total = 0;
    size_t i;
    if (x == NULL) { lt_fwj_fail(j, "out of memory"); return -1; }
    j->m = x;
    j->m_step = xcp_step;
    j->m_free = xcp_free;
    x->tp = j->method;
    x->can_tx = j->o.can_tx != 0 ? j->o.can_tx : LT_FW_CAN_TX_ID;
    x->can_rx = j->o.can_rx != 0 ? j->o.can_rx : LT_FW_CAN_RX_ID;
    x->can_kbps = j->o.can_kbps > 0 ? j->o.can_kbps : 500;
    if (x->tp == LT_FW_OPENBLT ? j->io->bulk == NULL || j->io->claim == NULL
                               : j->io->write == NULL || j->io->read == NULL) {
        lt_fwj_fail(j, "this connection cannot carry %s", lt_fw_method_name(x->tp));
        return -1;
    }
    if (x->tp == LT_FW_OPENBLT_CAN && lt_fw_slcan_speed(x->can_kbps) < 0) {
        lt_fwj_fail(j, "SLCAN adapters cannot run CAN at %d kbit/s", x->can_kbps);
        return -1;
    }
    x->ws = (xws *)lt_calloc(img->n_segs, sizeof(xws));
    if (x->ws == NULL) { lt_fwj_fail(j, "out of memory"); return -1; }
    for (i = 0; i < img->n_segs; i++) {
        const lt_fw_seg *s = &img->segs[i];
        lt_u32 a = s->addr, len = s->len, last = s->addr + (s->len - 1u);
        const lt_fw_range *pr;
        if (j->o.app_base != 0 && last < j->o.app_base) { skipped += len; continue; }
        if (j->o.app_base != 0 && a < j->o.app_base) {
            skipped += j->o.app_base - a;
            len -= j->o.app_base - a;
            a = j->o.app_base;
        }
        pr = lt_fwj_protected(j, a, len);
        if (pr != NULL) {
            lt_fwj_fail(j, "the image covers 0x%08lX-0x%08lX, which holds %s; untick \"Keep ECU settings\" "
                        "to allow it", (unsigned long)pr->addr, (unsigned long)(pr->addr + (pr->len - 1u)),
                        pr->what != NULL ? pr->what : "protected data");
            return -1;
        }
        x->ws[x->n_ws].addr = a;
        x->ws[x->n_ws].len = len;
        x->ws[x->n_ws].data = s->data + (a - s->addr);
        x->n_ws++;
        total += len;
    }
    if (x->n_ws == 0) {
        lt_fwj_fail(j, "nothing to write above the bootloader (0x%08lX): this image holds only the bootloader",
                    (unsigned long)j->o.app_base);
        return -1;
    }
    if (skipped > 0) {
        lt_fwj_log(j, 0, "Skipping %lu bytes below 0x%08lX (the bootloader is not rewritten over OpenBLT)",
                   (unsigned long)skipped, (unsigned long)j->o.app_base);
    }
    j->w_total[LT_FW_PH_ERASE] = total;
    j->w_total[LT_FW_PH_WRITE] = total;
    j->w_total[LT_FW_PH_VERIFY] = j->o.verify ? total : 0;
    return 0;
}
