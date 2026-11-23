# 07 - AutoTune (VE Analyze) Specification

Source: `libretune-core/src/autotune/{mod,declared_filters,preflight,replay,delay_measure,accel_enrich,anomaly,health,predictor}.rs`
and tests `tests/autotune_heatmap.rs`, `tests/autotune_preflight.rs`.
Target: C89, `lt_` prefix, `double` for all f64, no dynamic evalexpr (see custom filter).

Conventions
- Tables are `[row][col]`; row = y = load axis (`y_bins`), col = x = rpm axis (`x_bins`).
  Recommendation coords are `(cell_x = col, cell_y = row)`; reference tables are indexed `[cell_y][cell_x]`.
- All timestamps are integer ms (`uint64`-like; use `unsigned long` + care, or `double` in C89 where 64-bit is unavailable).
- Rust `as u64` on f64 truncates toward zero and saturates (negative -> 0). Replicate with a helper `lt_f2u`.
- Rust `f64::clamp(lo,hi)` panics if lo>hi or NaN; C port must never crash: order lo/hi, treat non-finite as "skip clamp" as noted below.
- OUT OF SCOPE: tracing/logging (drop or stub), serde/JSON (ports define their own I/O), Tauri command glue, LLM/agent, evalexpr crate (custom filter expression: see 3.8; implement a tiny expression evaluator or omit).
- "Heatmap data": there is NO heatmap code in core. The UI heat maps are derived from recommendation fields:
  Cell Weighting = `hit_weighting` (or hit_count / replay `coverage`), Cell Change = `recommended_value - beginning_value`.
  `tests/autotune_heatmap.rs` is just the recommendation-accumulation test suite (section 12).

---------------------------------------------------------------------------
## 1. Constants

```
LT_STOICH_AFR            14.7
LT_LAMBDA_AFR_THRESHOLD  2.0     /* value < 2.0 is lambda, else AFR */
LT_AFR_RAIL_LOW          10.0    /* inclusive valid range [10.0, 19.5] */
LT_AFR_RAIL_HIGH         19.5
LT_STEADY_RPM_TOLERANCE  100.0
LT_STEADY_LOAD_TOLERANCE 3.0
LT_CENTRE_ONLY_MIN_WEIGHT 0.75
```
`lt_normalise_to_afr(v) = (v < 2.0) ? v*14.7 : v`. Applied to BOTH the target table value and (upstream, caller) the measured sensor value. The engine itself assumes `VEDataPoint.afr` is already AFR.

---------------------------------------------------------------------------
## 2. Data structures

```c
typedef enum { LT_HW_UNIFORM=0, LT_HW_CELL_PROXIMITY, LT_HW_CELL_PROXIMITY_SQUARED, LT_HW_CELL_CENTRE_ONLY } lt_hit_weighting;
/* serde names: "uniform","cell_proximity","cell_proximity_squared","cell_centre_only" */

typedef struct {            /* AutoTuneSettings; defaults in () */
  double target_afr;        /* (14.7) fallback target */
  char   algorithm[16];     /* ("simple") unused by math */
  unsigned update_rate_ms;  /* (100) unused by math */
  double lambda_delay_ms;   /* (0.0) >0 = fixed transport delay; 0 = auto (per-cell table else RPM curve) */
  int    lambda_delay_flow_scaled; /* (0) caller builds table via lt_compute_flow_scaled_delay_table */
  double lambda_delay_floor_ms;    /* (120.0) */
  lt_hit_weighting hit_weighting;  /* (UNIFORM) */
  double base_weight;       /* (20.0) confidence ramp; <=0 disables */
  double min_change;        /* (1.0) smallest proposed change in table units */
} lt_at_settings;

typedef struct {            /* AutoTuneAuthorityLimits */
  double max_cell_value_change;      /* (10.0) abs units per update. JSON aliases: max_change_per_cell */
  double max_cell_percentage_change; /* (20.0) % of beginning_value.   alias: max_total_change */
  double min_cell_value;             /* (0.0)   alias: min_value */
  double max_cell_value;             /* (255.0) alias: max_value */
} lt_at_authority;

typedef struct {            /* AutoTuneFilters */
  double min_rpm;           /* (1000) */
  double max_rpm;           /* (7000) */
  char  *min_y_axis;        /* (NULL) numeric string; parsed w/ trim; unparsable => ignored */
  char  *max_y_axis;        /* (NULL) */
  double min_clt;           /* (160.0) Fahrenheit-shaped default; app substitutes INI's value when == 160.0 exactly */
  char  *custom_filter;     /* (NULL) boolean/number expression */
  double max_tps_rate;      /* (50.0) %/s, compared with fabs() */
  int    exclude_accel_enrich; /* (1) */
  unsigned long min_steady_ms; /* (0) 0 disables */
} lt_at_filters;

typedef struct {            /* AutoTuneReferenceTables; empty (rows==0) = not supplied */
  double **lambda_delay_table; /* [row][col] ms */
  double **target_afr_table;   /* [row][col] AFR or lambda */
  double **ve_table;           /* [row][col] the table being tuned */
  /* + row/col counts per table; accessor must bounds-check [y][x] and return "absent" */
} lt_at_reftables;

typedef struct {            /* VEDataPoint */
  double rpm, map, maf, load, afr /*(14.7)*/, ve, clt, tps, tps_rate;
  int accel_enrich_active;  /* tri-state: -1 unknown(None), 0 false, 1 true */
  int fuel_cut_active;      /* tri-state */
  unsigned long timestamp_ms;
} lt_ve_point;              /* Default: all 0 except afr=14.7, flags unknown */

typedef struct {            /* AutoTuneRecommendation */
  unsigned cell_x, cell_y;
  double beginning_value, recommended_value;
  unsigned hit_count;
  double hit_weighting;     /* accumulated weight */
  double target_afr;
  double hit_percentage;
  double raw_required_cma;  /* internal (serde skip) */
} lt_at_rec;

typedef struct lt_at_state {
  int is_running;
  char *table_name;                 /* kept after stop */
  /* set of locked (x,y) cells (hash set or sorted array; must be set semantics) */
  /* map (x,y)->lt_at_rec */
  /* ring buffer / deque of lt_ve_point */
  unsigned long buffer_max_age_ms;  /* (500) */
  lt_at_reftables ref;
  int strict_lambda_match;          /* (1) */
  unsigned long total_samples;
  unsigned long rejects_logged;     /* log-throttling only; may drop */
  /* map reason-string -> count */
} lt_at_state;
```

Public API
```c
void lt_at_init(lt_at_state*);           /* Default state */
void lt_at_start(lt_at_state*);          /* is_running=1; clear recommendations, data_buffer, total_samples=0, rejection counts. Does NOT clear locked_cells, ref tables, strict flag, table_name */
void lt_at_stop(lt_at_state*);           /* is_running=0 only */
void lt_at_set_reference_tables(lt_at_state*, const lt_at_reftables*);
void lt_at_set_strict_lambda_match(lt_at_state*, int);
int  lt_at_is_cell_locked(const lt_at_state*, unsigned x, unsigned y);
void lt_at_lock_cells(lt_at_state*, const unsigned (*cells)[2], size_t n);   /* set insert (idempotent) */
void lt_at_unlock_cells(lt_at_state*, ...);                                   /* set remove */
int  lt_at_classify(const lt_at_state*, const lt_ve_point*, const lt_at_filters*, const char **reason_out); /* 0 ok, else reason */
int  lt_at_passes_filters(const lt_at_state*, const lt_ve_point*, const lt_at_filters*);
void lt_at_add_data_point(lt_at_state*, const lt_ve_point*, const double *xbins, size_t nx, const double *ybins, size_t ny, const lt_at_settings*, const lt_at_filters*, const lt_at_authority*);
size_t lt_at_get_recommendations(const lt_at_state*, lt_at_rec *out, size_t cap); /* order unspecified (HashMap); sort (y,x) for determinism */
unsigned long lt_at_total_samples(const lt_at_state*);
size_t lt_at_rejection_counts(const lt_at_state*, ...); /* sorted count DESC, tie reason ASC (strcmp) */
double lt_at_apply_authority_limits(double beginning, double recommended, const lt_at_authority*);
double lt_clamp_to_rails(const lt_at_authority*, double v);
```

---------------------------------------------------------------------------
## 3. AutoTune core algorithm (`add_data_point`)

Steps, exactly in this order. If `!is_running` return immediately.

1. `buffer_max_age_ms = required_buffer_ms(settings, filters)` (3.3). Must precede pruning.
2. Push copy of `point` to back of `data_buffer`; `prune(point.timestamp_ms)`: `cutoff = ts.saturating_sub(buffer_max_age_ms)` (0 if ts<age); pop front while `front.timestamp_ms < cutoff`.
   (Note: the current point is already in the buffer when `is_steady` runs in step 3.)
