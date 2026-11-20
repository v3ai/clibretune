/* fw_sim_ser.c - simulated serial bootloaders behind the byte-stream half of lt_fw_io:
 *   STK500v2  the Arduino Mega 2560 "wiring" bootloader: runs for about a second after a
 *             DTR/RTS reset pulse (or always, when the port has no modem lines and the user
 *             resets the board), framed messages with sequence numbers and XOR checksum,
 *             per-page erase + write, read back, signature, an untouchable 8 KB boot section;
 *   OpenBLT   the XCP slave of fw_sim.c over a UART (length byte + packet) and over CAN behind
 *             an SLCAN (Lawicel ASCII) adapter: C / Sn / O, t/T frames, z/Z acks, BELL errors;
 *             frames on the wrong id or at the wrong bit rate get no answer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw_sim_priv.h"

#define BOOT_WINDOW_MS 1000.0
#define STK_OK 0x00
#define STK_FAILED 0xC0
#define STK_UNKNOWN 0xC9

static void out_put(lt_fw_sim *s, const lt_u8 *p, int n)
{
    if (s->out_pos > 0 && s->out_pos >= s->out_len) { s->out_len = 0; s->out_pos = 0; }
    if (n <= 0 || s->out_len + n > LT_FWSIM_OUT) { return; }    /* overflow: dropped, like a UART */
    memcpy(s->out + s->out_len, p, (size_t)n);
    s->out_len += n;
}

static void out_str(lt_fw_sim *s, const char *t)
{
    out_put(s, (const lt_u8 *)t, (int)strlen(t));
}

static void in_drop(lt_fw_sim *s, int n)
{
    if (n >= s->in_len) { s->in_len = 0; return; }
    memmove(s->in, s->in + n, (size_t)(s->in_len - n));
    s->in_len -= n;
}

/* queue an answer for later (resp / resp_at), moved to `out` by pump() once it is due */
static void later(lt_fw_sim *s, const lt_u8 *p, int n, double delay)
{
    if (n > (int)sizeof(s->resp)) { n = (int)sizeof(s->resp); }
    memcpy(s->resp, p, (size_t)n);
    s->resp_len = n;
    s->resp_pos = 0;
    s->resp_at = lt_fwsim_now(s) + delay;
}

/* ------------------------------------------------------------------ STK500v2 */
static void stk_answer(lt_fw_sim *s, lt_u8 seq, const lt_u8 *body, int n, double delay)
{
    lt_u8 f[600];
    lt_u8 x = 0;
    int i, k = 0;
    if (s->noise_left > 0) {                     /* a sketch's last words before the reset */
        static const lt_u8 junk[6] = { 0x00, 0xF0, 'o', 'k', 0x1B, '\n' };
        out_put(s, junk, 6);
        s->noise_left = 0;
    }
    f[k++] = 0x1B;
    f[k++] = seq;
    f[k++] = (lt_u8)((n >> 8) & 0xFF);
    f[k++] = (lt_u8)(n & 0xFF);
    f[k++] = 0x0E;
    memcpy(f + k, body, (size_t)n);
    k += n;
    for (i = 0; i < k; i++) { x = (lt_u8)(x ^ f[i]); }
    f[k++] = x;
    later(s, f, k, delay);
}

static void stk_cmd(lt_fw_sim *s, lt_u8 seq, const lt_u8 *b, int n)
{
    lt_u8 r[300];
    int k;
    double delay = 0.0;
    r[0] = b[0];
    r[1] = STK_OK;
    k = 2;
    switch (b[0]) {
    case 0x01: memcpy(r + 2, "\010AVRISP_2", 9); k = 11; break;
    case 0x03: r[2] = 0; k = 3; break;                              /* GET_PARAMETER */
    case 0x10: case 0x12: break;                                    /* ENTER_PROGMODE, CHIP_ERASE (ignored) */
    case 0x11: s->boot = 0; s->left = 1; break;                     /* LEAVE_PROGMODE: run the app */
    case 0x06:
        if (n < 5) { r[1] = STK_FAILED; break; }
        s->avr_addr = (lt_load_u32be(b + 1) << 1) & LT_U32_MASK;    /* word address; bit 31 = "extended" */
        break;
    case 0x13: {
        lt_u32 cnt = n >= 3 ? ((lt_u32)b[1] << 8) | b[2] : 0u, page;
        if (n < 10 || (lt_u32)n < 10u + cnt || cnt == 0 || cnt > 256u ||
            s->avr_addr + cnt > LT_FW_MEGA2560_APP) {               /* SPM cannot touch the boot section */
            r[1] = STK_FAILED;
            break;
        }
        page = s->avr_addr & ~(lt_u32)255u;
        memset(s->flash + page, 0xFF, 256u);                        /* boot_page_erase */
        memcpy(s->flash + s->avr_addr, b + 10, cnt);
        s->avr_addr += cnt;
        s->writes++;
        delay = (double)s->o.write_ms;
        break;
    }
    case 0x14: {
        lt_u32 cnt = n >= 3 ? ((lt_u32)b[1] << 8) | b[2] : 0u;
        if (cnt == 0 || cnt > 256u || !lt_fwsim_in_flash(s, s->avr_addr, cnt)) { r[1] = STK_FAILED; break; }
        memcpy(r + 2, s->flash + s->avr_addr, cnt);
        r[2 + cnt] = STK_OK;
        k = (int)cnt + 3;
        s->avr_addr += cnt;
        break;
    }
    case 0x1B: {
        static const lt_u8 m2560[3] = { 0x1E, 0x98, 0x01 }, m1280[3] = { 0x1E, 0x97, 0x03 };
        int idx = n >= 5 ? b[4] % 3 : 0;
        r[2] = s->o.bad_sig ? m1280[idx] : m2560[idx];
        r[3] = STK_OK;
        k = 4;
        break;
    }
    default: r[1] = STK_UNKNOWN; break;
    }
    stk_answer(s, seq, r, k, delay);
}

