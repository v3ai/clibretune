/* fw_stk500.c - STK500v2 host for the Arduino Mega 2560 bootloader (what avrdude calls the
 * "wiring" programmer; Speeduino on a Mega).
 *
 * Reset: DTR and RTS cleared for 50 ms, then asserted (the board's auto-reset capacitor turns
 * the edge into a reset pulse); after another 50 ms the input is drained and CMD_SIGN_ON is
 * sent until the bootloader answers (it only listens for about a second after a reset; the
 * pulse is repeated once). Frames: 0x1B, sequence number, 16-bit big-endian body size, 0x0E,
 * body, XOR of all previous bytes; answers echo the sequence number and command byte, then a
 * status (0x00 = OK).
 * Sequence: SIGN_ON, READ_SIGNATURE_ISP x3 (must be 1E 98 01, ATmega2560), ENTER_PROGMODE_ISP;
 * per 256-byte flash page: LOAD_ADDRESS (word address, bit 31 set for the extended address)
 * and PROGRAM_FLASH_ISP (the bootloader erases the page itself; gaps are written as 0xFF);
 * read back with LOAD_ADDRESS + READ_FLASH_ISP; LEAVE_PROGMODE_ISP starts the firmware.
 * Every command is sent from one step and its answer is polled with short reads.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw_priv.h"

#define CMD_SIGN_ON 0x01
#define CMD_LOAD_ADDRESS 0x06
#define CMD_ENTER_PROGMODE 0x10
#define CMD_LEAVE_PROGMODE 0x11
#define CMD_PROGRAM_FLASH 0x13
#define CMD_READ_FLASH 0x14
#define CMD_READ_SIGNATURE 0x1B
#define STATUS_OK 0x00
#define PAGE ((lt_u32)LT_FW_AVR_PAGE)
#define POLL_MS 10
#define SYNC_TRIES 10
#define SYNC_TRIES_MANUAL 30            /* about 9 s to press the reset button */
#define SYNC_MS 300

static const lt_u8 want_sig[3] = { 0x1E, 0x98, 0x01 };

typedef struct stk {
    lt_u32 *pages;                  /* device byte addresses of the pages to write */
    int n_pages, pi;
    int cstep, tries, addr_ok, no_lines;
    lt_u8 sig[3];
    lt_u8 seq;
    /* the command in flight */
    int waiting;
    lt_u8 cmd;
    double deadline;
    lt_u8 rx[600];
    int rx_len;
    lt_u8 page[LT_FW_AVR_PAGE];
} stk;

static const char *cmd_name(int c)
{
    switch (c) {
    case CMD_SIGN_ON: return "SIGN_ON";
    case CMD_LOAD_ADDRESS: return "LOAD_ADDRESS";
    case CMD_ENTER_PROGMODE: return "ENTER_PROGMODE";
    case CMD_LEAVE_PROGMODE: return "LEAVE_PROGMODE";
    case CMD_PROGRAM_FLASH: return "PROGRAM_FLASH";
    case CMD_READ_FLASH: return "READ_FLASH";
    default: return "READ_SIGNATURE";
    }
}

/* the image bytes of page pi, gaps as 0xFF */
static void fill_page(lt_fw_job *j, stk *k)
{
    (void)lt_fw_image_read(j->img, j->o.dev_base + k->pages[k->pi], k->page, PAGE, 0xFF);
}

static void send_cmd(lt_fw_job *j, const lt_u8 *body, int n, int timeout_ms)
{
    stk *k = (stk *)j->m;
    lt_u8 f[LT_FW_AVR_PAGE + 32];
    lt_u8 x = 0, junk[64];
    int i, len = 0, r, d;
    k->seq = (lt_u8)((k->seq + 1u) & 0xFFu);
    f[len++] = 0x1B;
    f[len++] = k->seq;
    f[len++] = (lt_u8)((n >> 8) & 0xFF);
    f[len++] = (lt_u8)(n & 0xFF);
    f[len++] = 0x0E;
    memcpy(f + len, body, (size_t)n);
    len += n;
    for (i = 0; i < len; i++) { x = (lt_u8)(x ^ f[i]); }
    f[len++] = x;
    for (d = 0; d < 32; d++) {                              /* stale bytes of an earlier attempt */
        if (j->io->read(j->io->ctx, junk, (int)sizeof(junk), 0) <= 0) { break; }
    }
    r = j->io->write(j->io->ctx, f, len);
    if (r < 0) { lt_fwj_fail(j, "writing %s to the serial port failed: %s", cmd_name(body[0]), lt_fwj_usb_err(j, r)); return; }
    k->waiting = 1;
    k->cmd = body[0];
    k->rx_len = 0;
    k->deadline = j->now + (double)timeout_ms;
}