3. `reason = classify(point)`. If rejected: `rejected_by_reason[reason]++` and return (no `total_samples++`).
4. `total_samples++`.
5. Delay resolution. `cx = find_bin_index(point.rpm, xbins)`, `cy = find_bin_index(point.load, ybins)` (3.4).
   If both Some: `delay = resolve_lambda_delay_ms(cx,cy)` (table value `v>0.1` -> `(u64)v` truncated; absent/<=0.1 -> None) `.unwrap_or(configured_or_curve_delay_ms(settings, point.rpm))`; else `configured_or_curve_delay_ms`.
6. `historical = (delay>0 && point.timestamp_ms > delay) ? find_delayed_data_point(ts, delay) : None`.
7. If historical is None: if `strict_lambda_match` -> return (drop; total_samples stays incremented!). Else use `historical = point` (current-cell fallback).
8. Cell: `x_idx = find_bin_index(historical.rpm, xbins)`, `y_idx = find_bin_index(historical.load, ybins)`; if either None return. (Cell is from the HISTORICAL point's rpm/load, but AFR is the CURRENT point's `afr`.)
9. `cell_ve = ref.ve_table[y_idx][x_idx]` if present and `> 0.1`, else `historical.ve`. (Anchoring on the table cell, not interpolated veCurr, is essential - see tests 12.2.)
10. If cell locked -> return.
11. `target = resolve_target_afr(x_idx,y_idx, settings.target_afr)`: `ref.target_afr_table[y][x]` if present and `v > 0.1` -> `normalise_to_afr(v)`, else fallback `settings.target_afr` (NOT normalised).
12. `required_ve = calculate_required_ve(cell_ve, point.afr, target)`:
    `if (afr < 0.1 || target < 0.1) return cell_ve; return cell_ve * (afr / target);`  (lean => VE up)
13. Get-or-create rec at (x_idx,y_idx): `{beginning_value=cell_ve, recommended_value=cell_ve, hit_count=0, hit_weighting=0, target_afr=target, hit_percentage=0, raw_required_cma=cell_ve}`. NOTE `beginning_value` is fixed at creation (first accepted hit of the session).
14. `hit_count++`.
15. `w = weight(settings.hit_weighting, historical.rpm, historical.load, x_idx, y_idx, xbins, ybins)` (3.5). `hit_weighting += w`. `w_total = max(hit_weighting, DBL_MIN)`.
    `raw_required_cma += (required_ve - raw_required_cma) * (w / w_total)`  (weighted incremental mean; seeded with cell_ve, so first sample moves it by w/w_total=1 i.e. fully to required_ve when w>0; with w=0 no move).
16. Confidence ramp: `confidence = base_weight>0 ? min(hit_weighting/base_weight, 1.0) : 1.0`.
    `ramped = beginning + (raw_cma - beginning) * confidence`.
17. `clamped = apply_authority_limits(beginning, ramped, authority)` (3.6).
18. `if fabs(clamped - beginning) < min_change: clamped = beginning`.
19. `recommended_value = clamped; target_afr = target;`
    `hit_percentage = total_samples>0 ? hit_count/total_samples*100 : 0` (only refreshed for the cell just hit; other cells keep stale percentages - port as-is).

### 3.1 Default lambda-delay RPM curve (`get_lambda_delay_ms`)
`r = clamp(rpm, 800, 6000); ratio=(r-800)/5200; delay = 200 - ratio*150; return (u64)delay` (truncate). 800rpm->200, 6000->50. Test note: at 1000 rpm = 200-0.03846*150 = 194 (comment says ~190).

### 3.2 `configured_or_curve_delay_ms`
`settings.lambda_delay_ms > 0 ? (u64)lambda_delay_ms : curve(rpm)`.

### 3.3 `required_buffer_ms(settings, filters)`
```
DEFAULT=500, MARGIN=500
table_max = max over all cells of ref.lambda_delay_table (fold from 0.0)
configured = max(max(settings.lambda_delay_ms, 0), table_max)
for_delay = configured<=0 ? 500 : max((u64)configured + 500, 500)
return max(for_delay, filters.min_steady_ms + 500)
```
Tests: default -> 500; lambda_delay_ms=900 -> 1400.

### 3.4 `find_bin_index(value, bins)` -> index or none
1. empty -> none.
2. First index i (ascending scan) with `fabs(bins[i]-value) < 0.1` -> i.
3. If `!is_within_axis(value,bins)` -> none.
4. Else index of nearest bin (min |bin-value|; ties -> first; comparison NaN treated equal).
`is_within_axis`: `n<2` -> true (n==0 false). `lo,lo_next,hi,hi_prev` taken ascending (if first<=last: lo=first, lo_next=bins[1], hi=last, hi_prev=bins[n-2]; else reversed: lo=last, lo_next=bins[n-2], hi=first, hi_prev=bins[1]). `lo_margin=fabs(lo_next-lo)/2; hi_margin=fabs(hi-hi_prev)/2; return value>=lo-lo_margin && value<=hi+hi_margin`.
Tests (rpm=[1000..6000 step 1000]): 6400 -> idx 5; 600 -> idx 0; 3400 -> idx 2; far outside (e.g. 9000, 100) -> none.

### 3.5 Hit weighting
```
axis_weight(value, idx, bins):
  if idx>=n: return 1.0
  centre=bins[idx]; d=value-centre
  if fabs(d) < DBL_EPSILON: return 1.0
  neighbour = d>0 ? (idx+1<n ? bins[idx+1] : none) : (idx>=1 ? bins[idx-1] : none)
  if none: return 1.0                     /* edge bin keeps whole sample */
  span=fabs(neighbour-centre); if span<DBL_EPSILON return 1.0
  return clamp(1 - fabs(d)/span, 0, 1)
weight():
  UNIFORM: 1.0
  PROXIMITY: ax*ay
  PROXIMITY_SQUARED: (ax*ay)^2
  CENTRE_ONLY: (ax>=0.75 && ay>=0.75) ? 1.0 : 0.0
```
Test axes RPM=[800,1200,2000,4000,6500], LOAD=[30,50,70,100]:
uniform weight(1234,41,1,0)=1; centre sample=1.0; halfway rpm 1200->2000 at 1600 (cell x=1) => 0.5 (load on centre); wide bin: same offset on the wide 4000..6500 span weighs >0.9 vs narrow 0.5; edge bins: weight(600,50,0,1)=1, weight(7000,50,4,1)=1; both axes multiply (0.5*0.5=0.25); squared at halfway = 0.25; centre-only at halfway = 0.0; weight(3400,50,2,1)=0.0 (3400 is 0.3 into 2000..4000 span 2000: 1-1400/2000=0.3 <0.75), weight(2200,50,2,1)=1.0 (1-200/2000=0.9). Squared <= linear always; dead-centre sample = 1.0 under all modes.
Weighted-mean test: weights half => cma from 100 toward 200 sample: full-weight first sample gives 150 (cma seeded 100? see test: full 150.0, half 133.333) i.e. update `cma += (x-cma)*(w/w_total)` with w_total including this sample: with prior total 0... NB the test seeds `wtot=0` then adds w; reproduce step 15 literally.

### 3.6 Authority limits (`apply_authority_limits(begin, recommended, a)`)
```
delta = recommended - begin
max_abs = fabs(a.max_cell_value_change)
cd = isfinite(max_abs) ? clamp(delta, -max_abs, max_abs) : delta
max_pct = fabs(begin * (a.max_cell_percentage_change/100))
fd = isfinite(max_pct) ? clamp(cd, -max_pct, max_pct) : cd      /* NaN begin => no pct clamp, no crash */
return clamp_to_rails(a, begin + fd)
clamp_to_rails: lo=min(min_cell_value,max_cell_value), hi=max(...); clamp(v,lo,hi)
```
(Magnitudes via fabs because UI may send negative limits.) Implement `clamp` safely for NaN (return value unchanged if NaN). Tests: UI payload names bind (max_change_per_cell=15, max_total_change=30, min_value=40, max_value=120); ceiling/floor bound a single update (begin 100, rec 60 w/ floor 80 -> 80); repeated sessions cannot compound past rail (130); reversed min/max does not crash and result in [50,200]; default rails 0..255; negative limits clamp rather than panic; NaN begin does not panic.

