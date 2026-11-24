/* fw_job.c - the method-independent part of a flashing job: logging, phases, progress.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw_priv.h"

void lt_fwj_log(lt_fw_job *j, int level, const char *fmt, ...)
{
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    (void)lt_vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (j->log != NULL) { j->log(j->user, level, b); }
}

void lt_fwj_fail(lt_fw_job *j, const char *fmt, ...)
{
    va_list ap;
    if (lt_fwj_finished(j)) { return; }
    va_start(ap, fmt);
    (void)lt_vsnprintf(j->msg, sizeof(j->msg), fmt, ap);
    va_end(ap);
    j->phase = LT_FW_PH_FAILED;
    if (j->log != NULL) { j->log(j->user, 3, j->msg); }
}

void lt_fwj_phase(lt_fw_job *j, int phase, lt_u32 total, const char *msg)
{
    j->phase = phase;
    j->done = 0;
    j->goal = total;
    if (msg != NULL) {
        lt_strlcpy(j->msg, msg, sizeof(j->msg));
        if (j->log != NULL) { j->log(j->user, phase == LT_FW_PH_DONE ? 1 : 0, msg); }
    }
}

const char *lt_fwj_usb_err(lt_fw_job *j, int code)
{
    if (code < 0) { j->usb_err = code; }
    return lt_usb_strerror(code);
}

int lt_fwj_finished(const lt_fw_job *j)
{
    return j->phase == LT_FW_PH_DONE || j->phase == LT_FW_PH_FAILED || j->phase == LT_FW_PH_CANCELLED;
}

const lt_fw_range *lt_fwj_protected(const lt_fw_job *j, lt_u32 addr, lt_u32 len)
{
    int i;
    if (!j->o.keep_settings || len == 0) { return NULL; }
    for (i = 0; i < j->o.n_protect; i++) {
        const lt_fw_range *r = &j->o.protect[i];
        if (r->len != 0 && r->addr <= addr + (len - 1u) && addr <= r->addr + (r->len - 1u)) { return r; }
    }
    return NULL;
}

int lt_fwj_blocks(lt_fw_job *j, lt_u32 bsize, lt_u32 **out)
{
    const lt_fw_image *img = j->img;
    char err[200];
    lt_u32 *b;
    size_t i, cap = 0;
    int n = 0;
    *out = NULL;
    if (lt_fw_image_fits(img, j->o.dev_base, j->o.dev_size, err, sizeof(err)) != 0) {
        lt_fwj_fail(j, "%s", err);
        return -1;
    }
    for (i = 0; i < img->n_segs; i++) { cap += (size_t)(img->segs[i].len / bsize) + 2u; }
    b = (lt_u32 *)lt_calloc(cap, sizeof(lt_u32));
    if (b == NULL) { lt_fwj_fail(j, "out of memory"); return -1; }
    for (i = 0; i < img->n_segs; i++) {
        lt_u32 first = (img->segs[i].addr - j->o.dev_base) / bsize * bsize;
        lt_u32 last = (img->segs[i].addr - j->o.dev_base + (img->segs[i].len - 1u)) / bsize * bsize, a;
        for (a = first;; a += bsize) {
            if (n == 0 || b[n - 1] < a) { b[n++] = a; }
            if (a >= last) { break; }
        }
    }
    *out = b;
    return n;
}

lt_fw_job *lt_fw_job_new(int method, const lt_fw_io *io, const lt_fw_image *img, const lt_fw_opts *o,
                         lt_fw_log_fn log, void *user)
{
    lt_fw_job *j;
    if (io == NULL || img == NULL || o == NULL || img->n_segs == 0) { return NULL; }
    if (method < 0 || method >= LT_FW_METHOD_COUNT) { return NULL; }
    j = (lt_fw_job *)lt_calloc(1, sizeof(*j));
    if (j == NULL) { return NULL; }
    j->method = method;
    j->io = io;
    j->img = img;
    j->o = *o;
    j->log = log;
    j->user = user;
    lt_fwj_phase(j, LT_FW_PH_CONNECT, 0, NULL);
    lt_strlcpy(j->msg, "Connecting to the bootloader", sizeof(j->msg));
    switch (method) {
    case LT_FW_DFUSE: (void)lt_fwj_dfuse_init(j); break;
    case LT_FW_STK500: (void)lt_fwj_stk500_init(j); break;
    case LT_FW_HALFKAY: (void)lt_fwj_halfkay_init(j); break;
    default: (void)lt_fwj_xcp_init(j); break;
    }
    return j;
}

void lt_fw_job_free(lt_fw_job *j)
{
    if (j == NULL) { return; }
    if (j->m_free != NULL) { j->m_free(j); }
    lt_free(j);
}

int lt_fw_step(lt_fw_job *j, double now_ms)
{
    if (j == NULL || lt_fwj_finished(j)) { return 0; }
    j->now = now_ms;
    if (now_ms < j->next_ms) { return 1; }
    j->next_ms = 0.0;
    if (j->m_step != NULL) { j->m_step(j); }
    return lt_fwj_finished(j) ? 0 : 1;
}

double lt_fw_next_ms(const lt_fw_job *j)
{
    return j != NULL ? j->next_ms : 0.0;
}

void lt_fw_cancel(lt_fw_job *j)
{
    if (j != NULL && !lt_fwj_finished(j)) { j->cancel = 1; }
}

void lt_fw_job_progress(const lt_fw_job *j, lt_fw_progress *p)
{
    static const int order[4] = { LT_FW_PH_ERASE, LT_FW_PH_WRITE, LT_FW_PH_VERIFY, LT_FW_PH_LEAVE };
    static const double weight[4] = { 0.6, 1.0, 0.6, 0.0 };
    double sum = 0.0, acc = 0.0;
    int i;
    memset(p, 0, sizeof(*p));
    if (j == NULL) { return; }
    p->phase = j->phase;
    p->done = j->done;
    p->goal = j->goal;
    p->usb_err = j->usb_err;
    lt_strlcpy(p->msg, j->msg, sizeof(p->msg));
    for (i = 0; i < 4; i++) { sum += weight[i] * (double)j->w_total[order[i]]; }
    if (j->phase == LT_FW_PH_DONE) { p->frac = 1.0; return; }
    if (sum <= 0.0 || j->phase <= LT_FW_PH_CONNECT) { p->frac = 0.0; return; }
    for (i = 0; i < 4; i++) {
        double w = weight[i] * (double)j->w_total[order[i]];
        if (order[i] < j->phase || (j->phase > LT_FW_PH_LEAVE)) { acc += w; }
        else if (order[i] == j->phase && j->goal > 0) {
            acc += w * (double)(j->done < j->goal ? j->done : j->goal) / (double)j->goal;
        }
    }
    if (j->phase > LT_FW_PH_LEAVE) { acc = 0.0; }      /* failed / cancelled: no bar */
    p->frac = acc / sum;
}

const char *lt_fw_phase_name(int phase)
{
    switch (phase) {
    case LT_FW_PH_IDLE: return "idle";
    case LT_FW_PH_CONNECT: return "connect";
    case LT_FW_PH_ERASE: return "erase";
    case LT_FW_PH_WRITE: return "write";
    case LT_FW_PH_VERIFY: return "verify";
    case LT_FW_PH_LEAVE: return "leave";
    case LT_FW_PH_DONE: return "done";
    case LT_FW_PH_FAILED: return "failed";
    case LT_FW_PH_CANCELLED: return "canceled";
    default: return "?";
    }
}
