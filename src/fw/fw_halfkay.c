/* fw_halfkay.c - PJRC HalfKay host (Teensy 3.5 / 3.6 / 4.1 bootloader, USB HID 16C0:0478),
 * following PJRC's teensy_loader_cli.
 *
 * The flash goes over in 1024-byte blocks, one HID SET_REPORT control transfer each
 * (bmRequestType 0x21, bRequest 9, wValue 0x0200, wIndex 0) of 1088 bytes: the block address
 * as 3 bytes little-endian, 61 zero bytes, then the 1024 data bytes (the layout PJRC uses for
 * every model with 512- or 1024-byte blocks, Teensy 3.x and 4.x alike). Addresses are flash
 * offsets: a Teensy 4.x .hex lies at the FlexSPI address 0x60000000, which is subtracted.
 * Block 0 is always sent first and makes HalfKay erase the whole flash, so the first five
 * blocks may take up to 45 s each (PJRC's timeout); later blocks get 0.5 s and every block is
 * retried every 10 ms until its time is up. Blocks the image does not touch, or that are all
 * 0xFF, are skipped (they read as erased). Address 0xFFFFFF reboots into the new firmware.
 * HalfKay cannot read the flash back, so there is no verify.
 * The interface is claimed first (Linux: usbhid is detached from it).
 * Each step does one transfer; a transfer that has to wait for the chip erase blocks for as
 * long as the erase takes (the USB layer has no asynchronous transfers), which the screen
 * announces before it starts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw_priv.h"

#define BLOCK 1024u
#define REPORT (BLOCK + 64u)
#define FIRST_MS 45000.0
#define NEXT_MS 500.0
#define RETRY_MS 10.0

typedef struct hk {
    lt_u32 *blocks;                 /* flash offsets to write, block 0 first */
    int n, bi, cstep;
    double deadline;                /* of the current block (0 = not started) */
    lt_u8 buf[REPORT];
} hk;

static void build(lt_fw_job *j, hk *h, lt_u32 addr)
{
    memset(h->buf, 0, REPORT);
    h->buf[0] = (lt_u8)(addr & 0xFFu);
    h->buf[1] = (lt_u8)((addr >> 8) & 0xFFu);
    h->buf[2] = (lt_u8)((addr >> 16) & 0xFFu);
    if (addr != 0xFFFFFFUL) { (void)lt_fw_image_read(j->img, j->o.dev_base + addr, h->buf + 64, BLOCK, 0xFF); }
}

