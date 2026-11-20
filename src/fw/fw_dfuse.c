/* fw_dfuse.c - ST DfuSe 1.1a host (STM32 ROM bootloader in DFU mode, 0483:df11).
 *
 * DFU class requests on the DFU interface: DNLOAD=1 UPLOAD=2 GETSTATUS=3 CLRSTATUS=4
 * GETSTATE=5 ABORT=6. DfuSe commands are DNLOAD block 0: 0x21 + address (set the address
 * pointer), 0x41 + address (erase that page/sector), 0x41 alone (mass erase). Data blocks
 * wBlockNum >= 2 land at pointer + (block - 2) * wTransferSize; UPLOAD reads the same way.
 * Every DNLOAD is followed by GETSTATUS polls: dfuDNBUSY with bwPollTimeout (wait that long
 * before the next poll) until dfuDNLOAD_IDLE, or dfuERROR (cleared with CLRSTATUS).
 * Only the sectors the image touches are erased, after checking them against the memory
 * map of the interface string and against the protected ranges (ECU settings).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw_priv.h"

#define DFU_DNLOAD    1
#define DFU_UPLOAD    2
#define DFU_GETSTATUS 3
#define DFU_CLRSTATUS 4
#define DFU_ABORT     6
#define RT_OUT 0x21
#define RT_IN  0xA1

#define ST_APPIDLE      0
#define ST_APPDETACH    1
#define ST_IDLE         2
#define ST_DNBUSY       4
#define ST_DNLOAD_IDLE  5
#define ST_MANIFEST_SYNC 6
#define ST_MANIFEST     7
#define ST_ERROR        10

#define TIMEOUT_MS   5000
#define MAX_RETRIES  3
#define OP_LIMIT_MS  60000.0
#define MAX_ERASE    512

enum { TX_NONE = 0, TX_SEND, TX_POLL, TX_RECOVER };
enum { K_SETADDR = 0, K_ERASE, K_MASS, K_DATA, K_LEAVE, K_UPLOAD };

typedef struct dfs {
    int iface, alt, xfer;
    lt_fw_region rg[LT_FW_MAX_REGIONS];
    int n_rg;
    char mem[64];
    lt_u32 er_addr[MAX_ERASE], er_size[MAX_ERASE];
    int n_er, er_idx;
    size_t seg;
    lt_u32 off;
    lt_u32 ptr;
    int ptr_valid, in_upload, verify_ready, leave_ptr, cstep, cfix;
    int tx, kind, retries, rec_n;
    lt_u16 block, blen;
    lt_u32 tx_addr;
    double tx_t0;
    lt_u8 *buf, *rbuf;
} dfs;

/* ------------------------------------------------------------------ descriptor helpers */
static const char *skip_sp(const char *s)
{
    while (*s == ' ' || *s == '\t') { s++; }
    return s;
}

static int dig(int c) { return c >= '0' && c <= '9'; }

