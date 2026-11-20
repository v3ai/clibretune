/* fw_sim_priv.h - state shared by the simulated bootloaders (fw_sim.c: DfuSe, OpenBLT USB,
 * HalfKay; fw_sim_ser.c: STK500v2, OpenBLT UART and CAN behind an SLCAN adapter).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FW_SIM_PRIV_H
#define LT_FW_SIM_PRIV_H

#include "fw.h"

#define LT_FWSIM_IN 1200
#define LT_FWSIM_OUT 1400

struct lt_fw_sim {
    lt_fw_sim_opts o;
    lt_fw_clock_fn clock;
    void *cu;
    lt_u8 *flash;
    lt_u32 base, size;              /* flash geometry */
    int alt, left, claimed;
    /* DfuSe */
    int state, status;
    lt_u32 ptr, poll;
    double busy_until;
    lt_u8 pbuf[2048];
    int plen;
    lt_u16 pblock;
    int n_dnload, n_busyend, early, erases;
    /* OpenBLT (XCP slave): the pending answer, length byte first */
    lt_u8 resp[600];
    int resp_len, resp_pos;
    double resp_at;
    int connected, pgm;
    lt_u32 mta;
    /* HalfKay */
    int hk_erased, writes;
    /* serial bootloaders */
    lt_u8 in[LT_FWSIM_IN];          /* received, not parsed yet */
    int in_len;
    lt_u8 out[LT_FWSIM_OUT];        /* to be read by the host */
    int out_len, out_pos;
    int dtr, boot, resets, noise_left;
    double boot_until;
    lt_u32 avr_addr;
    int can_open, can_speed;
};

double lt_fwsim_now(const lt_fw_sim *s);
int  lt_fwsim_in_flash(const lt_fw_sim *s, lt_u32 addr, lt_u32 n);
/* program n bytes: only erased bytes may change (as on real flash); 0 or a DFU status code */
int  lt_fwsim_program(lt_fw_sim *s, lt_u32 addr, const lt_u8 *d, lt_u32 n);
/* one XCP command of the OpenBLT slave; the answer lands in resp (length byte + packet) */
void lt_fwsim_xcp(lt_fw_sim *s, const lt_u8 *c, int n);
/* fw_sim_ser.c */
void lt_fwsim_ser_init(lt_fw_sim *s);
void lt_fwsim_ser_io(lt_fw_sim *s, lt_fw_io *io);

#endif