static void load_address(lt_fw_job *j, lt_u32 byte_addr)
{
    lt_u8 b[5];
    b[0] = CMD_LOAD_ADDRESS;
    lt_store_u32be(b + 1, (byte_addr >> 1) | 0x80000000UL);
    send_cmd(j, b, 5, 1000);
}

static void after_write(lt_fw_job *j)
{
    stk *k = (stk *)j->m;
    k->pi = 0;
    k->addr_ok = 0;
    if (j->o.verify) {
        lt_fwj_phase(j, LT_FW_PH_VERIFY, j->w_total[LT_FW_PH_VERIFY], "Verifying (reading the flash back)");
    } else {
        lt_fwj_phase(j, LT_FW_PH_LEAVE, 0, j->o.leave ? "Starting the firmware" : "Finishing");
    }
}

static void handle(lt_fw_job *j, const lt_u8 *b, int n)
{
    stk *k = (stk *)j->m;
    if (n < 2 || b[0] != k->cmd) { lt_fwj_fail(j, "unexpected answer to %s", cmd_name(k->cmd)); return; }
    if (b[1] != STATUS_OK) {
        lt_fwj_fail(j, "the bootloader refused %s (status 0x%02X)%s", cmd_name(k->cmd), (unsigned)b[1],
                    k->cmd == CMD_PROGRAM_FLASH ? ": the page may lie in the boot section" : "");
        return;
    }
    switch (k->cmd) {
    case CMD_SIGN_ON:
        if (n >= 3 && b[2] > 0 && b[2] <= 32 && n >= 3 + b[2]) {
            char name[40];
            memcpy(name, b + 3, b[2]);
            name[b[2]] = '\0';
            lt_fwj_log(j, 0, "Bootloader answered: %s (STK500v2)", name);
        }
        k->cstep = 3;
        break;
    case CMD_READ_SIGNATURE:
        if (n < 3) { lt_fwj_fail(j, "short signature answer"); return; }
        k->sig[k->cstep - 3] = b[2];
        k->cstep++;
        if (k->cstep == 6) {
            if (memcmp(k->sig, want_sig, 3) != 0) {
                lt_fwj_fail(j, "Device signature %02X %02X %02X is not an ATmega2560 (1E 98 01): wrong board "
                            "or preset; nothing was written", (unsigned)k->sig[0], (unsigned)k->sig[1],
                            (unsigned)k->sig[2]);
                return;
            }
            lt_fwj_log(j, 0, "ATmega2560 found (signature 1E 98 01)");
        }
        break;
    case CMD_ENTER_PROGMODE: {
        char m[80];
        lt_snprintf(m, sizeof(m), "Writing %d pages (%lu bytes)", k->n_pages,
                    (unsigned long)j->w_total[LT_FW_PH_WRITE]);
        lt_fwj_phase(j, LT_FW_PH_WRITE, j->w_total[LT_FW_PH_WRITE], m);
        k->pi = 0;
        k->addr_ok = 0;
        break;
    }
    case CMD_LOAD_ADDRESS: k->addr_ok = 1; break;
    case CMD_PROGRAM_FLASH:
        k->pi++;
        k->addr_ok = 0;
        j->done += PAGE;
        if (k->pi >= k->n_pages) { after_write(j); }
        break;
    case CMD_READ_FLASH:
        if (n < 2 + (int)PAGE) { lt_fwj_fail(j, "short READ_FLASH answer"); return; }
        fill_page(j, k);
        if (memcmp(b + 2, k->page, PAGE) != 0) {
            lt_u32 i = 0;
            while (i < PAGE - 1u && b[2 + i] == k->page[i]) { i++; }
            lt_fwj_fail(j, "Verify failed at 0x%05lX: flash holds 0x%02X, the image 0x%02X",
                        (unsigned long)(k->pages[k->pi] + i), (unsigned)b[2 + i], (unsigned)k->page[i]);
            return;
        }
        k->pi++;
        k->addr_ok = 0;
        j->done += PAGE;
        if (k->pi >= k->n_pages) {
            lt_fwj_log(j, 1, "Verify OK: the flash matches the image");
            lt_fwj_phase(j, LT_FW_PH_LEAVE, 0, j->o.leave ? "Starting the firmware" : "Finishing");
        }
        break;
    default:   /* LEAVE_PROGMODE */
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: the bootloader started the new firmware.");
        break;
    }
}