### 3.7 Filter order and reasons (`passes_filters` and `rejection_reason`)
`classify(point, filters)`: if `!is_steady` -> reason `"rpm/load not steady"`; else if `!passes_filters` -> `rejection_reason`; else ok.
Reason strings (exact) and check order:
1. `rpm < min_rpm || rpm > max_rpm` -> `"rpm out of range"`
2. `clt < min_clt` -> `"clt below min_clt"`
3. `min_y_axis` parses (trim, strtod full-string; f64::parse accepts "40", "15.0", "1e2", "inf", "nan", ".5", "+3") and `load < bound` -> `"load below min_y_axis"`
4. `max_y_axis` parses and `load > bound` -> `"load above max_y_axis"`
5. `fuel_cut_active == true` -> `"overrun fuel cut"`
6. `afr` not in [10.0,19.5] (NaN also fails) -> `"afr at sensor rail"`
7. `fabs(tps_rate) > max_tps_rate` -> `"tps_rate above max_tps_rate"`
8. (steady check lives here in passes_filters, after 7, but in classify it runs first)
9. `exclude_accel_enrich && accel_enrich_active == true` -> `"accel enrichment active"`
10. custom_filter (non-empty after trim) evaluates false or errors -> falls through `rejection_reason` to catch-all `"custom_filter"`.
Unknown (None) flags never reject.

`is_steady(point, filters)`:
```
if min_steady_ms==0 return true
window_start = point.ts.saturating_sub(min_steady_ms)
reached_back=false
for p in data_buffer from NEWEST to OLDEST:
   if p.ts < window_start: reached_back=true; break
   if fabs(p.rpm-point.rpm)>100 || fabs(p.load-point.load)>3.0 || p.fuel_cut_active==true: return false
return reached_back
```
A buffer that doesn't reach back far enough is NOT steady.

### 3.8 Custom filter
Expression language was `evalexpr`. Variables bound: `rpm, map, maf, load, afr, ve, clt, tps, tps_rate` (floats), `accel_enrich` and `accel_enrich_active` (bool; None->false). Result Boolean -> value; Int -> !=0; Float -> !=0.0; anything else / parse or eval error -> reject sample. Example tests: `"rpm > 2000 && tps < 50 && clt > 70"` accepts (rpm 2500,tps 25,clt 85, min_clt 70); an invalid expression rejects. Port: implement a minimal C expression evaluator (numbers, identifiers above, `+ - * / %`, comparisons, `&& || !`, parens, `true/false`) or declare unsupported. Note evalexpr mixes int/float semantics (int literals `2000` are Int; comparisons Int vs Float work).

### 3.9 `find_delayed_data_point(now_ms, delay_ms)`
```
target = now_ms.saturating_sub(delay_ms)
best = element of data_buffer (iterate oldest->newest) minimizing |ts-target|, strict '<' so first (oldest) wins ties
gap = mean_sample_gap_ms()   /* n<2 -> none; (last.ts - first.ts)/(n-1) integer division; if last<first none */
tol = gap ? max(gap*6/10 (integer), 50) : 50
return best_diff <= tol ? best : none
```
Tests: buffer of 20 samples at 150 ms spacing (ts=i*150, rpm 2000), buffer_max_age 5000: `mean_sample_gap_ms==150`; `find_delayed(2850, 975)` (target 1875, 75 ms from neighbours) is Some (tol 90). Buffer {0..750 step 150} + {2400..2750 step 150}: `find_delayed(2850,1200)` (target 1650) is None.

### 3.10 `compute_flow_scaled_delay_table(ve_table, x_bins, y_bins, idle_delay_ms, floor_ms)` -> [ny][nx]
```
ANCHOR_RPM=800, ANCHOR_LOAD=40
flow(rpm,load,ve)=max(rpm,1)*max(load,1)*max(ve,1)
nearest(v,bins)=index of min |b-v| (0 if empty)
ax=nearest(800,x_bins); ay=nearest(40,y_bins)
anchor_ve = ve_table[ay][ax] if present else 50
anchor_flow = flow(x_bins[ax] (or 800), y_bins[ay] (or 40), anchor_ve)
idle = max(idle_delay_ms, floor_ms)
k = (idle - floor_ms) * anchor_flow
out[j][i] = clamp(floor + k / flow(x_bins[i], y_bins[j], ve_table[j][i] or anchor_ve), floor, idle)
```
Test: x=[800,3000,6000], y=[40,70,100], ve all 50, idle=1050, floor=120: t[0][0]==1050 (+-1), t[2][2] in [120,400), strictly decreasing along rpm and along load, all in [floor,idle].

---------------------------------------------------------------------------
## 4. Fuel-table guard

```c
const char *lt_fuel_tune_refusal(const ecu_def*, const char *table_name); /* NULL = allowed, else message */
size_t lt_fuel_tunable_tables(const ecu_def*, char **names_out); /* sorted ascending (strcmp) */
```
`table = def.get_table_by_name_or_map(name)`; missing -> `"<name> is not a table in this definition."`.
By `table.role`: `Ve` -> allowed. `Ignition`/`AfrTarget`/`WarmupEnrichment` -> refuse with "an ignition table"/"the AFR target table"/"a warm-up enrichment table". `Other` -> if ANY table in def has role Ve then refuse ("not a fuel table") else allow (INI labels nothing).
Refusal text: `"<title-or-name> is <what>. AutoTune scales a table by measured/target AFR, which is only meaningful for a fuel table - on this one it would corrupt the values."` (title used when non-empty).
`fuel_tunable_tables`: tables with `y_bins` present (2-D) AND refusal==NULL, names sorted.
Tests: ignition refused (message contains "ignition"), AFR-target refused, unrelated table refused when INI labels a VE table, unlabelled INI allows its tables but never ignition, nonexistent refused, picker list == tables guard allows, dual-table INI offers both `veTable1Tbl` and `veTable2Tbl`. (Role inference itself belongs to INI spec.)

## 5. Proposal helpers
```c
/* proposed_ve_table: copy current; for each rec set out[cell_y][cell_x]=recommended_value; skip out-of-range (stale index). */
/* proposal_summary: for each rec within range: d = rec.recommended - current[y][x]; if fabs(d)>DBL_EPSILON {changed++; if fabs(d)>fabs(largest) largest=d (keeps sign)}; return (changed, largest) */
```
Tests: only recommended cells move; stale index (9,9) skipped, table shape kept; a no-op rec (40->40) not counted; largest delta keeps sign (-2.0 case); empty proposal -> table unchanged, (0, 0.0).

---------------------------------------------------------------------------
## 6. Declared INI filters (`declared_filters.rs`)

INI `[VeAnalyze]` line: `filter = name, "Label", channel, op, value, , enabled` where each filter states a REJECT condition.
```c
typedef enum {LT_OP_LT, LT_OP_GT, LT_OP_EQ, LT_OP_NE, LT_OP_BITAND} lt_declared_op;   /* serde: less_than, greater_than, equal, not_equal, bitwise_and ; JSON keys camelCase */
typedef struct { char name[64], channel[64]; lt_declared_op op; double value; int enabled; } lt_declared_filter;
int lt_declared_rejects(const lt_declared_filter*, lookup(channel)->double or missing);
```
- `!enabled` -> 0. Missing channel -> 0 (never reject).
- LT: `v < value`; GT: `v > value`; EQ: `fabs(v-value) < 1e-6`; NE: `fabs(v-value) >= 1e-6`;
- BITAND: `bits=(i64)round(v); mask=(i64)round(value); return mask!=0 && (bits&mask)!=0`. (round = half away from zero.)
`lt_first_rejecting(filters,n,sample)`: first rejecting filter in list order, else none.
`lt_rejection_tally(filters,n,samples,m)`: returns per-filter count in filter order (all filters listed, zero counts included); each sample counted only against its FIRST rejecting filter.
Tests: coolant<71: 40 rejected, 85 kept, 71 kept; accel `engine & 16`: 16,24,17 reject; 9,1 pass; ASE `& 4`: 5 rejects, 17 not; `pulseWidth = 0`: 0 rejects, 3.2 not; missing channel no reject; disabled no reject; mask 0 never rejects (even engine=255); first-rejecting reports minCltFilter for {coolant 30, engine 16}; tally for the 4 samples [(30,1),(40,1),(85,16),(85,1)] = [("minCltFilter",2),("accelFilter",1)].
(The INI parse of `[VeAnalyze]` itself - FilterOperator in `tests/ve_analyze.rs`: ops `<`,`>`,`=`,`!=`/`&` etc. - belongs to the INI spec; these declared filters are NOT consulted by `AutoTuneState`; the app maps them to AutoTuneFilters or applies them separately.)

---------------------------------------------------------------------------
## 7. Preflight (`preflight.rs`)

