/* sim_cmd.h - the demo ECU's command decoder (internal to src/sim, used by sim_ecu.c).
 *
 * The INI declares every command the host will send: queryCommand, versionInfo, the realtime
 * ochGetCommand and burst getCommand, and per page pageReadCommand, pageValueWrite /
 * pageChunkWrite, burnCommand and crc32CheckCommand. They are compiled here into byte
 * patterns exactly as lt_cmd_build fills them in (escapes decoded, $tsCanId already
 * substituted by the INI layer, %2i = the page identifier as the host sends it, %2o / %2c =
 * 16-bit offset / count in the INI's byte order, %v = count data bytes), so the simulator
 * answers whatever dialect the INI speaks: rusEFI "R%2i%2o%2c", FOME "R%2o%2c", Speeduino
 * "p%2i%2o%2c", MegaSquirt 2 "r\x00\x04%2o%2c" / "w\x00\x04%2o%2c%v" / "b\x00\x04",
 * MegaSquirt 3 "r%2i%2o%2c" with "\$tsCanId\x04" identifiers and "k%2i\x00\x00\x00\x04",
 * MegaSquirt-1 "V" / "W%o%v" / "X%o%c%v" (%i / %o / %c: one byte) after a pageActivate "P\001".
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_SIM_CMD_H
#define LT_SIM_CMD_H

#include "../lt.h"
#include "../ini/ini.h"

typedef enum lt_simcmd_kind {
    LT_SIMCMD_NONE = 0, LT_SIMCMD_QUERY, LT_SIMCMD_VERSION, LT_SIMCMD_OCH, LT_SIMCMD_BURST,
    LT_SIMCMD_READ, LT_SIMCMD_WRITE, LT_SIMCMD_BURN, LT_SIMCMD_CRC, LT_SIMCMD_ACTIVATE
} lt_simcmd_kind;

typedef struct lt_simcmd_match {
    lt_simcmd_kind kind;
    int page;                    /* page index (page commands), else -1 */
    lt_bool has_off, has_cnt;    /* the command carried an offset / a count */
    size_t off, cnt;
    const lt_u8 *data;           /* %v bytes (inside the matched buffer) */
    size_t ndata;
    size_t len;                  /* bytes of the buffer the command takes */
} lt_simcmd_match;

typedef struct lt_simcmds lt_simcmds;
lt_simcmds *lt_simcmds_new(const lt_ini *ini, size_t n_pages);
void        lt_simcmds_free(lt_simcmds *c);
/* Match the command at the start of b[0..n). exact: the command must be all of b (one
 * enveloped payload). 1 = matched (m filled), 0 = no declared command starts this way,
 * -1 = one could, but more bytes are needed. */
int         lt_simcmds_match(const lt_simcmds *c, const lt_u8 *b, size_t n, lt_bool exact,
                             lt_simcmd_match *m);
/* The page the ECU has selected (pageActivate): a command declared identically for several
 * pages (MegaSquirt-1 Extra "V", "W%o%v", "B") is that page's. -1 = none (first declared). */
void        lt_simcmds_set_active(lt_simcmds *c, int page);
/* The page identifier value of page idx as the host's %2i carries it (proto.c page_id). */
lt_u32      lt_simcmds_page_id(const lt_ini *ini, size_t idx);

#endif