static void hk_step(lt_fw_job *j)
{
    hk *h = (hk *)j->m;
    int r;
    if (j->cancel && h->deadline == 0.0) {
        lt_fwj_phase(j, LT_FW_PH_CANCELLED, 0, NULL);
        lt_strlcpy(j->msg, h->bi > 0 ? "Cancelled: the Teensy flash is incomplete; it stays in its bootloader - "
                                       "flash again."
                                     : "Cancelled.", sizeof(j->msg));
        lt_fwj_log(j, 2, "%s", j->msg);
        return;
    }
    switch (j->phase) {
    case LT_FW_PH_CONNECT:
        r = j->io->claim(j->io->ctx, 0, -1);
        if (r < 0) { lt_fwj_fail(j, "cannot claim the Teensy bootloader: %s", lt_fwj_usb_err(j, r)); return; }
        lt_fwj_phase(j, LT_FW_PH_WRITE, j->w_total[LT_FW_PH_WRITE],
                     "Erasing the Teensy and writing (the first block makes HalfKay erase the program flash: the window may "
                     "not respond for a few seconds)");
        j->next_ms = j->now + 60.0;             /* let the screen show that before the erase */
        return;
    case LT_FW_PH_WRITE: {
        double limit;
        if (h->deadline == 0.0) { h->deadline = j->now + (h->bi <= 4 ? FIRST_MS : NEXT_MS); }
        limit = h->deadline - j->now;
        if (limit < 1.0) { limit = 1.0; }
        build(j, h, h->blocks[h->bi]);
        r = j->io->control(j->io->ctx, 0x21, 9, 0x0200, 0, h->buf, (lt_u16)REPORT, (int)limit);
        if (r < 0) {
            if (r == LT_USB_ENODEV) { lt_fwj_fail(j, "the Teensy disappeared while writing: %s", lt_fwj_usb_err(j, r)); return; }
            if (j->now + RETRY_MS < h->deadline) { j->next_ms = j->now + RETRY_MS; return; }   /* again */
            lt_fwj_fail(j, "the Teensy did not take the block at 0x%06lX: %s", (unsigned long)h->blocks[h->bi],
                        lt_fwj_usb_err(j, r));
            return;
        }
        if (h->bi == 0) { lt_strlcpy(j->msg, "Writing", sizeof(j->msg)); }
        h->deadline = 0.0;
        h->bi++;
        j->done += BLOCK;
        if (h->bi >= h->n) {
            lt_fwj_phase(j, LT_FW_PH_LEAVE, 0, j->o.leave ? "Rebooting the Teensy into the firmware" : "Finishing");
        }
        return;
    }
    case LT_FW_PH_LEAVE:
        if (!j->o.leave) {
            lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: firmware written; the Teensy stays in its bootloader until it "
                                              "is reset or power-cycled.");
            return;
        }
        build(j, h, 0xFFFFFFUL);
        (void)j->io->control(j->io->ctx, 0x21, 9, 0x0200, 0, h->buf, (lt_u16)REPORT, 500);   /* it reboots */
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: the Teensy rebooted into the new firmware.");
        return;
    default:
        return;
    }
}

static void hk_free(lt_fw_job *j)
{
    hk *h = (hk *)j->m;
    if (h == NULL) { return; }
    lt_free(h->blocks);
    lt_free(h);
    j->m = NULL;
}

/* 1 when the image has a byte other than 0xFF in the block */
static int block_used(const lt_fw_job *j, lt_u32 addr, lt_u8 *tmp)
{
    lt_u32 i;
    if (lt_fw_image_read(j->img, j->o.dev_base + addr, tmp, BLOCK, 0xFF) == 0) { return 0; }
    for (i = 0; i < BLOCK; i++) {
        if (tmp[i] != 0xFF) { return 1; }
    }
    return 0;
}

int lt_fwj_halfkay_init(lt_fw_job *j)
{
    hk *h = (hk *)lt_calloc(1, sizeof(hk));
    lt_u32 *b = NULL;
    int n, i, k = 0;
    if (h == NULL) { lt_fwj_fail(j, "out of memory"); return -1; }
    j->m = h;
    j->m_step = hk_step;
    j->m_free = hk_free;
    if (j->io->control == NULL || j->io->claim == NULL) {
        lt_fwj_fail(j, "this connection cannot carry Teensy HalfKay (it needs USB)");
        return -1;
    }
    if (j->o.dev_size == 0) { lt_fwj_fail(j, "pick the Teensy model (its flash size is unknown)"); return -1; }
    n = lt_fwj_blocks(j, BLOCK, &b);
    if (n < 0) { return -1; }
    h->blocks = (lt_u32 *)lt_calloc((size_t)n + 1u, sizeof(lt_u32));
    if (h->blocks == NULL) { lt_free(b); lt_fwj_fail(j, "out of memory"); return -1; }
    h->blocks[k++] = 0;                         /* always first: it erases the chip */
    for (i = 0; i < n; i++) {
        if (b[i] != 0 && block_used(j, b[i], h->buf)) { h->blocks[k++] = b[i]; }
    }
    lt_free(b);
    h->n = k;
    j->w_total[LT_FW_PH_WRITE] = (lt_u32)k * BLOCK;
    j->w_total[LT_FW_PH_VERIFY] = 0;
    if (j->o.verify) { lt_fwj_log(j, 0, "HalfKay cannot read the flash back: no verify for Teensy"); }
    j->o.verify = 0;
    return 0;
}
