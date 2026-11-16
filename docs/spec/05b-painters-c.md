# 05b - Gauge painters, C implementation notes

Companion to `05-dash-gauges.md` section 6 (which stays the authority for exact numbers).
This page only says how to wire the painters in C on top of `lt_gfx` and the `dash` module,
and lists which spec numbers matter for each painter. Rendering is NOT in `src/dash/`.

## 1. Wiring

```c
static void my_gauge(void *ud, lt_gfx *g, lt_rect r, const lt_dash_gauge *cfg,
                     const lt_dash_gstate *st, double now_ms);
static void my_ind(void *ud, lt_gfx *g, lt_rect r, const lt_dash_indicator *ind, lt_bool on);
lt_dash_painters p = { NULL, my_gauge, my_ind };
lt_dash_set_painters(&p);                 /* once at startup */
/* each frame: */
lt_dash_view_update(view, now_ms, &varenv.env);
lt_dash_view_render(view, gfx, area, &varenv.env);   /* layout, enabled_condition, letterbox, hit rects */
```
`my_gauge` dispatches on `cfg->painter` (unknown -> BasicReadout). `lt_dash_render_gauge` /
`lt_dash_render_indicator` call the same hooks for previews (state built from one value).

Everything a painter needs is in `lt_dash_gstate`:
| field | use |
|---|---|
| `display` | smoothed + pegged value `v` (EMA tau 120 ms). Always draw with this, never `raw`. |
| `lim.min/max` | effective range (MinVP/MaxVP resolved). Use instead of `cfg->min/max`. |
| `lim.has_*`, `lim.lw/hw/lc/hc` | effective warning/critical thresholds (VP resolved). |
| `zone` | 0 normal, 1 warn, 2 critical, with hysteresis already applied. Value colour = `font_color / warn_color / critical_color`. |
| `lt_dash_gstate_spiking(st, now)` | flash value text in `critical_color` (400 ms after last spike). |
| `peak.peak` | peak-hold marker (only when `cfg->peak_hold`; dial draws it when `peak > lim.min`). |
| `ring[0..n_series-1]`, `series_latest[]`, `n_series`, `series_ch[]` | LineGraph (1 series) / MultiChannelTrend (<=12). `lt_dash_ring_get(&ring, i)`, i = 0 oldest, `ring.n` samples, 1 sample per >= 100 ms. |
| `lt_gauge_resolve_arc(cfg, &arc)` | dial geometry: `start_deg`, `sweep_deg`, `face_start_deg`, `face_deg`, `ccw`, `full_circle` (degrees). |
| `lt_dash_view_due(view, i, now)` | optional per-gauge repaint throttling (redraw_hz + stagger slot). |

Indicators: `on` comes from `lt_dash_view_state(view, i)->on`; the render hook receives it.

## 2. Primitive mapping (spec 5.3)
- Angles: radians, 0 = east, clockwise positive (y down) = `lt_gfx_arc` convention. Dial: `a = deg * PI / 180`.
  `angle_at(p) = start + p*sweep` (minus when `ccw`).
- Gradients: only `lt_gfx_fill_rect_vgrad` exists. Use it for the vertical ones; for horizontal and radial gradients
  draw `n` thin strips / concentric rings (e.g. 6..12 steps) or one flat colour = the middle stop.
- Metallic bezel (trim colour): 5 stops `lighten 60, 30, base, darken 20, darken 40` over normalized distance from the
  point offset (-0.3 r, -0.3 r); as rings of `lt_gfx_fill_arc(r_in, r_out, 0, 2PI)`.
- Shadows/glow: skip, or draw a `darken` offset copy. `lt_color_lighten/darken(c, pct)`; negative pct inverts.
- Round rect: `lt_gfx_fill_round_rect`; thick line caps: `lt_gfx_line` + `lt_gfx_fill_circle` at the ends.
- Text: `lt_gfx_vtext(g, x, y, s, height_px, 0, align, valign, stroke, color)`; sizes below are CSS px = `height_px`.
  Bold -> stroke ~ height/10. `"%.*f"` via `lt_snprintf`. Units/titles that start with `{` are expressions: resolve
  with `lt_expr_cache_display` before drawing.
- Guard every `(v - min) / (max - min)` with `max > min` else pct = 0; `n-1 == 0` for history lines.