```c
typedef enum { LT_SEV_BLOCKER=0, LT_SEV_WARNING=1, LT_SEV_INFO=2 } lt_severity;
typedef struct { lt_severity sev; char code[32], title[96], detail[512], current[48], suggested[48]; /* current/suggested optional */ } lt_finding;
typedef struct {
  const char *target_table;            /* NULL = unresolved */
  const double *const *target_values; size_t t_rows; /* ragged allowed: cols = first row len */ 
  size_t ve_rows, ve_cols;             /* ve_shape (rows, cols) */
  const double *const *ve_values; size_t v_rows;
  int celsius;
  /* candidate_target_tables: string list (UI only) */
  int will_write_to_ecu;
} lt_preflight_input;
size_t lt_preflight_check(const lt_preflight_input*, const lt_at_settings*, const lt_at_filters*, const lt_at_authority*, lt_finding *out, size_t cap);
int lt_preflight_has_blocker(const lt_finding*, size_t n);
```
Check order (output then STABLE-sorted by severity Blocker<Warning<Info): target_table, ve_table, filters, authority, delay, write_mode.

**check_target_table**
- `target_table == NULL` -> Blocker `target_table_missing`, title "No AFR target table", detail "Every cell will be tuned to a flat {target_afr:.1} AFR, including full load. If the engine is meant to run richer under load, this asks AutoTune to remove fuel exactly where it is most dangerous to do so."; current=`"flat {:.1}"`, suggested="pick a target table". STOP.
- `rows=len(target_values)`, `cols=len(target_values[0])` or 0. If `(rows,cols) != ve_shape` -> Blocker `target_table_shape`, title "AFR target table is the wrong shape", detail mentions "'{name}' is {rows}x{cols} but the VE table is {r}x{c}"...; current "{rows}x{cols}", suggested "{r}x{c}". STOP.
- `vals` = all target values `> 0`. Empty -> Blocker `target_table_empty` "AFR target table reads as all zeros". STOP.
- `lo,hi` = min,max of vals. `looks_afr = lo>=8 && hi<=22`; `looks_lambda = lo>=0.5 && hi<=1.6`. Neither -> Warning `target_table_range` "AFR target values look wrong", current "{lo:.2}..{hi:.2}", suggested "8-22 AFR or 0.5-1.6 lambda".
**check_ve_table**: all values flattened. Empty -> Blocker `ve_table_unreadable` "VE table could not be read". All `<=0` -> Blocker `ve_table_zero` "VE table is all zeros". Else `hi>255` -> Warning `ve_table_scale` "VE values are larger than the table can hold", current "max {hi:.0}", suggested "<= 255".
**check_filters**: celsius: `(sane_lo,sane_hi,unit,suggest)=(40,100,"C",60)`; else `(100,210,"F",140)`. If `min_clt<sane_lo || min_clt>sane_hi`: `boils = celsius && min_clt>100`; severity Blocker if boils else Warning; code `min_clt_units`, title "Minimum coolant temperature looks like the wrong units"; current "{min_clt:.0} {unit}", suggested "{suggest:.0} {unit}". Then `min_rpm >= max_rpm` -> Blocker `rpm_window_empty` "RPM filter window is empty" (current "{min:.0}..{max:.0}", suggested "min below max"). Then `max_tps_rate <= 0` -> Warning `tps_rate_inert` "Transient filter is switched off" (current "{:.1}", suggested "10 %/s").
**check_authority**: `max_cell_value_change<=0 || max_cell_percentage_change<=0` -> Blocker `authority_zero` "Authority limit is zero" (current "{:.1} / {:.1}%", suggested "10 / 20%"). `min_cell_value > max_cell_value` -> Warning `rails_reversed` "Cell value limits are the wrong way round" (current "{:.0}..{:.0}", suggested "min below max").
**check_delay**: `lambda_delay_ms<=0 && !lambda_delay_flow_scaled` -> Warning `delay_default_curve` "Using the built-in RPM delay curve" (current "200-50 ms (assumed)", suggested "measure, or enable flow scaling").
**check_write_mode** (always exactly one Info): `will_write_to_ecu` -> `writes_live` "Changes will be written to ECU RAM as they are accepted"; else `collect_only` "Recommendations only - nothing will be written".
`has_blocker` = any Blocker.
Test fixtures: target `[[14.7,14.7],[12.8,12.8]]`, ve `[[45,52],[94,98]]`, ve_shape (2,2), celsius, filters min_clt=60 -> no blockers. Cases: missing table (Blocker, mentions flat 14.7); wrong shape; values outside both unit ranges Warning; lambda table (0.8-1.0) valid; default min_clt=160 on Celsius blocks; Celsius value on F project warns not blocks; empty RPM window blocks; max_tps_rate<=0 warns; authority 0 blocks; all-zero VE blocks; default delay flagged and `lambda_delay_ms=550` clears it; write mode always stated; findings sorted blocker->warning->info; `min_clt=71` Celsius raises no `min_clt_units`. App rule (outside core): if `filters.min_clt == 160.0` exactly (untouched default) substitute the INI's `minCltFilter` value (71 for CELSIUS).

---------------------------------------------------------------------------
## 8. Replay from logs (`replay.rs`)

```c
typedef struct {            /* channel-major log; empty optional arrays mean channel absent */
  double *time_ms, *rpm, *load, *afr;   /* required, each length n_* */
  double *ve, *clt, *tps, *tps_rate, *fuel_cut, *accel_enrich; /* optional (len 0 = absent) */
} lt_log_channels;
size_t lt_log_len(const lt_log_channels*);  /* MIN of lengths of time_ms,rpm,load,afr */
lt_ve_point lt_log_point(const lt_log_channels*, size_t i):
  rpm=rpm[i]; map=load=load[i]; afr=afr[i];
  ve = ve[i] or 0; clt = clt[i] or 90.0 (absent => warm); tps = or 0; tps_rate = or 0;
  fuel_cut_active = fuel_cut present&&i<len ? (fuel_cut[i] > 0.5) : UNKNOWN; accel likewise;
  timestamp_ms = (u64)max(time_ms[i], 0)
typedef struct { lt_at_settings settings; lt_at_filters filters; lt_at_authority authority;
                 int strict_lambda_match /*1*/, validate /*1*/; } lt_replay_config;
typedef struct { size_t x,y; double rpm,load,current_ve,proposed_ve,delta; unsigned hits; double weight,confidence,target_afr,mean_afr; } lt_cell_result;
typedef struct { const char *rejected_because /*NULL=accepted*/; int has_cell; size_t cx,cy; } lt_sample_verdict;
typedef struct { double gain_pct, worsened_pct; size_t scored; unsigned long folds; } lt_validation;
typedef struct { lt_cell_result *cells; size_t ncells; unsigned long total_samples; /* rejections (string,count) sorted count DESC */ lt_sample_verdict *verdicts; lt_validation validation; int has_validation; unsigned **coverage; /*[ny][nx]*/ } lt_replay_report;
lt_replay_report lt_replay(const lt_log_channels*, xbins,nx, ybins,ny, const lt_at_reftables*, const lt_replay_config*);
```

**run(include)**: fresh state; set reference tables, strict flag; `start()`; `coverage` zeroed ny x nx. For i in 0..len: if `!include(i)` push verdict `{rejected_because:"not in this fold", cell:none}` and continue. Else `point=log.point(i)`; `verdict=state.classify(point, filters)` evaluated BEFORE `add_data_point` (a sample is not evidence of its own steadiness); if Ok: `cell=(nearest(xbins,point.rpm), nearest(ybins,point.load))` (nearest bin, NO range rejection, 0 if empty) and `coverage[cy][cx]++`; push verdict; then `add_data_point(point, ...)` (always called, even for rejected samples so the buffer fills).
**replay()**: `run(all)`; `mean_afr[cell]` = mean of `log.afr[i]` over verdicts with a cell (accepted samples; cell = nearest-bin cell NOT delayed cell); cells = one CellResult per recommendation: `x,y`, `rpm=xbins[x]`, `load=ybins[y]`, `current_ve=beginning_value`, `proposed_ve=recommended_value`, `delta=proposed-current`, `hits=hit_count`, `weight=hit_weighting`, `confidence = base_weight>0 ? min(weight/base_weight,1):1`, `target_afr`, `mean_afr` (0 if none). Sorted by `(y, x)`. `rejections` = state.rejection_counts() re-sorted by count DESC (stable). `total_samples = state.total_samples()`. `validation` only if `config.validate`.

