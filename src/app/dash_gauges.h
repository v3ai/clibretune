/* dash_gauges.h - gauge painters (src/app/gauges.c) for the dashboard tab.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DASH_GAUGES_H
#define LT_DASH_GAUGES_H

#include "../dash/dash.h"

/* Register the painters with lt_dash_set_painters (idempotent). */
void lt_gauges_install(void);
/* Expression context used to resolve "{expr}" titles/units (may be NULL). */
void lt_gauges_set_env(lt_expr_cache *cache, const lt_expr_env *env);

#endif