static void on_timeout(lt_fw_job *j)
{
    stk *k = (stk *)j->m;
    k->waiting = 0;
    if (k->cmd == CMD_SIGN_ON && ++k->tries < (k->no_lines ? SYNC_TRIES_MANUAL : SYNC_TRIES)) {
        if (k->tries == SYNC_TRIES / 2 && !k->no_lines) { k->cstep = 0; }   /* reset once more */
        return;
    }
    if (k->cmd == CMD_LEAVE_PROGMODE) {
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: the bootloader started the new firmware.");
        return;
    }
    if (k->cmd == CMD_SIGN_ON) {
        lt_fwj_fail(j, k->no_lines
                    ? "The bootloader does not answer: this port cannot reset the board (no DTR/RTS); click "
                      "Start, then press the board's reset button within a few seconds"
                    : "The bootloader does not answer (right port? is a Mega 2560 with its bootloader attached?)");
        return;
    }
    lt_fwj_fail(j, "no answer to %s", cmd_name(k->cmd));
}

/* collect answer bytes, resynchronise on the start byte, check size, token and checksum */
static void recv_step(lt_fw_job *j)
{
    stk *k = (stk *)j->m;
    lt_u8 b[128];
    int r = j->io->read(j->io->ctx, b, (int)sizeof(b), POLL_MS);
    if (r < 0) {
        if (k->cmd == CMD_LEAVE_PROGMODE) {
            lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: the bootloader started the new firmware.");
            return;
        }
        lt_fwj_fail(j, "reading the serial port failed: %s", lt_fwj_usb_err(j, r));
        return;
    }
    if (r > (int)sizeof(k->rx) - k->rx_len) { k->rx_len = 0; }
    memcpy(k->rx + k->rx_len, b, (size_t)r);
    k->rx_len += r;
    for (;;) {
        int size, total, i;
        lt_u8 x = 0;
        while (k->rx_len > 0 && k->rx[0] != 0x1B) {
            memmove(k->rx, k->rx + 1, (size_t)(k->rx_len - 1));
            k->rx_len--;
        }
        if (k->rx_len < 5) { break; }
        size = (k->rx[2] << 8) | k->rx[3];
        total = size + 6;
        if (k->rx[4] != 0x0E || size < 2 || total > (int)sizeof(k->rx)) {
            memmove(k->rx, k->rx + 1, (size_t)(k->rx_len - 1));
            k->rx_len--;
            continue;
        }
        if (k->rx_len < total) { break; }
        for (i = 0; i < total; i++) { x = (lt_u8)(x ^ k->rx[i]); }
        if (x != 0 || k->rx[1] != k->seq) {             /* damaged, or an answer to an older try */
            memmove(k->rx, k->rx + 1, (size_t)(k->rx_len - 1));
            k->rx_len--;
            continue;
        }
        k->waiting = 0;
        handle(j, k->rx + 5, size);
        return;
    }
    if (j->now >= k->deadline) { on_timeout(j); }
}