**Validation (k-fold over time blocks)**
```
BLOCK_MS=10000, FOLDS=5, VALIDATION_STEADY_MS=800.0, MAX_STEADY_LOOKBACK=512
block(i) = (u64)max(time_ms[i],0) / BLOCK_MS
steady_mask: for each i: false if rpm<1 or afr outside [10,19.5]; start = time_ms[i]-800;
   for j from i DOWN to max(i-512,0):   (inclusive of i)
       if time_ms[j] < start: reached_back=1; break
       if fabs(rpm[j]-rpm[i])>100 || fabs(load[j]-load[i])>3 || fuel_cut[j]==true(>0.5): steady=0; break
   mask[i] = steady && reached_back
for fold in 0..5:
   state = run(include = block(i)%5 != fold)       /* train on the other 4/5 */
   by_cell = {(x,y): (beginning, recommended)}
   for each sample i with block(i)%5==fold and steady_mask[i]:
       (x,y)=nearest cells of (rpm,load); skip if no rec for cell; skip if current<=0 or proposed<=0
       target = target_afr_table[y][x] if present && >0.1 ? normalise_to_afr(v) : settings.target_afr
       e0=|afr-target|; e1=|afr*current/proposed - target|
       before+=e0; after+=e1; scored++; if e1 > e0+1e-9 worse++
result only if scored>0 && before>0: gain_pct=100*(before-after)/before; worsened_pct=100*worse/scored; folds=5
n==0 -> None
```
Totals are pooled across folds. Fold samples excluded in `run` still receive verdict "not in this fold" (irrelevant to validation).
Tests (X=[2000,3000,4000], Y=[30,50,70]; log: n samples at 100 ms, rpm 3000, load 50, afr 14.7, ve 45, clt 90, tps 20, tps_rate 0, fuel_cut 0, accel 0; tables: ve all 50, target all 14.0, no delay table; config min_clt=71, strict=false):
- 200 samples: cell (1,1) `current_ve==50` (not 45), `delta>0`, `hits>0`.
- clt=20 for 50 samples: `verdicts.len==50`, `total_samples==0`, rejections contain ("clt below min_clt", >0).
- 120 samples: `coverage[1][1]>0`, `coverage[0][0]==0`.
- 3 samples: `validation` is None.
- afr truncated to 40 of 100: `log.len()==40`, verdicts.len==40.
- 20000 samples with time_ms all 0: steady_mask all false and fast (lookback cap).
- 200 samples at 100 ms steady: mask[0]==false, mask[100]==true, >half true.

---------------------------------------------------------------------------
## 9. AFR transport-delay measurement (`delay_measure.rs`)

```c
typedef struct { unsigned long t_ms; double afr; } lt_afr_sample;
typedef struct { double delay_ms, excursion, baseline_afr, leading_edge_ms; } lt_delay_measurement;
typedef enum { LT_DR_INSUFFICIENT_BASELINE, LT_DR_UNSTABLE_BASELINE, LT_DR_NO_RESPONSE, LT_DR_RESPONSE_PRECEDES_STEP, LT_DR_RESPONSE_NOT_SETTLED } lt_delay_rejection;
labels: "too few baseline samples","baseline unstable - hold steadier","no AFR response detected","AFR already moving at the step","still moving at end of hold - use a longer hold"
int lt_detect_delay(unsigned long anchor_ms, const lt_afr_sample *pre, size_t npre, const lt_afr_sample *post, size_t npost, lt_delay_measurement *out, lt_delay_rejection *why);
```
Constants: `MIN_BASELINE_SAMPLES=4`, `MAX_BASELINE_MAD=0.35`, `K_MAD=3.0`, `MIN_EXCURSION_AFR=0.15`, `SUSTAIN_SAMPLES=2`, `PLATEAU_TAIL_FRAC=0.30`, `MAX_PLATEAU_MAD=0.30`.
Helpers: `median(v)` sorts in place; even n = mean of two middle; `mad(values, med)` = median(|v-med|).

`first_crossing(post, level)` -> (crossing_ms (double), sample_at_edge) or none:
```
run=0; edge=none; before_edge=none; prev=none
for s in post:
  if s.afr < level:
     run++
     if run==1: edge=s; before_edge=prev
     if run>=2:
        if before_edge exists && before_edge.afr > edge.afr && before_edge.t < edge.t:
             frac = clamp((before.afr - level)/(before.afr - edge.afr), 0,1)
             crossing = before.t + (edge.t-before.t)*frac        /* linear interpolation */
        else crossing = edge.t
        return (crossing, edge)
  else: run=0; edge=none; before_edge=none
  prev = s
return none
```
`detect_delay` (enrichment drives AFR DOWN):
1. `npre < 4` -> InsufficientBaseline.
2. `baseline=median(pre.afr)`; `scatter=mad(pre.afr, baseline)`; `scatter > 0.35` -> UnstableBaseline. (Note C: median sorts the copy; compute `mad` on the sorted copy - same result.)
3. `threshold = max(3*scatter, 0.15)`.
4. `(leading_ms, edge) = first_crossing(post, baseline - threshold)` else NoResponse.
5. `leading_ms <= anchor_ms` -> ResponsePrecedesStep.
6. Plateau: `first_t,last_t` = post first/last t (0 if empty); `tail_start = last_t - (last_t-first_t)*0.30`; tail = afr of post samples with `t >= tail_start`. `len(tail) < 4` -> ResponseNotSettled. `plateau=median(tail)`; `mad(tail,plateau) > 0.30` -> ResponseNotSettled.
7. `excursion = baseline - plateau`; `excursion < threshold` -> NoResponse.
8. `half_level = baseline - excursion/2`. `edge_idx = first index in post with t >= edge.t` (0 if none). `from = last index < edge_idx with afr >= half_level` (else 0) [rposition over post[..edge_idx]]. `(half_ms,_) = first_crossing(post[from..], half_level)` else ResponseNotSettled. `half_ms <= anchor_ms` -> ResponsePrecedesStep.
9. Result: `delay_ms = half_ms - anchor_ms`, `excursion`, `baseline_afr=baseline`, `leading_edge_ms = leading_ms - anchor_ms`.
Rationale: half-height of a step response = median transit time; leading edge is biased short (kept for comparison only).
Tests:
- interpolation: pre 4x14.70 at t=0,60,120,180; post (240,14.70),(300,14.10),(360,13.85),(420,13.72),(480..900 step 60,13.70); anchor 240: `0 < delay < 59` (sub-sample).
- crossing at/before anchor: post (240,14.20),(300,14.10),(360,14.05), anchor 240 -> Err(ResponsePrecedesStep).
- pre-step drift: post (240,14.70),(300,14.58),(360,14.58),(420,14.70),(480,14.70),(540,14.50),(600..900 step 60,14.475); delay ~= 274 (+-1).
- clean step: pre (0,14.7),(40,14.68),(80,14.72),(120,14.7),(160,14.69); post (200,14.70),(240,14.70),(280,14.70),(320,14.60),(360,14.40),(400,14.15),(440,13.95),(480,13.80),(520,13.72),(560..680 step 40,13.70); anchor 200: delay ~192 (+-1), excursion ~1.00 (+-0.05), baseline ~14.7.
- single spike: pre 4x14.7 at 0,40,80,120; post (200,14.7),(240,13.9),(280,14.7),(320,14.71),(360,14.69), anchor 200 -> NoResponse.
- sustained after spike: post (200,14.7),(240,13.9),(280,14.7),(320,14.3),(360,14.0),(400,13.85),(440,13.75),(480..600 step 40,13.70) -> edge anchored on real run (crossing in 280..320), not 240.
- wandering baseline [13.2,15.4,12.9,15.8,13.5] -> UnstableBaseline; 2 baseline samples -> InsufficientBaseline; flat trace -> NoResponse.
- half_excursion_is_later_than_the_leading_edge: pre 4x14.70; post (200,14.70),(240,14.60),(280,14.45),(320,14.25),(360,14.05),(400,13.90),(440,13.78),(480,13.72),(520..640 step 40,13.70): `delay_ms > leading_edge_ms`.
- still moving: post (200,14.70),(240,14.60),(280,14.35),(320,14.05),(360,13.75),(400,13.45),(440,13.15) -> NotSettled.

**Delay table (aggregate)**
```
RPM_EDGES = {1200, 2000, 3000, 4500, 6500}  (5 edges -> 6 bins; last open-ended)
LOAD_EDGES = {40, 60, 80, 100}               (4 edges -> 5 bins)
cells[load_bin 0..4][rpm_bin 0..5] = {n, mean_ms, range_ms, min_ms, max_ms}
bin(edges,v) = first i with v < edges[i], else len(edges)
add(rpm,load,delay): (l,r)=cell_index; if n==0 {min=max=delay} else {min=min(..); max=max(..)}
   mean=(mean*n+delay)/(n+1); n++; range=max-min
```
Test: add(900,35,400), add(950,38,360) -> cell[0][0] n=2 mean 380 range 40; add(3200,85,140) lands at `cell_index(3200,85)`; add(7000,105,90) -> cells[4][5] n=1 mean 90.

