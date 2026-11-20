/* fw_priv.h - shared job state of the flashing engines (fw_job.c, fw_dfuse.c, fw_openblt.c,
 * fw_stk500.c, fw_halfkay.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FW_PRIV_H
#define LT_FW_PRIV_H

#include "fw.h"

struct lt_fw_job {
    int method;
    const lt_fw_io *io;
    const lt_fw_image *img;
    lt_fw_opts o;
    lt_fw_log_fn log;
    void *user;
    int phase, cancel;
    double now, next_ms;
    lt_u32 done, goal;              /* bytes of the current phase */
    lt_u32 w_total[LT_FW_PH_CANCELLED + 1];  /* planned bytes per phase (overall progress) */
    int usb_err;
    char msg[200];
    void *m;                        /* engine state */
    void (*m_step)(lt_fw_job *j);
    void (*m_free)(lt_fw_job *j);
};

void lt_fwj_log(lt_fw_job *j, int level, const char *fmt, ...);
/* finish with FAILED; msg is logged as an error and kept as the result */
void lt_fwj_fail(lt_fw_job *j, const char *fmt, ...);
/* enter a phase: resets done, sets total and the activity message */
void lt_fwj_phase(lt_fw_job *j, int phase, lt_u32 goal, const char *msg);
/* record a USB error code; returns the one-line text */
const char *lt_fwj_usb_err(lt_fw_job *j, int code);
int  lt_fwj_finished(const lt_fw_job *j);
/* protected range (keep_settings) overlapping [addr, addr + len), else NULL */
const lt_fw_range *lt_fwj_protected(const lt_fw_job *j, lt_u32 addr, lt_u32 len);

/* STK500 / HalfKay: device addresses (image address - o.dev_base) of the bsize blocks the
 * image touches, ascending, into a malloc'd *out; count, or -1 after lt_fwj_fail when the image
 * leaves [dev_base, dev_base + dev_size) */
int lt_fwj_blocks(lt_fw_job *j, lt_u32 bsize, lt_u32 **out);

int lt_fwj_dfuse_init(lt_fw_job *j);    /* 0, or -1 after lt_fwj_fail */
int lt_fwj_xcp_init(lt_fw_job *j);       /* OpenBLT over USB, UART and CAN */
int lt_fwj_stk500_init(lt_fw_job *j);
int lt_fwj_halfkay_init(lt_fw_job *j);

#endif
