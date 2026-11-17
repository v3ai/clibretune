/* sim.h - demo mode: virtual ECU (engine model + memory + wire protocol),
 *         cosmetic channel generator.
 *
 * Spec: docs/spec/08-sim-basemap-tableops.md sections 5 (cosmetic demo), 6
 * (engine model, och codec, VE seeding) and 7 (virtual ECU wire protocol).
 *
 * DEMO MODE = the real app code path over a fake serial line:
 *     lt_ecusim *sim = lt_ecusim_new(ini);
 *     lt_proto_io io = lt_ecusim_io(sim);          (proto.h io vtable)
 *     lt_proto *p   = lt_proto_new(&io, ini, &cfg);
 *     lt_proto_connect(p);  lt_proto_read_all(...); lt_proto_get_realtime(...)
 * so proto, ini, tune and gui are exercised unchanged. The sim answers
 * handshake, page read/write/burn/CRC, realtime (O/A/r) in both legacy and
 * msEnvelope framing, honouring the INI's endianness and page identifiers.
 * The realtime block is produced by encoding engine state into the INI's
 * output channels (names rpm, map, coolant, tps, afr, ...).
 *
 * Everything is deterministic given the same call/tick sequence (xorshift RNG,
 * time injected via lt_ecusim_set_clock) so tests are repeatable.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_SIM_H
#define LT_SIM_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"
#include "../proto/proto.h"

/* ====================================================================== */
/* Engine model (spec 6): integer physics at 50 ms steps                   */
/* ====================================================================== */
typedef enum lt_sim_mode {
    LT_SIM_STARTUP = 0, LT_SIM_WARMUP_IDLE, LT_SIM_IDLE, LT_SIM_LIGHT_LOAD, LT_SIM_ACCELERATION,
    LT_SIM_HIGH_RPM, LT_SIM_DECELERATION, LT_SIM_WOT
} lt_sim_mode;

typedef struct lt_sim_snapshot {    /* physical values for the och codec */
    int secl, rpm, map_kpa, baro_kpa, coolant_c, iat_c, tps_percent, battery_dv, advance_deg;
    double afr, afr_target, ego_correction;
    lt_bool running, cranking;
} lt_sim_snapshot;

typedef struct lt_simengine lt_simengine;
lt_simengine *lt_simengine_new(void);                       /* seed 0x4F545531 */
void   lt_simengine_free(lt_simengine *e);
void   lt_simengine_tick(lt_simengine *e, unsigned dt_ms);  /* accumulates, steps every 50 ms */
void   lt_simengine_set_mode(lt_simengine *e, lt_sim_mode m);
lt_sim_mode lt_simengine_mode(const lt_simengine *e);
void   lt_simengine_reset_secl(lt_simengine *e);
void   lt_simengine_advance_secl(lt_simengine *e, unsigned d);
/* Optional VE context so the simulated AFR reacts to the tune's VE table (spec 6.1). */
void   lt_simengine_set_ve(lt_simengine *e, const double *rpm_bins, size_t nx, const double *load_bins,
                           size_t ny, const double *ve /* ny*nx */);   /* copies; nx or ny < 2 clears */
void   lt_simengine_snapshot(const lt_simengine *e, lt_sim_snapshot *out);
double lt_sim_true_ve(double rpm, double load);             /* 40 + 25*load/100 + 15*rpm/6000, clamp 20..110 */
/* Seed the INI's VE table in page images with 85% of the true VE (spec 6.1); LT_TRUE on success. */
lt_bool lt_sim_seed_ve_table(const lt_ini *ini, lt_pagesrc *dst);
/* Seed the AFR / lambda target table with stoichiometry and the warmup curve with a plain decay. */
lt_bool lt_sim_seed_afr_target(const lt_ini *ini, lt_pagesrc *dst);

/* och codec (spec 6.2): write snapshot values into a realtime block by channel names. */
size_t lt_sim_block_size(const lt_ini *ini);
void   lt_sim_encode_och(const lt_ini *ini, const lt_sim_snapshot *s, lt_u8 *block, size_t len);

/* ====================================================================== */
/* Virtual ECU (spec 7)                                                    */
/* ====================================================================== */
typedef struct lt_ecusim lt_ecusim;
/* ini may be NULL: handshake-only plain ECU with default signature. With an INI
 * the sim takes signature, version, query command, endianness, framing, page
 * sizes/identifiers and the och layout from it, and seeds the VE table. */
lt_ecusim *lt_ecusim_new(const lt_ini *ini);
void   lt_ecusim_free(lt_ecusim *s);
/* proto.h io over the sim (read: empty buffer = 0 bytes "would block", never EOF;
 * write: processes the bytes immediately). Valid while `s` lives. */
lt_proto_io lt_ecusim_io(lt_ecusim *s);
size_t lt_ecusim_write(lt_ecusim *s, const lt_u8 *data, size_t n);   /* (size_t)-1 if the link is dropped */
long   lt_ecusim_read(lt_ecusim *s, lt_u8 *buf, size_t max);          /* >=0 bytes; -1 dropped */
size_t lt_ecusim_bytes_to_read(const lt_ecusim *s);
void   lt_ecusim_clear_input(lt_ecusim *s);                           /* drop pending responses */
void   lt_ecusim_clear_output(lt_ecusim *s);                          /* drop pending commands */
void   lt_ecusim_set_clock(lt_ecusim *s, double (*now_ms)(void *ud), void *ud);  /* NULL = plat clock */
void   lt_ecusim_tick_engine(lt_ecusim *s, unsigned dt_ms);           /* manual tick; disables auto tick */
void   lt_ecusim_set_mode(lt_ecusim *s, lt_sim_mode m);
void   lt_ecusim_set_link_dropped(lt_ecusim *s, lt_bool dropped);     /* simulate unplugged cable */
void   lt_ecusim_advance_secl(lt_ecusim *s, unsigned d);
void   lt_ecusim_reboot(lt_ecusim *s);                                /* RAM <- flash (unburned writes lost) */
/* direct access for tests / GUI "ECU" inspector (RAM image of a page) */
const lt_u8 *lt_ecusim_ram(const lt_ecusim *s, int page, size_t *len);
const lt_u8 *lt_ecusim_flash(const lt_ecusim *s, int page, size_t *len);
lt_simengine *lt_ecusim_engine(lt_ecusim *s);

/* ====================================================================== */
/* Cosmetic demo channels (spec 5) - no INI, no protocol                   */
/* ====================================================================== */
typedef struct lt_demo lt_demo;
lt_demo *lt_demo_new(unsigned long seed);          /* seeded xorshift */
void     lt_demo_free(lt_demo *d);
void     lt_demo_update(lt_demo *d, unsigned long elapsed_ms);
lt_bool  lt_demo_get(const lt_demo *d, const char *name, double *out);    /* rpm,tps,map,afr,coolant,... aliases */
void     lt_demo_env(const lt_demo *d, lt_expr_env *env);                 /* get_var over the demo values */

#endif /* LT_SIM_H */