static void stk_step(lt_fw_job *j)
{
    stk *k = (stk *)j->m;
    lt_u8 p[LT_FW_AVR_PAGE + 16];
    int r;
    if (k->waiting) { recv_step(j); return; }
    if (j->cancel) {
        int partial = j->phase >= LT_FW_PH_WRITE && (j->phase > LT_FW_PH_WRITE || j->done > 0);
        lt_fwj_phase(j, LT_FW_PH_CANCELLED, 0, NULL);
        lt_strlcpy(j->msg, partial ? "Canceled: the firmware is incomplete; the bootloader runs after a reset - "
                                     "flash again."
                                   : "Canceled.", sizeof(j->msg));
        lt_fwj_log(j, 2, "%s", j->msg);
        return;
    }
    switch (j->phase) {
    case LT_FW_PH_CONNECT:
        if (k->cstep == 0 || k->cstep == 1) {               /* the reset pulse */
            r = j->io->set_lines != NULL ? j->io->set_lines(j->io->ctx, k->cstep, k->cstep) : LT_USB_ENOTSUP;
            if (r < 0) {
                if (!k->no_lines) {
                    lt_fwj_log(j, 2, "This port cannot drive DTR/RTS: press the board's reset button now");
                }
                k->no_lines = 1;
                k->cstep = 2;
                return;
            }
            if (k->cstep == 0) { lt_fwj_log(j, 0, "Resetting the board (DTR/RTS pulse)"); }
            k->cstep++;
            j->next_ms = j->now + 50.0;
            return;
        }
        if (k->cstep == 2) { p[0] = CMD_SIGN_ON; send_cmd(j, p, 1, SYNC_MS); return; }
        if (k->cstep >= 3 && k->cstep <= 5) {
            p[0] = CMD_READ_SIGNATURE; p[1] = 0; p[2] = 0x30; p[3] = 0; p[4] = (lt_u8)(k->cstep - 3); p[5] = 0;
            send_cmd(j, p, 6, 1000);
            return;
        }
        /* timeout, stabDelay, cmdexeDelay, synchLoops, byteDelay, pollValue, pollIndex, programming enable */
        p[0] = CMD_ENTER_PROGMODE; p[1] = 200; p[2] = 100; p[3] = 25; p[4] = 32; p[5] = 0; p[6] = 0x53;
        p[7] = 3; p[8] = 0xAC; p[9] = 0x53; p[10] = 0; p[11] = 0;
        send_cmd(j, p, 12, 1000);
        return;
    case LT_FW_PH_WRITE:
    case LT_FW_PH_VERIFY:
        if (!k->addr_ok) { load_address(j, k->pages[k->pi]); return; }
        if (j->phase == LT_FW_PH_VERIFY) {
            p[0] = CMD_READ_FLASH; p[1] = (lt_u8)(PAGE >> 8); p[2] = (lt_u8)(PAGE & 0xFFu); p[3] = 0x20;
            send_cmd(j, p, 4, 1000);
            return;
        }
        fill_page(j, k);
        /* NumBytes, mode (page, write), delay, Load Page / Write Page / Read cmds, poll values */
        p[0] = CMD_PROGRAM_FLASH; p[1] = (lt_u8)(PAGE >> 8); p[2] = (lt_u8)(PAGE & 0xFFu); p[3] = 0xC1;
        p[4] = 10; p[5] = 0x40; p[6] = 0x4C; p[7] = 0x20; p[8] = 0xFF; p[9] = 0xFF;
        memcpy(p + 10, k->page, PAGE);
        send_cmd(j, p, 10 + (int)PAGE, 2000);
        return;
    case LT_FW_PH_LEAVE:
        if (!j->o.leave) {
            lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: firmware written; the bootloader starts it after its timeout "
                                              "or a reset.");
            return;
        }
        p[0] = CMD_LEAVE_PROGMODE; p[1] = 1; p[2] = 1;
        send_cmd(j, p, 3, 1000);
        return;
    default:
        return;
    }
}

static void stk_free(lt_fw_job *j)
{
    stk *k = (stk *)j->m;
    if (k == NULL) { return; }
    lt_free(k->pages);
    lt_free(k);
    j->m = NULL;
}

int lt_fwj_stk500_init(lt_fw_job *j)
{
    stk *k = (stk *)lt_calloc(1, sizeof(stk));
    if (k == NULL) { lt_fwj_fail(j, "out of memory"); return -1; }
    j->m = k;
    j->m_step = stk_step;
    j->m_free = stk_free;
    if (j->io->write == NULL || j->io->read == NULL) {
        lt_fwj_fail(j, "this connection cannot carry STK500 (it needs a serial port)");
        return -1;
    }
    if (j->o.dev_size == 0) { j->o.dev_size = LT_FW_MEGA2560_APP; }
    k->n_pages = lt_fwj_blocks(j, PAGE, &k->pages);
    if (k->n_pages < 0) { return -1; }
    j->w_total[LT_FW_PH_WRITE] = (lt_u32)k->n_pages * PAGE;
    j->w_total[LT_FW_PH_VERIFY] = j->o.verify ? (lt_u32)k->n_pages * PAGE : 0;
    return 0;
}