**Flow-delay fit** `lt_fit_flow_delay(samples (rpm,load,ms)[n]) -> optional {floor_ms,k,anchor_ms,rms_ms,samples}`:
```
usable = samples with rpm>0 && load>0 && ms>0 ; if count<4 return none
x_i = 1/(rpm*load)
for floor = 40.0; floor <= 250.0; floor += 5.0:        /* accumulate by repeated += 5.0 (exact in fp) */
   k = sum(x*(ms-floor)) / sum(x*x)    (skip if den<=0)
   ss = sum((floor + k*x - ms)^2)
   keep first strictly smaller ss (best)
anchor_ms = floor + k/(800*40);  rms_ms = sqrt(ss/n);  samples = n
```
Model: `delay = floor + k/(rpm*load)`. Tests: data from floor=150,k=3e7 at (900,35),(1500,40),(2500,50),(3500,70),(5000,90),(6000,96): recovered floor within 5, rms<5, samples 6; samples [(900,35,1100),(1500,40,700),(2500,50,450),(5000,90,260)] -> k>0 and delay falls with flow; <4 samples -> None; rubbish (rpm/load/ms <=0) dropped before fitting (4 good of N -> samples==4); `anchor_ms == floor + k/(800*40)`, `anchor_ms > floor_ms`.

---------------------------------------------------------------------------
## 10. Acceleration-enrichment tuning (`accel_enrich.rs`)

```c
typedef struct { unsigned long timestamp_ms; double tps_dot, afr, afr_target /*0 => fallback*/, clt; } lt_ae_sample;
typedef struct {
  double *tae_bins; size_t nbins;  double *tae_rates; size_t nrates;
  double ae_time_ms, delay_ms, event_floor, min_clt; size_t min_events;
  double authority_pct, rate_gain_pct_per_afr, target_afr_fallback;
} lt_ae_config;
/* defaults: tae_bins {60,160,400,600}; tae_rates {10,13,17,20}; ae_time_ms 160; delay_ms 800; event_floor 30; min_clt 70;
   min_events 8; authority_pct 5.0; rate_gain_pct_per_afr 100/14.7; target_afr_fallback 14.7 */
typedef struct { double peak_tps_dot, residual_afr; } lt_ae_event;
typedef struct { size_t bin; double lo; int has_hi; double hi; size_t events; int has_median; double median_residual_afr, current_rate, recommended_rate; int sufficient_data; char note[96]; } lt_ae_bin_rec;
```
`afr_err(s) = s.afr - (s.afr_target>0 ? s.afr_target : fallback)`.
`bin_of(peak, bins)`: last index i with `peak >= bins[i]` (bins ascending), else none.
**analyze_events(samples sorted by time)**:
```
first_bin = tae_bins[0] or 60; settle_ms = ae_time_ms + 500; base_span = 400
i=1
while i<n:
  if samples[i].tps_dot < event_floor: i++; continue
  start=i; j=i; peak=samples[i].tps_dot; peak_i=i
  while j<n && samples[j].tps_dot >= event_floor*0.5:     /* event persists above half the floor */
       if samples[j].tps_dot > peak: peak=...; peak_i=j
       j++
  if samples[peak_i].clt >= min_clt && peak >= first_bin:
       peak_ts=ts[peak_i]; start_ts=ts[start]
       resp_lo = peak_ts + delay_ms; resp_hi = resp_lo + settle_ms
       resp = afr_err of samples with resp_lo <= ts <= resp_hi
       base = afr_err of samples with (start_ts-400 <= ts < start_ts) OR (resp_hi < ts <= resp_hi+400)   [else-branch: samples in resp window never go to base]
       (bounds search via binary search is just an optimisation: window = ts in [min(start_ts-400, resp_lo), resp_hi+400])
       if len(resp)>=3 && len(base)>=3: emit {peak_tps_dot=peak, residual_afr = mean(resp) - median(base)}
  i = j+1
```
(Note: baseline uses MEDIAN, response uses MEAN. `i=j+1` skips one sample after the event.)
**recommend(events, cfg)** one per bin b in `0..len(tae_bins)`: bucket residuals by `bin_of(peak)`. `cur = tae_rates[b]` or 0; `lo=tae_bins[b]`; `hi=tae_bins[b+1]` or none. If `count < min_events`: held (`recommended=cur`, `sufficient_data=0`, note `"insufficient data (need {min_events}) - held"`, median still reported). Else `m=median`; `delta = clamp(m*rate_gain_pct_per_afr, -authority_pct, +authority_pct)`; `rec = max(cur+delta, 0)`; note: `delta>0.3` -> `"richen (+{delta:.1}); tip-ins run {m:+.2} AFR lean"`; `delta<-0.3` -> `"lean ({delta:.1}); tip-ins run {m:+.2} AFR rich"`; else `"on target ({m:+.2} AFR); no change"`. (+ residual = lean = enrichment too weak = raise rate.)
Tests: synth log 100 ms steps 0..8000, tip-in at 3000 with tps_dot=peak at that sample only, afr = 14.7 + resp_afr during `[tip+delay, tip+delay+660)`, clt 85, target 14.7: peak 100, +0.6 AFR, delay 800, min_events=1 -> 1 event, residual>0.4, `bin_of(100)==0`, bin0 sufficient and recommended>current; -0.6 -> residual<-0.4 and recommended<current; wrong delay config reads no excursion (events empty or |residual|<0.3); empty bins (<min_events) are held with `sufficient_data=false` and recommended==current; bin 0 with 12 events is sufficient.

---------------------------------------------------------------------------
## 11. Summaries (shorter): anomaly, health, predictor