static void stk_feed(lt_fw_sim *s)
{
    for (;;) {
        int size, total, i;
        lt_u8 x = 0;
        while (s->in_len > 0 && s->in[0] != 0x1B) { in_drop(s, 1); }
        if (s->in_len < 5) { return; }
        size = (s->in[2] << 8) | s->in[3];
        if (s->in[4] != 0x0E || size == 0 || size > 300) { in_drop(s, 1); continue; }
        total = 5 + size + 1;
        if (s->in_len < total) { return; }
        for (i = 0; i < total; i++) { x = (lt_u8)(x ^ s->in[i]); }
        if (x != 0) { in_drop(s, 1); continue; }               /* the real one answers ANSWER_CKSUM_ERROR */
        s->boot_until = 1e18;                                   /* talking to the host: stay */
        stk_cmd(s, s->in[1], s->in + 5, size);
        in_drop(s, total);
    }
}

/* ------------------------------------------------------------------ OpenBLT UART */
static void uart_feed(lt_fw_sim *s)
{
    while (s->in_len > 0) {
        int n = s->in[0];
        if (n == 0) { in_drop(s, 1); continue; }
        if (s->in_len < n + 1) { return; }
        lt_fwsim_xcp(s, s->in + 1, n);
        in_drop(s, n + 1);
    }
}

/* ------------------------------------------------------------------ SLCAN */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

static void slcan_line(lt_fw_sim *s, const lt_u8 *l, int n)
{
    int want = lt_fw_slcan_speed(s->o.can_kbps > 0 ? s->o.can_kbps : 500);
    lt_u32 rx = s->o.can_tx != 0 ? s->o.can_tx : LT_FW_CAN_TX_ID;
    if (n == 0) { return; }
    switch (l[0]) {
    case 'C': s->can_open = 0; out_str(s, "\r"); return;
    case 'S':
        if (s->can_open || n != 2 || l[1] < '0' || l[1] > '8') { out_str(s, "\a"); return; }
        s->can_speed = l[1] - '0';
        out_str(s, "\r");
        return;
    case 'O':
        if (s->can_open) { out_str(s, "\a"); return; }
        s->can_open = 1;
        out_str(s, "\r");
        return;
    case 't':
    case 'T': {
        int idn = l[0] == 't' ? 3 : 8, i, dlc;
        lt_u32 id = 0;
        lt_u8 d[8];
        if (!s->can_open || n < idn + 2) { out_str(s, "\a"); return; }
        for (i = 1; i <= idn; i++) {
            int v = hexval(l[i]);
            if (v < 0) { out_str(s, "\a"); return; }
            id = (id << 4) | (lt_u32)v;
        }
        dlc = l[idn + 1] - '0';
        if (dlc < 0 || dlc > 8 || n != idn + 2 + 2 * dlc) { out_str(s, "\a"); return; }
        for (i = 0; i < dlc; i++) {
            int hi = hexval(l[idn + 2 + 2 * i]), lo = hexval(l[idn + 3 + 2 * i]);
            if (hi < 0 || lo < 0) { out_str(s, "\a"); return; }
            d[i] = (lt_u8)(hi * 16 + lo);
        }
        out_str(s, l[0] == 't' ? "z\r" : "Z\r");
        if (l[0] == 'T') { id |= LT_FW_CAN_EXT; }
        if (s->can_speed == want && id == rx && dlc > 0) { lt_fwsim_xcp(s, d, dlc); }
        return;
    }
    default: out_str(s, "\a"); return;
    }
}