static int xdig(int c)
{
    if (dig(c)) { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

int lt_fw_dfuse_layout(const char *s, lt_fw_region *out, int cap, char *name, size_t namecap)
{
    int n = 0;
    size_t k = 0;
    if (s == NULL) { return -1; }
    s = skip_sp(s);
    if (*s != '@') { return -1; }
    s++;
    while (*s != '\0' && *s != '/') {
        if (name != NULL && k + 1 < namecap) { name[k++] = *s; }
        s++;
    }
    if (name != NULL && namecap > 0) {
        while (k > 0 && name[k - 1] == ' ') { k--; }
        name[k] = '\0';
    }
    while (*s == '/') {
        lt_u32 addr = 0;
        int nd = 0;
        s = skip_sp(s + 1);
        if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) { return -1; }
        s += 2;
        while (xdig((unsigned char)*s) >= 0) {
            if (++nd > 8) { return -1; }
            addr = (addr << 4) | (lt_u32)xdig((unsigned char)*s);
            s++;
        }
        if (nd == 0) { return -1; }
        s = skip_sp(s);
        if (*s != '/') { return -1; }
        s++;
        for (;;) {
            lt_u32 cnt = 0, size = 0, mult = 1;
            int nd2 = 0, type = 0;
            s = skip_sp(s);
            while (dig((unsigned char)*s)) {
                if (++nd2 > 6) { return -1; }
                cnt = cnt * 10u + (lt_u32)(*s - '0');
                s++;
            }
            if (nd2 == 0 || *s != '*') { return -1; }
            s++;
            nd2 = 0;
            while (dig((unsigned char)*s)) {
                if (++nd2 > 6) { return -1; }
                size = size * 10u + (lt_u32)(*s - '0');
                s++;
            }
            if (nd2 == 0) { return -1; }
            if (*s == 'K') { mult = 1024u; s++; }
            else if (*s == 'M') { mult = 1024u * 1024u; s++; }
            else if (*s == 'B' || *s == ' ') { s++; }
            if (*s >= 'a' && *s <= 'g') { type = *s - 'a' + 1; s++; }
            if (size > 0xFFFFFFFFUL / mult) { return -1; }
            size *= mult;
            if (cnt == 0 || size == 0) { return -1; }
            if (n >= cap) { return -1; }
            out[n].addr = addr;
            out[n].count = cnt;
            out[n].size = size;
            out[n].type = type;
            n++;
            if (cnt > 0xFFFFFFFFUL / size || cnt * size - 1u > 0xFFFFFFFFUL - addr) { return -1; }
            addr += cnt * size;
            s = skip_sp(s);
            if (*s != ',') { break; }
            s++;
        }
        s = skip_sp(s);
    }
    return n;
}

int lt_fw_dfuse_alts(const lt_u8 *cfg, int len, lt_fw_dfu_alt *out, int cap)
{
    int pos = 0, n = 0, i;
    while (pos + 2 <= len) {
        int bl = cfg[pos], ty = cfg[pos + 1];
        if (bl < 2 || pos + bl > len) { break; }
        if (ty == 4 && bl >= 9 && cfg[pos + 5] == 0xFE && cfg[pos + 6] == 0x01 && n < cap) {
            out[n].iface = cfg[pos + 2];
            out[n].alt = cfg[pos + 3];
            out[n].istr = cfg[pos + 8];
            out[n].xfer = 0;
            out[n].dfuse = 0;
            n++;
        } else if (ty == 0x21 && bl >= 7 && n > 0) {
            int xf = cfg[pos + 5] | (cfg[pos + 6] << 8);
            int ver = bl >= 9 ? (cfg[pos + 7] | (cfg[pos + 8] << 8)) : 0;
            for (i = 0; i < n; i++) {
                if (out[i].iface == out[n - 1].iface && out[i].xfer == 0) {
                    out[i].xfer = xf;
                    out[i].dfuse = ver == 0x011A;
                }
            }
        }
        pos += bl;
    }
    return n;
}

int lt_fw_dfuse_plan(const lt_fw_region *rg, int n_rg, const lt_fw_image *img,
                     const lt_fw_range *protect, int n_protect, lt_u32 *addr, lt_u32 *size, int cap,
                     char *err, size_t errcap)
{
    size_t si;
    int n = 0, i, p;
    if (err != NULL && errcap > 0) { err[0] = '\0'; }
    for (si = 0; si < img->n_segs; si++) {
        const lt_fw_seg *s = &img->segs[si];
        lt_u32 cur = s->addr, last = s->addr + (s->len - 1u);
        for (;;) {
            lt_u32 sa = 0, sz = 0;
            int found = 0, ty = 0;
            for (i = 0; i < n_rg && !found; i++) {
                if (cur >= rg[i].addr && (cur - rg[i].addr) / rg[i].size < rg[i].count) {
                    sz = rg[i].size;
                    sa = rg[i].addr + (cur - rg[i].addr) / sz * sz;
                    ty = rg[i].type;
                    found = 1;
                }
            }
            if (!found) {
                lt_snprintf(err, errcap, "address 0x%08lX is outside the device memory (wrong base address "
                            "or wrong device?)", (unsigned long)cur);
                return -1;
            }
            if ((ty & 4) == 0 || (ty & 2) == 0) {
                lt_snprintf(err, errcap, "address 0x%08lX is in a read-only area", (unsigned long)cur);
                return -1;
            }
            for (p = 0; p < n_protect; p++) {
                const lt_fw_range *r = &protect[p];
                if (r->len != 0 && r->addr <= sa + (sz - 1u) && sa <= r->addr + (r->len - 1u)) {
                    lt_snprintf(err, errcap, "the image needs the sector at 0x%08lX, which holds %s; untick "
                                "\"Keep ECU settings\" to allow it", (unsigned long)sa,
                                r->what != NULL ? r->what : "protected data");
                    return -1;
                }
            }
            if (n == 0 || addr[n - 1] != sa) {
                if (n >= cap) { lt_snprintf(err, errcap, "the image touches too many sectors"); return -1; }
                addr[n] = sa;
                size[n] = sz;
                n++;
            }
            if (sa + (sz - 1u) >= last || sa + (sz - 1u) < sa) { break; }
            cur = sa + sz;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ transactions */
static const char *status_text(int st)
{
    static const char *const t[16] = {
        "OK", "errTARGET (file not for this device)", "errFILE", "errWRITE (cannot write memory)",
        "errERASE (erase failed)", "errCHECK_ERASED", "errPROG (program failed)", "errVERIFY",
        "errADDRESS (address out of range)", "errNOTDONE", "errFIRMWARE (firmware corrupt)", "errVENDOR",
        "errUSBR (unexpected USB reset)", "errPOR (unexpected power-on reset)", "errUNKNOWN",
        "errSTALLEDPKT (request rejected)"
    };
    return st >= 0 && st < 16 ? t[st] : "unknown status";
}

static const char *kind_text(int k)
{
    switch (k) {
    case K_SETADDR: return "set address";
    case K_ERASE: return "erase";
    case K_MASS: return "mass erase";
    case K_DATA: return "write";
    case K_LEAVE: return "leave";
    default: return "read back";
    }
}

static int ctl(lt_fw_job *j, lt_u8 rt, lt_u8 req, lt_u16 value, lt_u8 *data, lt_u16 len)
{
    dfs *d = (dfs *)j->m;
    return j->io->control(j->io->ctx, rt, req, value, (lt_u16)d->iface, data, len, TIMEOUT_MS);
}

static void start_tx(lt_fw_job *j, int kind, lt_u32 addr, lt_u16 block, const lt_u8 *data, lt_u16 len)
{
    dfs *d = (dfs *)j->m;
    d->kind = kind;
    d->tx_addr = addr;
    d->block = block;
    d->blen = len;
    if (len > 0) { memcpy(d->buf, data, len); }
    d->tx = TX_SEND;
    d->tx_t0 = j->now;
}

static void cmd_tx(lt_fw_job *j, int kind, lt_u8 cmd, lt_u32 addr, int with_addr)
{
    lt_u8 b[5];
    b[0] = cmd;
    lt_store_u32le(b + 1, addr);
    start_tx(j, kind, addr, 0, b, (lt_u16)(with_addr ? 5 : 1));
}

static void after_phase(lt_fw_job *j);

/* a transaction failed with a USB error or a device error status: retry after recovering */
static void tx_error(lt_fw_job *j, int code, int dev_status)
{
    dfs *d = (dfs *)j->m;
    char why[120];
    if (d->kind == K_LEAVE) {
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: the device left DFU mode and starts the firmware.");
        return;
    }
    if (code == LT_USB_ENODEV || code == LT_USB_EACCES || code == LT_USB_ENOTSUP || code == LT_USB_EBUSY ||
        code == LT_USB_ENOMEM) {
        lt_fwj_fail(j, "%s at 0x%08lX failed: %s", kind_text(d->kind), (unsigned long)d->tx_addr,
                    lt_fwj_usb_err(j, code));
        return;
    }
    if (code < 0) { lt_snprintf(why, sizeof(why), "%s", lt_fwj_usb_err(j, code)); }
    else { lt_snprintf(why, sizeof(why), "device reports %s", status_text(dev_status)); }
    d->retries++;
    if (d->retries > MAX_RETRIES) {
        lt_fwj_fail(j, "%s at 0x%08lX failed after %d retries: %s", kind_text(d->kind),
                    (unsigned long)d->tx_addr, MAX_RETRIES, why);
        return;
    }
    lt_fwj_log(j, 2, "%s at 0x%08lX: %s - recovering and retrying", kind_text(d->kind),
               (unsigned long)d->tx_addr, why);
    d->tx = TX_RECOVER;
    d->rec_n = 0;
    d->ptr_valid = 0;
    d->in_upload = 0;
    d->verify_ready = 0;
    d->leave_ptr = 0;
}

static void tx_done(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    d->tx = TX_NONE;
    switch (d->kind) {
    case K_SETADDR:
        d->ptr = d->tx_addr;
        d->ptr_valid = 1;
        if (j->phase == LT_FW_PH_LEAVE) { d->leave_ptr = 1; }
        break;
    case K_ERASE:
        j->done += d->er_size[d->er_idx];
        d->er_idx++;
        d->retries = 0;
        break;
    case K_MASS:
        d->er_idx = d->n_er;
        j->done = j->total;
        d->retries = 0;
        break;
    case K_DATA:
        d->off += d->blen;
        j->done += d->blen;
        d->retries = 0;
        break;
    default:
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: the device left DFU mode and starts the firmware.");
        break;
    }
}

static void tx_send(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    int r = ctl(j, RT_OUT, DFU_DNLOAD, d->block, d->blen > 0 ? d->buf : NULL, d->blen);
    if (r < 0) { tx_error(j, r, 0); return; }
    d->tx = TX_POLL;
}

static void tx_poll(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    lt_u8 st[6];
    lt_u32 poll;
    int r = ctl(j, RT_IN, DFU_GETSTATUS, 0, st, 6);
    if (r < 0) { tx_error(j, r, 0); return; }
    if (r < 6) { tx_error(j, LT_USB_EIO, 0); return; }
    poll = (lt_u32)st[1] | ((lt_u32)st[2] << 8) | ((lt_u32)st[3] << 16);
    if (d->kind == K_LEAVE) {
        tx_done(j);
        return;
    }
    if (st[0] != 0 || st[4] == ST_ERROR) { tx_error(j, 0, st[0]); return; }
    if (st[4] == ST_DNLOAD_IDLE || st[4] == ST_IDLE) { tx_done(j); return; }
    if (j->now - d->tx_t0 > OP_LIMIT_MS) {
        lt_fwj_fail(j, "%s at 0x%08lX: the device stayed busy for more than a minute", kind_text(d->kind),
                    (unsigned long)d->tx_addr);
        return;
    }
    /* dfuDNBUSY (or still syncing): poll again after bwPollTimeout */
    j->next_ms = j->now + (double)(poll > 0 ? poll : (st[4] == ST_DNBUSY ? 1u : 5u));
}

/* back to dfuIDLE after an error: GETSTATUS, then CLRSTATUS or ABORT as the state needs */
static void recover_step(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    lt_u8 st[6];
    int r;
    if (++d->rec_n > 8) {
        lt_fwj_fail(j, "the device does not recover from the error (unplug it and start over)");
        return;
    }
    r = ctl(j, RT_IN, DFU_GETSTATUS, 0, st, 6);
    if (r == LT_USB_ENODEV) { lt_fwj_fail(j, "the device disconnected"); return; }
    if (r < 6) { (void)ctl(j, RT_OUT, DFU_CLRSTATUS, 0, NULL, 0); return; }
    if (st[4] == ST_ERROR) { (void)ctl(j, RT_OUT, DFU_CLRSTATUS, 0, NULL, 0); return; }
    if (st[4] == ST_DNBUSY) {
        lt_u32 poll = (lt_u32)st[1] | ((lt_u32)st[2] << 8) | ((lt_u32)st[3] << 16);
        j->next_ms = j->now + (double)(poll > 0 ? poll : 1u);
        return;
    }
    if (st[4] == ST_IDLE) { d->tx = TX_NONE; return; }   /* the phase logic repeats the operation */
    (void)ctl(j, RT_OUT, DFU_ABORT, 0, NULL, 0);
}

/* ------------------------------------------------------------------ phases */
static void connect_step(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    if (d->cstep == 0) {
        lt_u8 cfg[1024];
        lt_fw_dfu_alt alts[8];
        char s[256], err[200];
        int n, i, r, pick = -1;
        r = j->io->config_desc(j->io->ctx, cfg, (int)sizeof(cfg));
        if (r < 0) { lt_fwj_fail(j, "cannot read the USB descriptors: %s", lt_fwj_usb_err(j, r)); return; }
        n = lt_fw_dfuse_alts(cfg, r, alts, 8);
        if (n == 0) { lt_fwj_fail(j, "no DFU interface on this device (is it in DFU mode?)"); return; }
        for (i = 0; i < n && pick < 0; i++) {
            s[0] = '\0';
            if (alts[i].istr > 0 && j->io->string_desc(j->io->ctx, alts[i].istr, s, sizeof(s)) > 0 &&
                lt_str_istarts(skip_sp(s), "@Internal Flash")) {
                pick = i;
            }
        }
        if (pick < 0) {
            pick = 0;
            s[0] = '\0';
            if (alts[0].istr > 0) { (void)j->io->string_desc(j->io->ctx, alts[0].istr, s, sizeof(s)); }
        }
        d->iface = alts[pick].iface;
        d->alt = alts[pick].alt;
        d->xfer = alts[pick].xfer > 0 ? alts[pick].xfer : 2048;
        if (d->xfer > 4096) { d->xfer = 4096; }
        d->n_rg = lt_fw_dfuse_layout(s, d->rg, LT_FW_MAX_REGIONS, d->mem, sizeof(d->mem));
        if (d->n_rg <= 0) {
            lt_fwj_fail(j, "the device does not describe its memory (interface string \"%.60s\")", s);
            return;
        }
        d->buf = (lt_u8 *)lt_malloc((size_t)d->xfer);
        d->rbuf = (lt_u8 *)lt_malloc((size_t)d->xfer);
        if (d->buf == NULL || d->rbuf == NULL) { lt_fwj_fail(j, "out of memory"); return; }
        if (j->o.mass_erase) {
            if (j->o.keep_settings && j->o.n_protect > 0) {
                lt_fwj_fail(j, "mass erase would wipe the ECU settings; untick \"Keep ECU settings\" first");
                return;
            }
            d->n_er = 1;
            d->er_addr[0] = d->rg[0].addr;
            d->er_size[0] = 0;
            for (i = 0; i < d->n_rg; i++) { d->er_size[0] += d->rg[i].count * d->rg[i].size; }
        } else {
            d->n_er = lt_fw_dfuse_plan(d->rg, d->n_rg, j->img, j->o.keep_settings ? j->o.protect : NULL,
                                       j->o.keep_settings ? j->o.n_protect : 0, d->er_addr, d->er_size,
                                       MAX_ERASE, err, sizeof(err));
            if (d->n_er < 0) { lt_fwj_fail(j, "%s", err); return; }
        }
        j->w_total[LT_FW_PH_ERASE] = 0;
        for (i = 0; i < d->n_er; i++) { j->w_total[LT_FW_PH_ERASE] += d->er_size[i]; }
        j->w_total[LT_FW_PH_WRITE] = j->img->total;
        j->w_total[LT_FW_PH_VERIFY] = j->o.verify ? j->img->total : 0;
        lt_fwj_log(j, 0, "DFU device: %s, interface %d alt %d, transfer size %d", d->mem[0] != '\0' ? d->mem : "memory",
                   d->iface, d->alt, d->xfer);
        r = j->io->claim(j->io->ctx, d->iface, d->alt);
        if (r < 0) { lt_fwj_fail(j, "cannot claim the DFU interface: %s", lt_fwj_usb_err(j, r)); return; }
        d->cstep = 1;
        return;
    }
    {
        lt_u8 st[6];
        int r = ctl(j, RT_IN, DFU_GETSTATUS, 0, st, 6);
        if (++d->cfix > 6) { lt_fwj_fail(j, "the device does not reach the DFU idle state"); return; }
        if (r < 0) {
            if (r == LT_USB_EPIPE || r == LT_USB_ETIMEOUT) { (void)ctl(j, RT_OUT, DFU_CLRSTATUS, 0, NULL, 0); return; }
            lt_fwj_fail(j, "GETSTATUS failed: %s", lt_fwj_usb_err(j, r));
            return;
        }
        if (r < 6) { lt_fwj_fail(j, "short GETSTATUS reply"); return; }
        if (st[4] == ST_APPIDLE || st[4] == ST_APPDETACH) {
            lt_fwj_fail(j, "the device is in run-time mode, not in its DFU bootloader");
            return;
        }
        if (st[4] == ST_ERROR) { (void)ctl(j, RT_OUT, DFU_CLRSTATUS, 0, NULL, 0); return; }
        if (st[4] != ST_IDLE) { (void)ctl(j, RT_OUT, DFU_ABORT, 0, NULL, 0); return; }
        {
            char m[160];
            lt_snprintf(m, sizeof(m), "Erasing %d sector%s (%lu KB)", d->n_er, d->n_er == 1 ? "" : "s",
                        (unsigned long)(j->w_total[LT_FW_PH_ERASE] / 1024u));
            lt_fwj_phase(j, LT_FW_PH_ERASE, j->w_total[LT_FW_PH_ERASE], m);
        }
    }
}

static void enter_write(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    char m[120];
    d->seg = 0;
    d->off = 0;
    d->ptr_valid = 0;
    lt_snprintf(m, sizeof(m), "Writing %lu bytes", (unsigned long)j->img->total);
    lt_fwj_phase(j, LT_FW_PH_WRITE, j->img->total, m);
}

/* the next data block of the cursor: address and length; 0 when all segments are done */
static int cursor(lt_fw_job *j, lt_u32 *addr, lt_u32 *n)
{
    dfs *d = (dfs *)j->m;
    while (d->seg < j->img->n_segs && d->off >= j->img->segs[d->seg].len) {
        d->seg++;
        d->off = 0;
        d->verify_ready = 0;
    }
    if (d->seg >= j->img->n_segs) { return 0; }
    *addr = j->img->segs[d->seg].addr + d->off;
    *n = j->img->segs[d->seg].len - d->off;
    if (*n > (lt_u32)d->xfer) { *n = (lt_u32)d->xfer; }
    return 1;
}

/* Block number for n bytes at addr from the current pointer, or 0 when the pointer must be
 * set first. ST computes pointer + (block - 2) * wLength, so a short block is only sent as
 * block 2 right at the pointer. */
static lt_u16 block_for(const dfs *d, lt_u32 addr, lt_u32 n)
{
    lt_u32 q;
    if (!d->ptr_valid || addr < d->ptr || (addr - d->ptr) % (lt_u32)d->xfer != 0) { return 0; }
    if (n < (lt_u32)d->xfer && addr != d->ptr) { return 0; }
    q = (addr - d->ptr) / (lt_u32)d->xfer;
    if (q > 0xFFFDu) { return 0; }
    return (lt_u16)(q + 2u);
}

static void write_step(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    lt_u32 addr = 0, n = 0;
    lt_u16 blk;
    if (!cursor(j, &addr, &n)) { after_phase(j); return; }
    blk = block_for(d, addr, n);
    if (blk == 0) { cmd_tx(j, K_SETADDR, 0x21, addr, 1); return; }
    start_tx(j, K_DATA, addr, blk, j->img->segs[d->seg].data + d->off, (lt_u16)n);
}

static void verify_step(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    lt_u32 addr = 0, n = 0;
    lt_u16 blk;
    int r;
    if (!cursor(j, &addr, &n)) { after_phase(j); return; }
    blk = d->verify_ready ? block_for(d, addr, n) : 0;
    if (blk == 0) {
        if (d->in_upload) {                 /* DNLOAD is not allowed in dfuUPLOAD_IDLE */
            (void)ctl(j, RT_OUT, DFU_ABORT, 0, NULL, 0);
            d->in_upload = 0;
            return;
        }
        if (!d->ptr_valid || d->ptr != addr) {
            d->ptr_valid = 0;
            cmd_tx(j, K_SETADDR, 0x21, addr, 1);
            return;
        }
        r = ctl(j, RT_OUT, DFU_ABORT, 0, NULL, 0);   /* dfuDNLOAD_IDLE -> dfuIDLE for UPLOAD */
        if (r < 0) { d->kind = K_UPLOAD; d->tx_addr = addr; tx_error(j, r, 0); return; }
        d->verify_ready = 1;
        return;
    }
    r = ctl(j, RT_IN, DFU_UPLOAD, blk, d->rbuf, (lt_u16)n);
    if (r < 0 || (lt_u32)r != n) {
        d->kind = K_UPLOAD;
        d->tx_addr = addr;
        tx_error(j, r < 0 ? r : LT_USB_EIO, 0);
        return;
    }
    d->in_upload = 1;
    if (memcmp(d->rbuf, j->img->segs[d->seg].data + d->off, (size_t)n) != 0) {
        lt_u32 k = 0;
        while (k < n && d->rbuf[k] == j->img->segs[d->seg].data[d->off + k]) { k++; }
        lt_fwj_fail(j, "Verify failed at 0x%08lX: flash holds 0x%02X, the image 0x%02X", (unsigned long)(addr + k),
                    (unsigned)d->rbuf[k], (unsigned)j->img->segs[d->seg].data[d->off + k]);
        return;
    }
    d->off += n;
    j->done += n;
    d->retries = 0;
}

static void leave_step(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    if (d->in_upload) {
        (void)ctl(j, RT_OUT, DFU_ABORT, 0, NULL, 0);
        d->in_upload = 0;
        return;
    }
    if (!j->o.leave) {
        (void)ctl(j, RT_OUT, DFU_ABORT, 0, NULL, 0);
        lt_fwj_phase(j, LT_FW_PH_DONE, 0, "Done: firmware written; the device stays in DFU mode "
                                          "(reset it to start the firmware).");
        return;
    }
    if (!d->leave_ptr) { cmd_tx(j, K_SETADDR, 0x21, j->img->lo, 1); return; }
    start_tx(j, K_LEAVE, j->img->lo, 2, NULL, 0);
}

static void after_phase(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    if (j->phase == LT_FW_PH_ERASE) { enter_write(j); return; }
    if (j->phase == LT_FW_PH_WRITE && j->o.verify) {
        d->seg = 0;
        d->off = 0;
        d->verify_ready = 0;
        lt_fwj_phase(j, LT_FW_PH_VERIFY, j->img->total, "Verifying (reading the flash back)");
        return;
    }
    if (j->phase == LT_FW_PH_VERIFY) { lt_fwj_log(j, 1, "Verify OK: the flash matches the image"); }
    d->leave_ptr = 0;
    lt_fwj_phase(j, LT_FW_PH_LEAVE, 0, j->o.leave ? "Starting the firmware" : "Finishing");
}

static void dfuse_step(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    if (d->tx == TX_RECOVER) { recover_step(j); return; }
    if (d->tx == TX_SEND) { tx_send(j); return; }
    if (d->tx == TX_POLL) { tx_poll(j); return; }
    if (j->cancel) {
        if (d->cstep > 0) { (void)ctl(j, RT_OUT, DFU_ABORT, 0, NULL, 0); }
        lt_fwj_phase(j, LT_FW_PH_CANCELLED, 0, NULL);
        lt_strlcpy(j->msg, j->done > 0 || j->w_total[LT_FW_PH_ERASE] > 0
                   ? "Cancelled: the flash is incomplete - flash again before starting the ECU."
                   : "Cancelled.", sizeof(j->msg));
        lt_fwj_log(j, 2, "%s", j->msg);
        return;
    }
    switch (j->phase) {
    case LT_FW_PH_CONNECT: connect_step(j); break;
    case LT_FW_PH_ERASE:
        if (d->er_idx >= d->n_er) { after_phase(j); }
        else if (j->o.mass_erase) { cmd_tx(j, K_MASS, 0x41, 0, 0); }
        else { cmd_tx(j, K_ERASE, 0x41, d->er_addr[d->er_idx], 1); }
        break;
    case LT_FW_PH_WRITE: write_step(j); break;
    case LT_FW_PH_VERIFY: verify_step(j); break;
    case LT_FW_PH_LEAVE: leave_step(j); break;
    default: break;
    }
}

static void dfuse_free(lt_fw_job *j)
{
    dfs *d = (dfs *)j->m;
    if (d == NULL) { return; }
    lt_free(d->buf);
    lt_free(d->rbuf);
    lt_free(d);
    j->m = NULL;
}

int lt_fwj_dfuse_init(lt_fw_job *j)
{
    dfs *d = (dfs *)lt_calloc(1, sizeof(dfs));
    if (d == NULL) { lt_fwj_fail(j, "out of memory"); return -1; }
    j->m = d;
    j->m_step = dfuse_step;
    j->m_free = dfuse_free;
    return 0;
}