### 11.1 Anomaly detection (`anomaly.rs`)
Types: `AnomalyType {StatisticalOutlier, MonotonicityViolation, GradientDiscontinuity, PhysicallyUnreasonable, FlatRegion}`; `TuneAnomaly{row,col,value,expected_value,type,severity 0..1,description}`.
Config defaults: `outlier_sigma 2.0, gradient_threshold 2.5, min_reasonable_ve 5.0, max_reasonable_ve 180.0, min_flat_region_size 4, flat_tolerance 0.1, monotonicity_load_floor_kpa 30.0, monotonicity_min_rpm 0.0`.
`detect(table,x_bins,y_bins)`: empty -> []. Run 5 passes (order below), append, **stable sort by severity DESC**, then keep the first entry per (row,col) (dedup by cell only; type ignored).
1. **Outlier (local plane fit)**: for each cell gather the 8-neighbourhood (excl. self) as `(x=x_bins[c'] or c', y=y_bins[r'] or r', z)`. If `npts <= 3` skip (corners/edges with <=3 pts). Centered LSQ: `mean_x,mean_y,mean_z; sxx=Σ(x-mx)², syy, sxy=Σ(x-mx)(y-my), sxz=Σ(x-mx)z, syz=Σ(y-my)z; det=sxx*syy-sxy²`. If `|det|>1e-9`: `a=(syy*sxz-sxy*syz)/det; b=(sxx*syz-sxy*sxz)/det; c=mean_z-a*mx-b*my; expected=a*cx+b*cy+c` (cx,cy = this cell's axis values or indices); residuals `z-(a*x+b*y+c)`; `dof=max(npts-3,1)`; `rstd=sqrt(Σres²/dof)`. Else `expected=mean_z, rstd=0`.
   - If `rstd < 0.01`: `residual=|val-expected|`; `gate = 5.0 * max(outlier_sigma/2.0, 0)`; if `residual>gate`: severity `clamp((residual-gate)/45, 0,1)`; desc `"Cell value {val:.1} deviates {residual:.1} from a clean local plane ({expected:.1})"`.
   - Else `z=|val-expected|/rstd`; if `z>outlier_sigma`: severity `min((z-outlier_sigma)/3, 1)`; desc `"Cell value {val:.1} is {z:.1}σ from local plane fit ({expected:.1})"`.
2. **Monotonicity**: needs `len(y_bins)>=2`. For each col c (skip if `x_bins[c] < monotonicity_min_rpm`), for r in 1..rows: `load=y_bins[r-1]` (skip if `< monotonicity_load_floor_kpa`); `prev=t[r-1][c]; curr=t[r][c]`; `load_increasing = y_bins[r] > y_bins[r-1]` (true if missing); if `load_increasing && curr < prev*0.8 && prev>10`: `drop_pct=|(prev-curr)/prev*100|`, severity `min(drop_pct/50,1)`, row=r, value=curr, expected=prev, desc `"VE dropped {drop_pct:.0}% ({prev:.1}→{curr:.1}) with increasing load"`.
3. **Gradient**: `grads` = |right-left| and |down-up| for every adjacent pair; `avg=mean`; if `avg<0.01` skip pass; `threshold=avg*gradient_threshold`; per cell `max_local` = largest |neighbour-cell| over 4 neighbours (first-wins on ties, order right,left,down,up) and that neighbour's value; if `max_local>threshold`: severity `min((max_local/threshold - 1)/2, 1)`, expected=that neighbour; desc `"Sharp gradient: {max_local:.1} change vs {avg:.1} average"`.
4. **Physically unreasonable**: `val<min_reasonable_ve` -> severity 0.8 expected=min, desc `"VE {v:.1} below minimum reasonable value {min:.1}"`; `else val>max_reasonable_ve` -> 0.9, `"VE {v:.1} above maximum reasonable value {max:.1}"`.
5. **Flat regions**: DFS (4-connected, stack-based) flood fill where each cell is compared to the SEED value `|t-seed| <= flat_tolerance`; visited global; region size `>= min_flat_region_size` -> every cell gets severity `min(size/(rows*cols), 0.8)`, expected=value=seed, desc `"Part of {n} identical cells (value {v:.1}) - likely untuned"`.
Tests: smooth 4x4 `[[48,50,52,54],[50,52,54,56],[52,54,56,58],[54,56,58,60]]` x=[1000..4000], y=[20,40,60,80], sigma=3.0 -> no non-flat anomalies; table with 120 at (1,1) in a 4x4 ramp -> outlier flagged; monotonicity drop >20% detected above load floor; flat region found; <5 and >180 flagged; ramped surface gives no false outliers; flat_tolerance configurable; load floor ignores low load; dedup keeps highest severity per cell.

### 11.2 Health scoring (`health.rs`)
Output: `TuneHealthReport{overall_score u32, overall_grade, regions[], recommendations[], total_cells, data_coverage_cells, data_coverage_percent}`; `RegionHealth{name,type,score,coverage_score,smoothness_score,monotonicity_score,cell_count,row_range,col_range}`; `RegionType {Idle,PartThrottle,Cruise,WOT,Transition}`.
Config defaults: `idle_rpm_max 1500, wot_rpm_min 4500, wot_load_fraction 0.8, idle_load_fraction 0.3, coverage_weight 0.4, smoothness_weight 0.35, monotonicity_weight 0.25, monotonicity_load_floor_kpa 30, monotonicity_min_rpm 0, use_absolute_map_wot true, wot_map_kpa 90`.
`score_table(values, hit_counts u32[][], x_bins, y_bins)`: rows==0 or empty bins -> overall 0, "F", recs `["No table data available"]`, zeros.
**define_regions** (all indices inclusive; `cols=len(values[0])`):
```
idle_col_end = pos(x>=idle_rpm_max) .unwrap_or(1) .max(1) .saturating_sub(1)
wot_col_start = pos(x>=wot_rpm_min) .unwrap_or(cols-1 saturating)
min_load=y[0]; max_load=y[last]; range=max-min
idle_load_thr = min_load + range*idle_load_fraction
wot_row_start = use_absolute ? pos(y>=wot_map_kpa) : pos(y>= min_load+range*wot_load_fraction);  .unwrap_or(rows-1 saturating)
idle_row_end = pos(y>=idle_load_thr) .unwrap_or(1) .max(1) .saturating_sub(1)
regions in this order:
 "Idle" (Idle): rows 0..=min(idle_row_end,rows-1), cols 0..=min(idle_col_end,cols-1)   [if idle_col_end<cols && idle_row_end<rows]
 "Cruise" (Cruise): cruise_col_start=min(idle_col_end+1,cols-1); cruise_col_end=max(wot_col_start-1 (sat), cruise_col_start); cruise_row_start=min(idle_row_end+1,rows-1); cruise_row_end=wot_row_start-1 (sat, NO upward clamp);
        emitted iff cruise_col_end>=cruise_col_start && cruise_row_end>=cruise_row_start
 "WOT" (WOT): rows wot_row_start..=rows-1, ALL columns 0..=cols-1   [if wot_row_start<rows]
 "Low Load" (Transition): rows 0..=min(idle_row_end,rows-1), cols idle_col_end+1..=cols-1   [if idle_row_end<rows && idle_col_end+1<cols]
 pt_row_start=min(idle_row_end+1,rows-1); pt_row_end=wot_row_start-1 (sat)
 if pt_row_end>=pt_row_start: "Part Throttle, low RPM" (PartThrottle) cols 0..=idle_col_end [if idle_col_end>0];  "Part Throttle, high RPM" cols wot_col_start..=cols-1 [if wot_col_start<cols]
 if none emitted: "Full Table" (Cruise) whole table.
```
(`pos` = first index satisfying predicate; `saturating_sub` floors at 0. Note `.unwrap_or(1).max(1).saturating_sub(1)` => index >=0.) Regions tile the table without overlap (test `regions_cover_every_cell`, `part_throttle_does_not_overlap_cruise`).
**score_region** returns `(overall, coverage, smooth, mono)`: `overall = min((u32)(cov*0.4 + smooth*0.35 + mono*0.25), 100)` (truncating).
- coverage: `(u32)(cells_with_hits>0 / total * 100)`; total 0 -> 0.
- smoothness: interior rows/cols = `range.1 - (range.0+1)` (saturating); if either 0 -> 100. Mean over interior cells (r in start+1..end-1, c likewise) of `|up+down+left+right-4v|` (missing neighbours default to v, missing v to 0); `score=(u32)(clamp(1 - mean/10.0, 0,1)*100)`; count 0 -> 100.
- monotonicity: for c in col range (skip if `x_bins[c] < monotonicity_min_rpm`), for r in `row_range.0..row_range.1` (EXCLUSIVE end): skip if `y_bins[r] < load_floor`; `total++`; `if values[r+1][c] >= values[r][c]*0.95` mono++. total 0 -> 100; else `(u32)(mono/total*100)`.
**Overall**: `overall_score=(u32)(Σ score_i*cell_count_i / Σ cell_count_i)` (truncate; cell_count=(rows span)*(cols span) per region); `data_coverage_cells` = count of hit>0 anywhere; percent = cells/(rows*cols)*100.
Grade: 90..=100 A, 80..=89 B, 70..=79 C, 60..=69 D, else F.
Recommendations (in order): if overall coverage% < 50: `"Low data coverage ({:.0}%) - more driving needed to populate the VE table"`; then per region in order: `coverage_score<30` -> `"{name} region has very low data coverage ({coverage_score}) - focus on driving in this area"`; `smoothness_score<40` -> `"{name} region has rough VE transitions - consider smoothing"`; `monotonicity_score<50` -> `"{name} region has VE values that decrease with load - check for tuning errors"`.
Tests: perfect table scores high; empty table -> 0/"F"; no coverage lowers coverage score; grade boundaries; boosted engine (20-250 kPa) classifies WOT by absolute 90 kPa; perfect ramp smoothness 100, wavy table low; region boundary at >= threshold belongs to higher region.

### 11.3 Predictor (`predictor.rs`) - fill zero-hit cells
Types: `PredictedCell{row,col,predicted_value,current_value,confidence,method,neighbor_count}`; `PredictionMethod {BilinearInterpolation, NeighborWeighted, LinearExtrapolation, OneDInterpolation, PhysicsModel}`.
Config defaults: `min_confidence 0.3, max_search_radius 5, min_hit_count 3, distance_decay 2.0, physics_peak_torque_rpm 4500, physics_min_ve 10, physics_max_ve 200`.
`predict_cells(values, hits, x_bins, y_bins)`:
- `known` = cells with `hits >= min_hit_count` -> (value, hits). Empty known -> []. Cells with `hits>=min_hit_count` are skipped (no prediction). For each other cell (row-major) try in order, accepting the first whose `confidence >= min_confidence`: bilinear -> neighbor_weighted -> linear_extrapolation -> physics_model. Output sorted by confidence DESC (stable).
Helpers: `axis_range(bins)=max(last-first,1e-6)` (defaults last=100, first=0 if empty). `physical_distance(r,c,r2,c2)= max(sqrt(dx²+dy²), 0.001)` with `dx=|x[c]-x[c2]|/x_range` and `dy=|y[r]-y[r2]|/y_range` (fallback to raw index differences if bin missing).
**Bilinear (quadrant corners)**: for each quadrant (row_dir,col_dir) in order UL(-1,-1), UR(-1,1), DL(1,-1), DR(1,1), `find_nearest_known`: for ring=1..=max_search_radius: candidates = offsets (dr,dc) on the Chebyshev ring (`|dr|==ring || |dc|==ring`) enumerated dr from -ring..ring, dc from -ring..ring, kept if `in_quadrant`, STABLE-sorted by Euclidean distance; first in-bounds known cell wins. Half-open quadrants (each non-origin offset in exactly one): UL `dr<0 && dc<=0`; UR `dr<=0 && dc>0`; DR `dr>0 && dc>=0`; DL `dr>=0 && dc<0`. Collect distinct corners; need >=3 else None. Weights `1/dist^distance_decay` with dist = physical distance (dx uses col of corner vs target on x_bins; dy uses row on y_bins; min 0.001); `weight_sum<0.001` -> None. `predicted = clamp(Σ v*w/Σ w, 1, 200)`. Confidence: `corner_factor=ncorners/4`; `radius_fraction(bins)=min(max_search_radius/max(len-1,1), 1)`; `rx,ry`; `max_norm_dist=max(sqrt(rx²+ry²)*1.5, 1e-6)`; `distance_factor=max(1 - max_dist/max_norm_dist, 0)`; `confidence = clamp(corner_factor*0.7 + distance_factor*0.3, 0,1)`; neighbor_count=ncorners.
**NeighborWeighted**: all known cells in the square radius `max_search_radius` (clipped), excluding self; `w = (1/dist^decay) * sqrt(hits)` with physical distance; need `count>=2 && weight_sum>=0.001`; `predicted=clamp(Σvw/Σw, 1, 200)`; `confidence = min(count/8,1)*0.8`.
**Linear/1-D**: collect known cells in same row (then same column) as (index,value) sorted by index; need >=2 known; row-wise tried first with `base_extrap_conf=0.4`, then column-wise `0.35`. `extrapolate_1d(target, pts)`: if bracketed by consecutive pts (`i0<=target<=i1, i0!=i1`) -> linear interpolation, `is_interp=true`; else if target before first: slope from first two pts, value `v0 + slope*(target-i0)`; target after last: slope from last two, `v1 + slope*(target-i1)`; else None. In `build_extrapolated_cell`: interpolation -> value clamp(1,200), confidence 0.75, method OneDInterpolation. Extrapolation: `distance` = gap beyond nearest end; `distance==0 || distance>3 (MAX_EXTRAP_CELLS)` -> None; `ref_val` = nearest end value; `bounded = clamp(val, max(ref*0.7,1), min(ref*1.3,200))`; `confidence = base_conf * (1 - (distance-1)/4)`; method LinearExtrapolation; neighbor_count = number of known pts on the line. If the row-wise build returns None the column-wise is tried.
**Physics model** (confidence fixed 0.15, which is below default min_confidence 0.3 so it is effectively disabled by default): `y_bins` empty -> None; `load_fraction = clamp((y[row]-y0)/max(ylast-y0,1), 0,1)` (y[row] default 50); `peak=max(peak_torque_rpm,1)`; `rpm=x[col]` (default peak); `rpm_factor = rpm<=peak ? 0.6+0.4*clamp(rpm/peak,0,1) : 1 - 0.4*clamp((rpm-peak)/peak,0,1)`; `ve = clamp((min_ve + load_fraction*max(max_ve-min_ve,1)) * rpm_factor, min_ve, max_ve)`; method PhysicsModel, neighbor_count 0.
Tests: no known -> []; all known -> []; centre predicted from corners; sorted by confidence; extrapolation never returns near-zero fuel far from data; orthogonal neighbours found via half-open quadrants; physical distance weighting; interpolation vs extrapolation tagging; empty table; physics scales with rpm and supports forced induction (VE>100).

---------------------------------------------------------------------------
## 12. Test cases to port (AutoTuneState-level; concrete)

Common: `settings0 = {base_weight:0, min_change:0}` otherwise defaults; filters default except as noted.
12.1 `tests/autotune_heatmap.rs` (helper `feed`: point rpm,load(map=load),afr,ve, clt=170, tps=30, ts advances 200 ms per call from 0, default filters/authority, settings0; table x=[1000,2000,3000], y=[10,20,30]; state `start()`ed):
- **accumulate**: feed (1000,10,afr 14.7/13.0/12.0, ve 50): rec at cell (0,0), hit_count>=1, `target_afr==14.7`, `recommended_value < beginning_value`.
- **target table used**: `target_afr_table=[[12.5,13,13.5],[13,13.5,14],[14,14.5,15]]`; two feeds at afr 12.5, ve 60: `r.target_afr==12.5`, `recommended==60 (+-1e-6)` (also verifies beginning_value=60 comes from sample ve since no ve_table).
- **strict**: strict=true, single sample ts=10000 (default settings/filters, afr 13, rpm 1000, load 10, clt 170, tps 30) -> no recs. Then strict=false, sample ts=10200 -> recs non-empty.
- **CMA**: 12 feeds at (1000,10) alternating afr 15.0/16.0, ve 50, table x=[1000,2000], y=[10,20]: hit_count>=4, recommended within 1.0 of 50*15.5/14.7=52.72 and strictly between 50*15/14.7+0.5 and 50*16/14.7-0.5.
- **y-axis filter**: min_y_axis "15.0", max_y_axis "25.0": load 10 rejected, 30 rejected, 20 accepted (rpm 1500, clt 170).
- **ramp holds sparse cell**: three samples afr 13,12.5,12 at ts 0,200,400: `run(base_weight=0,min_change=0)` < 50 and `run(20,0)` closer to 50 than unramped (ramped moves less).
12.2 `mod.rs` lean-cell anchor: x=[...3 rpm bins incl 3000 at idx1], y=[...50 at idx1]; ve_table all 50, target table all 14.0; strict=false; 40 samples (rpm 3000, load 50, ve 45, afr 14.7, ts i*100, clt 90, fuel_cut false, accel false), filters min_clt 71: rec (1,1): `beginning_value==50` and `recommended ~= 52.5 (+-0.1)`. Without ve_table: `beginning_value==45`.
12.3 Rejection counting: filters {min_rpm 1000, min_clt 160, max_tps_rate 50}; samples cold(clt 100) x2, fast_throttle(tps_rate 80, clt 180), good(clt 180,afr 14.7,ve 50); tables x=[1000], y=[50]: counts[0]==("clt below min_clt",2), counts[1]==("tps_rate above max_tps_rate",1), total_samples==1; `start()` again clears counts and total.
12.4 Reasons: fuel_cut true rejects with "overrun fuel cut"; Some(false)/None pass. Railed afr [9.9, 9.99, 19.6, 22, 0] rejected, [11.5,12.7,14.7,16,19.4] accepted. Accel: Some(true) rejects "accel enrichment active"; Some(false)/None pass. Default filters (min_rpm 1000, clt 160): rpm 500 -> "rpm out of range"; clt 100 -> "clt below min_clt"; tps_rate 80 (clt 180) -> "tps_rate above max_tps_rate". Default `max_tps_rate==50`; point rpm 2000, clt 180, tps_rate 50 passes defaults.
12.5 `tps_load_axis_attributes_to_correct_cell`: x=[1000,2000,3000,4000], y=[0,25,50,75,100]; rpm 3000, load 75 -> single rec at (2,3).
12.6 Buffer sizing: configured 900 ms; feed ts 0..1000 step 100 (x=[1000,2000], y=[50,100], rpm 1500 load 75 afr 14 ve 50 clt 80 tps 10): `required_buffer_ms==1400`, `configured_or_curve_delay_ms(900,1500)==900`, after feeding the oldest buffered ts is 0 and `buffer_max_age_ms>=900`.
12.7 Lock set: lock same cell twice then unlock once -> unlocked; unrelated locks survive.
12.8 Defaults: `base_weight==20, min_change==1.0, target_afr 14.7, update_rate 100, lambda_delay_floor 120`; ramp arithmetic (5 of 20 weight: 100->120 gives 105; weight 25 gives 120; base 0 -> confidence 1); min_change: proposed 100.4/99.7/100.0 from begin 100 stays 100 at min_change 1; 102 passes.
12.9 Custom filter: see 3.8.

---------------------------------------------------------------------------
## 13. Gotchas / port notes
1. Cell = delayed (historical) point's rpm/load; measured AFR = current point's afr; delay lookup uses the CURRENT point's cell.
2. `total_samples` increments before the delay match, so strict-dropped samples still count in the `hit_percentage` denominator.
3. `find_bin_index` rejects out-of-axis samples (half-span margin); `replay` coverage/verdicts/validation use unrestricted `nearest`.
4. `beginning_value` is frozen at creation of the rec (first accepted hit of the session); `locked_cells`, reference tables and `table_name` survive `start()`.
5. Lambda/AFR normalisation: target table normalised; `settings.target_afr` fallback and sample `afr` are not (caller must convert measured lambda upstream).
6. u64 `saturating_sub` everywhere with timestamps; buffer pruning uses `<` cutoff; match tolerance integer math `gap*6/10`.
7. `min_clt` default 160 is Fahrenheit-shaped; Celsius INI substitution done by app (see section 7).
8. All clamps must tolerate lo>hi and NaN (Rust would panic; C must not misbehave).
9. HashMap iteration order is unspecified; sort recommendations by (y,x) and rejection counts by (count DESC, reason ASC) for deterministic output.
10. Out of scope: logging, JSON/serde, evalexpr (custom filter), Tauri commands (`commands/afr_delay_test.rs` drives `detect_delay`; step execution/ECU writes not specified here), LLM agent integration.