## 3. Painter quick table (W,H = rect size; P padding; pct = clamp01((v-min)/(max-min)))
| painter | key geometry | zones/colours |
|---|---|---|
| BasicReadout | frame roundRect corner `min(8,.05W)`; panel inset P=6; title size `max(9,m*.12)` top y=P+2; value `max(14,m*.35)` bold mono centered at `(W/2, H/2+ts*.3)`; units `max(8,m*.10)` bottom; m=min(W,H), x`1+adj*.1` | value colour from `zone`; spike flash |
| HorizontalBar | P=6, bar h=`.35H` at y=`(H-barH)/2+.08H`; fill `barW*pct`; value right-top `"%.nf units"` | fill colour = zone colour |
| VerticalBar | bar w=`.45W`, labelH=`.12H`, 6 ticks right of bar, value bottom (no units) | zone colour |
| HorizontalLine | track y=`.55H` stroke 6 / fill 4 / dot r 7; min/max labels under ends | zone colour |
| Dashed H(14)/V(12) | `filled = ceil(pct*N)`; segVal=`min+i/N*range`; seg colour: `>=hc` critical, `>=hw` warn, else needle colour; unfilled = #401010/#403010/#303030 | uses `lim.hw/hc` |
| Histogram | decorative bell: 20 bars, `h = exp(-(i-center)^2/20)` min .05, marker line at pct | bars `>= pct(hc)` critical, `>= pct(hw)` warn |
| LineGraph | graph rect P=8; history EMA tau 250 ms/dt 100 ms; spike dots r 2.5 where `|raw-ema| > .15*range`; fallback 50 flat points | line = zone colour; labels max/min `%.0f` |
| MultiTrend | legend `cols=max(2,floor((W-12)/110))`; per-series normalisation; primary drawn last/on top | series colours from extras `lt_seriesN_*` (`lt_series1_label` for primary) |
| TelemetryStat | compact (`H<42 || H<.45W`): label left, `value units` right; full: accent stripe, big value, range bar with threshold ticks | needle colour = accent; zone colour on value |
| Analog/BasicAnalog/CircleAnalog | `radius=min(W,H)/2-8`; bezel `border_width>0 ? min(.3r,bw) : max(6,.08r)`; faceR=radius-bezel-2; zones only high_warning/high_critical arcs at faceR-4 (width `max(4,.06 faceR)`); majors `(max-min)/10` unless `major_ticks>0`; needle len `.35 faceR` default; peak wedge; cap `max(6,.1 faceR)` | dial uses `lt_gauge_resolve_arc` incl. sector faces |
| AsymmetricSweep | `cx=W/2+pivotX`, `cy=.58H+pivotY`, `radius=min(W,1.15H)/2-8`, arcW=`max(16,.18 r)`; fill start->value; tip circle | no clamp by `face_angle` |
| Round / RoundDashed | 60 segments r .55..85 / 30 dashes at 135 deg + i/29*270 deg; hard-coded 10%/25% range zones; unlit = `lighten(c,-60)` | ignores configured thresholds |
| Tachometer | fixed 135 deg + 270 deg sweep, thousands labels, redline from `.85*max`, red needle `.65 r` | ignores `needle_color` |
| AnalogBar | semicircle centre (W/2,.85H) radius `.75 min(W,H)`, 11 ticks | zone colour |
| AnalogMovingBar | 252 deg arc centre (W/2,.9H) radius `.8 min`, zones at `hw ?? min+.7r`, `hc ?? min+.9r`, needle | uses limits |
| FuelMeter | arc start .8PI sweep 1.4PI, gradient critical/warn/trim, E and F labels, clamp pct | |
| Indicator | on = value != 0 / `lt_expression`; rectangle (bg fill, 1px border, centred text), bulb (80% circle, highlight), LED = rectangle; embedded image replaces the shape when named | `st->on` |

## 4. Layout facts already done by `lt_dash_view_render`
- cluster rect: `force_aspect` letterbox (W:H from cluster), background colour fill; image backgrounds are the
  painter layer's job (decode via the GUI image loader, `LT_BG_*` styles).
- component rect = relative fractions of the cluster rect, `shortest_size` floor, `enabled_condition` skip;
  `lt_dash_view_component_rect(view, i)` and `lt_dash_view_hit(view, x, y)` give selection/click targets.
- Click actions (`short_click_action` / `long_click_action`) are strings; scripting is out of scope.
