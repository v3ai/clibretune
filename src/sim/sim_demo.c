/* sim_demo.c - cosmetic demo channel generator (no INI, no protocol).
 * Spec: docs/spec/08-sim-basemap-tableops.md section 5.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sim.h"

#include <math.h>
#include <string.h>

enum { B_IDLE = 0, B_UP, B_HOLD, B_DOWN };

#define DEMO_N 40

typedef struct demo_kv {
    const char *name;
    double v;
} demo_kv;

struct lt_demo {
    lt_u32 rng;
    unsigned long start_ms, last_ms, next_blip;
    int state;
    unsigned long state_start;
    double cur_rpm, blip_target;
    demo_kv kv[DEMO_N];
    size_t n;
};

static lt_u32 drng(lt_demo *d)
{
    lt_u32 x = d->rng & LT_U32_MASK;

    x ^= (x << 13) & LT_U32_MASK;
    x ^= x >> 17;
    x ^= (x << 5) & LT_U32_MASK;
    d->rng = x & LT_U32_MASK;
    return x;
}

static unsigned long urange(lt_demo *d, unsigned long lo, unsigned long hi)
{
    return lo + (unsigned long)(drng(d) % (lt_u32)(hi - lo));
}

lt_demo *lt_demo_new(unsigned long seed)
{
    lt_demo *d = (lt_demo *)lt_calloc(1, sizeof(*d));

    d->rng = (lt_u32)(seed & LT_U32_MASK);
    if (d->rng == 0) {
        d->rng = 0x2545F491UL;
    }
    d->next_blip = urange(d, 8000, 15000);
    d->cur_rpm = 850.0;
    return d;
}

void lt_demo_free(lt_demo *d)
{
    lt_free(d);
}

static double clampd(double x, double lo, double hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static void put(lt_demo *d, const char *name, double v)
{
    if (d->n < DEMO_N) {
        d->kv[d->n].name = name;
        d->kv[d->n].v = v;
        d->n++;
    }
}

void lt_demo_update(lt_demo *d, unsigned long elapsed_ms)
{
    unsigned long sim_time, delta;
    double target, t, rpm, tps_x, tps, map, afr, clt, adv, pw, p, wobble, rate, maxchg, diff;

    if (d->start_ms == 0) {
        d->start_ms = elapsed_ms;
    }
    sim_time = elapsed_ms >= d->start_ms ? elapsed_ms - d->start_ms : 0;
    delta = (d->last_ms > 0 && elapsed_ms > d->last_ms) ? elapsed_ms - d->last_ms : 0;
    d->last_ms = elapsed_ms;

    switch (d->state) {
    case B_IDLE:
        if (sim_time >= d->next_blip) {
            d->blip_target = (double)urange(d, 2000, 4000);
            d->state = B_UP;
            d->state_start = sim_time;
        }
        break;
    case B_UP:
        if (sim_time >= d->state_start + 300) {
            d->state = B_HOLD;
            d->state_start = sim_time;
        }
        break;
    case B_HOLD:
        if (sim_time >= d->state_start + 200) {
            d->state = B_DOWN;
            d->state_start = sim_time;
        }
        break;
    default:
        if (sim_time >= d->state_start + 800) {
            d->state = B_IDLE;
            d->next_blip = sim_time + urange(d, 8000, 15000);
        }
        break;
    }

    if (d->state == B_UP) {
        double f = (double)(sim_time - d->state_start) / 300.0;

        target = 850.0 + (d->blip_target - 850.0) * (f < 1.0 ? f : 1.0);
    } else if (d->state == B_HOLD) {
        target = d->blip_target;
    } else if (d->state == B_DOWN) {
        double f = (double)(sim_time - d->state_start) / 800.0;

        target = d->blip_target + (850.0 - d->blip_target) * (f < 1.0 ? f : 1.0);
    } else {
        target = 850.0;
    }
    rate = target > d->cur_rpm ? 8000.0 : 3000.0;
    maxchg = rate * (double)delta / 1000.0;
    diff = clampd(target - d->cur_rpm, -maxchg, maxchg);
    d->cur_rpm += diff;

    t = (double)sim_time / 1000.0;
    wobble = d->state == B_IDLE ? 20.0 * sin(t * 2.5) + 10.0 * sin(t * 7.3) : 0.0;
    rpm = d->cur_rpm + wobble;
    if (rpm < 0.0) {
        rpm = 0.0;
    }

    p = d->blip_target > 850.0 ? (d->cur_rpm - 850.0) / (d->blip_target - 850.0) : 0.0;
    if (d->state == B_UP) {
        tps_x = 2.0 + p * 45.0;
    } else if (d->state == B_HOLD) {
        tps_x = 45.0 + 5.0 * sin(t * 3.0);
    } else if (d->state == B_DOWN) {
        tps_x = 2.0 + p * 40.0;
    } else {
        tps_x = 1.5 + 1.0 * fabs(sin(t * 0.3));
    }
    tps = clampd(tps_x, 0.0, 100.0);
    map = rpm < 1000.0 ? 35.0 + 10.0 * (1.0 - rpm / 1000.0) : 25.0 + rpm / 100.0;
    map = clampd(map, 20.0, 105.0);
    afr = 14.7 + 0.3 * sin(t * 1.5) + 0.1 * sin(t * 4.7);
    clt = 20.0 + 70.0 * (1.0 - exp(-t / 120.0));
    adv = clampd(10.0 + rpm / 200.0 + 3.0 * sin(t * 0.7), -10.0, 45.0);
    pw = 2.5 + (rpm / 1000.0) * 1.5 + (tps_x / 50.0) * 2.0;

    d->n = 0;
    put(d, "rpm", rpm); put(d, "RPM", rpm);
    put(d, "tps", tps); put(d, "TPS", tps); put(d, "throttle", tps);
    put(d, "map", map); put(d, "MAP", map); put(d, "fuelLoad", map);
    put(d, "afr", afr); put(d, "AFR", afr); put(d, "AFRValue", afr);
    put(d, "lambda", afr / 14.7);
    put(d, "afrTarget", rpm < 1500.0 ? 14.7 : 13.5);
    put(d, "coolant", clt); put(d, "CLT", clt); put(d, "coolantTemperature", clt);
    put(d, "iat", 25.0 + 5.0 * sin(t * 0.1)); put(d, "IAT", 25.0 + 5.0 * sin(t * 0.1));
    put(d, "batteryVoltage", 13.8 + 0.2 * sin(t * 0.5));
    put(d, "vBatt", 13.8 + 0.2 * sin(t * 0.5));
    put(d, "advance", adv); put(d, "timing", adv);
    put(d, "pulseWidth", pw); put(d, "actualLastInjection", pw);
    {
        double ve = clampd(75.0 + rpm / 100.0 + 10.0 * (tps_x / 50.0), 0.0, 150.0);

        put(d, "VE1", ve); put(d, "veCurr", ve);
    }
    put(d, "egoCorrection", 100.0 + 5.0 * sin(t * 2.0));
    put(d, "hasSync", 1.0); put(d, "running", 1.0); put(d, "engine", 1.0);
    put(d, "dwell", 3.0 + 0.5 * (rpm / 2000.0));
    put(d, "baro", 101.0); put(d, "baroPressure", 101.0);
}

lt_bool lt_demo_get(const lt_demo *d, const char *name, double *out)
{
    size_t i;

    for (i = 0; i < d->n; i++) {
        if (strcmp(d->kv[i].name, name) == 0) {
            if (out) {
                *out = d->kv[i].v;
            }
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

static lt_bool demo_get_var(void *ud, const char *name, double *out)
{
    return lt_demo_get((const lt_demo *)ud, name, out);
}

void lt_demo_env(const lt_demo *d, lt_expr_env *env)
{
    lt_expr_env_init(env);
    env->ud = (void *)d;
    env->get_var = demo_get_var;
}