static void slcan_feed(lt_fw_sim *s)
{
    int i;
    for (i = 0; i < s->in_len; i++) {
        if (s->in[i] == '\r') {
            slcan_line(s, s->in, i);
            in_drop(s, i + 1);
            i = -1;
        }
    }
}

static void slcan_frame(lt_fw_sim *s, const lt_u8 *p, int n)
{
    static const char hx[17] = "0123456789ABCDEF";
    lt_u32 id = s->o.can_rx != 0 ? s->o.can_rx : LT_FW_CAN_RX_ID;
    char line[40];
    int k = 0, i, digits = (id & LT_FW_CAN_EXT) != 0 ? 8 : 3;
    if (n > 8) { n = 8; }
    line[k++] = digits == 8 ? 'T' : 't';
    id &= ~LT_FW_CAN_EXT;
    for (i = digits - 1; i >= 0; i--) { line[k++] = hx[(id >> (4 * i)) & 15u]; }
    line[k++] = (char)('0' + n);
    for (i = 0; i < n; i++) {
        line[k++] = hx[p[i] >> 4];
        line[k++] = hx[p[i] & 15];
    }
    line[k++] = '\r';
    line[k] = '\0';
    out_str(s, line);
}

/* ------------------------------------------------------------------ the stream */
static void pump(lt_fw_sim *s)
{
    if (s->o.kind == LT_FW_SIM_STK500 && s->boot && lt_fwsim_now(s) > s->boot_until) {
        s->boot = 0;                                            /* window over: the old sketch runs */
        s->left = 1;
    }
    if (s->resp_len == 0 || lt_fwsim_now(s) < s->resp_at) { return; }
    if (s->o.kind == LT_FW_SIM_OPENBLT_CAN) { slcan_frame(s, s->resp + 1, s->resp[0]); }
    else { out_put(s, s->resp, s->resp_len); }
    s->resp_len = 0;
    s->resp_pos = 0;
}

static int ser_write(void *ctx, const lt_u8 *data, int len)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    int n = len;
    pump(s);
    if (len <= 0) { return 0; }
    if (s->o.kind == LT_FW_SIM_STK500 && !s->boot) { return len; }   /* the sketch ignores it */
    if (n > LT_FWSIM_IN - s->in_len) { n = LT_FWSIM_IN - s->in_len; }
    memcpy(s->in + s->in_len, data, (size_t)n);
    s->in_len += n;
    if (s->o.kind == LT_FW_SIM_STK500) { stk_feed(s); }
    else if (s->o.kind == LT_FW_SIM_OPENBLT_UART) { uart_feed(s); }
    else { slcan_feed(s); }
    return len;
}

static int ser_read(void *ctx, lt_u8 *buf, int cap, int timeout_ms)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    int n;
    LT_UNUSED(timeout_ms);
    pump(s);
    n = s->out_len - s->out_pos;
    if (n > cap) { n = cap; }
    if (n <= 0) { return 0; }
    memcpy(buf, s->out + s->out_pos, (size_t)n);
    s->out_pos += n;
    if (s->out_pos >= s->out_len) { s->out_len = 0; s->out_pos = 0; }
    return n;
}

static int ser_lines(void *ctx, int dtr, int rts)
{
    lt_fw_sim *s = (lt_fw_sim *)ctx;
    LT_UNUSED(rts);
    if (s->o.no_lines) { return LT_USB_ENOTSUP; }
    if (s->o.kind == LT_FW_SIM_STK500 && !s->dtr && dtr) {      /* the auto-reset capacitor */
        s->resets++;
        s->boot = 1;
        s->left = 0;
        s->boot_until = lt_fwsim_now(s) + BOOT_WINDOW_MS;
        s->in_len = 0;
        s->out_len = 0;
        s->out_pos = 0;
        s->resp_len = 0;
        s->noise_left = s->o.noise;
    }
    s->dtr = dtr;
    return 0;
}

void lt_fwsim_ser_init(lt_fw_sim *s)
{
    s->dtr = 1;
    if (s->o.kind == LT_FW_SIM_STK500 && s->o.no_lines) {    /* the user pressed reset */
        s->boot = 1;
        s->boot_until = 1e18;
        s->noise_left = s->o.noise;
    }
    if (s->o.kind == LT_FW_SIM_OPENBLT_CAN) { s->can_speed = -1; }
}

void lt_fwsim_ser_io(lt_fw_sim *s, lt_fw_io *io)
{
    io->ctx = s;
    io->write = ser_write;
    io->read = ser_read;
    io->set_lines = ser_lines;
}
