/* dash.h - dashboards and gauges: TunerStudio .dash/.gauge model + XML IO,
 *          validation, default templates, and the framebuffer gauge renderer.
 *
 * Spec: docs/spec/05-dash-gauges.md (section numbers in comments).
 *
 * Differences from the spec text:
 *   - colours are lt_color (0xAARRGGBB, gui.h), not an {a,r,g,b} struct. The
 *     XML ARGB int32 maps 1:1 (lt_color_from_argb is just a cast). Note the
 *     reader quirk: a colour element with no alpha attribute has alpha 0.
 *   - the INI [GaugeConfigurations] type is lt_ini_gauge (ini.h); this header
 *     has lt_dash_gauge, the file-format gauge.
 *   - rendering draws with lt_gfx (gui.h) into a lt_rect; no canvas/DOM.
 *
 * Runtime model: an lt_dash_view couples a parsed lt_dash_file with per-gauge
 * runtime state (smoothed value, peak hold, spike timers, line-graph history)
 * and renders all components every frame:
 *     lt_dash_view_update(view, now_ms, &varenv.env);   (resolve channel values, EMA, history)
 *     lt_dash_view_render(view, gfx, rect, &varenv.env); (conditions, titles via lt_expr_*)
 *
 * MEMORY: lt_dash_file owns all its strings/arrays (lt_dash_free). A view
 * borrows the file (must outlive it) and owns its runtime arrays.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DASH_H
#define LT_DASH_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../gui/gui.h"
#include "../ini/ini.h"
#include "../ini/expr.h"
#include "../xml/xml.h"

/* ---- colours (spec 1.1) ---- */
lt_color lt_color_from_argb(lt_s32 v);
lt_s32   lt_color_to_argb(lt_color c);
lt_bool  lt_color_from_css(const char *s, lt_color *out);    /* "#rrggbb" or "#rrggbbaa" */
void     lt_color_to_css(lt_color c, char out[10]);
lt_color lt_color_lighten(lt_color c, int percent);          /* spec 5.3; negative = darken */
lt_color lt_color_darken(lt_color c, int percent);

/* ---- painters (spec 1.2) ---- */
typedef enum lt_gauge_painter {
    LT_GP_ANALOG = 0, LT_GP_BASIC_ANALOG, LT_GP_CIRCLE_ANALOG, LT_GP_ASYM_SWEEP, LT_GP_BASIC_READOUT,
    LT_GP_HBAR, LT_GP_VBAR, LT_GP_HLINE, LT_GP_HDASHED, LT_GP_VDASHED, LT_GP_ANALOG_BAR,
    LT_GP_ANALOG_MOVING_BAR, LT_GP_HISTOGRAM, LT_GP_LINE_GRAPH, LT_GP_ROUND, LT_GP_ROUND_DASHED,
    LT_GP_FUEL_METER, LT_GP_TACHOMETER, LT_GP_TELEMETRY_STAT, LT_GP_MULTI_TREND
} lt_gauge_painter;
typedef enum lt_ind_painter { LT_IP_RECTANGLE = 0, LT_IP_BULB, LT_IP_LED } lt_ind_painter;
typedef enum lt_bg_style { LT_BG_TILE = 0, LT_BG_STRETCH, LT_BG_CENTER, LT_BG_FIT } lt_bg_style;
typedef enum lt_res_type { LT_RES_PNG = 0, LT_RES_GIF, LT_RES_TTF } lt_res_type;
lt_gauge_painter lt_painter_from_ts(const char *s);          /* fuzzy order of spec 1.2 */
const char      *lt_painter_to_ts(lt_gauge_painter p);       /* "Analog Gauge", ... */
lt_ind_painter   lt_indpainter_from_ts(const char *s);
const char      *lt_indpainter_to_ts(lt_ind_painter p);

/* sorted string->string list for unknown properties that must round-trip */
typedef struct lt_dash_kv { char *k, *v; } lt_dash_kv;
typedef struct lt_dash_extras { lt_dash_kv *a; size_t n; } lt_dash_extras;   /* keep sorted by key */
void        lt_dash_extras_set(lt_dash_extras *e, const char *k, const char *v);
const char *lt_dash_extras_get(const lt_dash_extras *e, const char *k);

/* ---- components (spec 1.3, 1.4) ---- */
typedef struct lt_dash_gauge {
    char *id;
    lt_gauge_painter painter;
    char *gauge_style;                    /* provenance only */
    char *output_channel, *title, *units; /* title/units may be "{expr}" */
    double value, min, max;               /* value = demo value; min 0 max 100 */
    char *min_vp, *max_vp;
    lt_bool has_default_min, has_default_max; double default_min, default_max;
    lt_bool peg_limits;
    lt_bool has_low_warning, has_high_warning, has_low_critical, has_high_critical;
    double low_warning, high_warning, low_critical, high_critical;
    char *low_warning_vp, *high_warning_vp, *low_critical_vp, *high_critical_vp;
    lt_color back_color, font_color, trim_color, warn_color, critical_color, needle_color;
    int value_digits, label_digits;
    char *font_family; int font_size_adjustment; lt_bool italic_font;
    int sweep_angle, start_angle, face_angle, sweep_begin_degree; lt_bool counter_clockwise;
    double major_ticks, minor_ticks;      /* -1 = auto */
    double relative_x, relative_y, relative_width, relative_height;
    int border_width, shortest_size;
    lt_bool shape_locked_to_aspect, antialiasing_on;
    char *background_image_file_name, *needle_image_file_name;
    lt_bool has_needle_length; double needle_length;
    lt_bool has_pivot; double needle_pivot_offset_x, needle_pivot_offset_y;
    lt_bool has_needle_image_offset; double needle_image_offset_x, needle_image_offset_y;
    double history_value; int history_delay; int needle_smoothing;
    char *short_click_action, *long_click_action;
    lt_bool display_value_at_180;
    char *enabled_condition;              /* INI expression, hide when false */
    lt_bool peak_hold;                    /* serialised as ShowHistory */
    lt_bool has_hysteresis; double hysteresis;
    lt_bool run_demo;
    lt_dash_extras extras;                /* unknown TS props + lt_series* for trend panels */
} lt_dash_gauge;

typedef struct lt_dash_indicator {
    char *id; lt_ind_painter painter;
    char *output_channel; double value;
    char *on_text, *off_text;             /* "ON"/"OFF" */
    lt_color on_text_color, off_text_color, on_background_color, off_background_color;
    char *on_image_file_name, *off_image_file_name;
    double relative_x, relative_y, relative_width, relative_height;
    char *font_family; lt_bool italic_font, antialiasing_on;
    char *short_click_action, *long_click_action, *ecu_configuration_name, *enabled_condition;
    lt_bool run_demo;
    lt_dash_extras extras;
} lt_dash_indicator;

typedef struct lt_dash_comp {
    lt_dash_gauge *gauge;                 /* exactly one of gauge/indicator is non-NULL */
    lt_dash_indicator *indicator;
} lt_dash_comp;

typedef struct lt_dash_image {
    char *file_name, *image_id; lt_res_type type; char *data_base64;
} lt_dash_image;

typedef struct lt_dash_cluster {
    lt_bool anti_aliasing, force_aspect; double force_aspect_width, force_aspect_height;
    lt_bool has_dither; lt_color dither_color;
    lt_color background_color;
    char *background_image_file_name; lt_bg_style background_image_style;
    lt_dash_image *images; size_t n_images;
    lt_dash_comp *comps; size_t n_comps;
    char *layout, *enabled_condition;
    lt_dash_extras extras;
} lt_dash_cluster;

typedef struct lt_dash_file {
    char *author, *company, *write_date;
    char *file_format;                    /* "3.0" */
    char *firmware_signature;             /* may be NULL */
    lt_dash_cluster cluster;              /* only the first <gaugeCluster> is kept */
} lt_dash_file;

typedef struct lt_gauge_file {            /* .gauge: bibliography + embedded images + one gauge */
    char *author, *company, *write_date, *file_format;
    lt_dash_image *images; size_t n_images;
    lt_dash_gauge gauge;
} lt_gauge_file;

/* ---- construction / free ---- */
void lt_dash_gauge_init(lt_dash_gauge *g);        /* spec defaults */
void lt_dash_gauge_free(lt_dash_gauge *g);        /* frees owned strings (not the struct itself) */
void lt_dash_indicator_init(lt_dash_indicator *i);
void lt_dash_indicator_free(lt_dash_indicator *i);
lt_dash_file *lt_dash_new(void);                  /* default cluster, empty */
void          lt_dash_free(lt_dash_file *d);
lt_dash_gauge     *lt_dash_add_gauge(lt_dash_file *d);        /* appended, defaults, owned by d */
lt_dash_indicator *lt_dash_add_indicator(lt_dash_file *d);
void lt_dash_remove(lt_dash_file *d, size_t index);

/* ---- XML IO (spec 2) via xml.h. Errors: LT_EDATA with message. ---- */
lt_dash_file *lt_dash_parse(const char *xml, size_t len, char *err, size_t errlen);
void          lt_dash_write(const lt_dash_file *d, lt_buf *out);
lt_dash_file *lt_dash_load(const char *path);
int           lt_dash_save(const lt_dash_file *d, const char *path);
int  lt_gaugefile_parse(const char *xml, size_t len, lt_gauge_file *out, char *err, size_t errlen);
void lt_gaugefile_write(const lt_gauge_file *g, lt_buf *out);
void lt_gaugefile_free(lt_gauge_file *g);

/* ---- validation (spec 3) ---- */
typedef enum lt_dash_issue_kind {
    LT_DI_EMPTY = 0, LT_DI_UNKNOWN_CHANNEL, LT_DI_INVALID_RANGE, LT_DI_MISSING_IMAGE,
    LT_DI_TINY_GAUGE, LT_DI_OUT_OF_BOUNDS, LT_DI_OVERLAP, LT_DI_PERFORMANCE
} lt_dash_issue_kind;
typedef struct lt_dash_issue { lt_dash_issue_kind kind; lt_bool is_error; char gauge_id[64]; char message[160]; } lt_dash_issue;
typedef struct lt_dash_report {
    lt_dash_issue *problems; size_t n;
    size_t gauge_count, indicator_count, unique_channels, embedded_image_count; lt_bool has_embedded_fonts;
    lt_bool is_valid;                    /* no error problems */
} lt_dash_report;
/* channels: INI output channel names to check against, or NULL to skip that check. */
void lt_dash_validate(const lt_dash_file *d, const char *const *channels, size_t n_channels,
                      lt_dash_report *out);
void lt_dash_report_free(lt_dash_report *r);

/* ---- templates (spec 7) ---- */
lt_dash_file *lt_dash_template_basic(void);           /* 10 gauges */
lt_dash_file *lt_dash_template_tuning(void);          /* 15 gauges */
lt_dash_file *lt_dash_template_telemetry(void);       /* dense 16:10 grid */
/* Build a dash from the INI [FrontPage]: gauge1..N resolved via [GaugeConfigurations]
 * (limits that are {expr} resolved with env), indicators as dash indicators laid out in a strip. */
lt_dash_file *lt_dash_from_frontpage(const lt_ini *ini, const lt_expr_env *env);
/* ADDITIVE (integration): rewrite generic template channel names (rpm, afr, map, ...) to the
 * INI's real output channel names (RPMValue, afrGasolineScale, ...). Returns the number of
 * component channels that still do not exist in the INI. */
size_t lt_dash_resolve_channels(lt_dash_file *d, const lt_ini *ini);
/* One INI gauge -> dash gauge (painter chosen by name hints: tachometer/rpm -> LT_GP_TACHOMETER ...). */
void lt_dash_gauge_from_ini(const lt_ini_gauge *src, lt_dash_gauge *dst, const lt_expr_env *env);

/* ====================================================================== */
/* Runtime + rendering                                                     */
/* ====================================================================== */
typedef struct lt_dash_view lt_dash_view;
lt_dash_view *lt_dash_view_new(const lt_dash_file *d);
void   lt_dash_view_free(lt_dash_view *v);
void   lt_dash_view_set_redraw_hz(lt_dash_view *v, int hz);          /* 10,15,20,25,30 (default 30) */
/* Pull current values from env (get_var by output_channel), advance smoothing
 * (EMA tau 120 ms), peak hold, spike flash, history rings. Also resolves
 * {expr} limits lazily (cached by expression text). */
void   lt_dash_view_update(lt_dash_view *v, double now_ms, const lt_expr_env *env);
/* Draw the whole cluster into `area` (letterboxed when force_aspect). Components whose
 * enabled_condition is false are skipped. */
void   lt_dash_view_render(lt_dash_view *v, lt_gfx *g, lt_rect area, const lt_expr_env *env);
/* Draw one standalone gauge/indicator (preview in the designer/gauge picker). */
void   lt_dash_render_gauge(lt_gfx *g, lt_rect r, const lt_dash_gauge *cfg, double value);
void   lt_dash_render_indicator(lt_gfx *g, lt_rect r, const lt_dash_indicator *ind, lt_bool on);
/* Hit test: component index under (x,y) of the last render, or -1 (designer selection, click actions). */
int    lt_dash_view_hit(const lt_dash_view *v, int x, int y);
lt_rect lt_dash_view_component_rect(const lt_dash_view *v, size_t index);
/* terminal (cell) UI: lay gauges out as a filling grid of panels and indicators as pills,
 * ignoring the file's positions (view mode only) */
void   lt_dash_view_set_cell_auto(lt_dash_view *v, int on);
/* Zone helpers used by painters (spec 5.2): colour for value with threshold hysteresis. */
lt_color lt_dash_value_color(const lt_dash_gauge *g, double v, int *prev_zone);
/* Arc geometry of the analog dial (spec 5.4). */
typedef struct lt_gauge_arc {
    double start_deg, sweep_deg, face_start_deg, face_deg; lt_bool ccw, full_circle;
} lt_gauge_arc;
void lt_gauge_resolve_arc(const lt_dash_gauge *g, lt_gauge_arc *out);
/* EMA step used for display smoothing; dt<=0 returns prev (spec 5.2). */
double lt_gauge_ema_step(double prev, double target, double dt_ms);


/* ====================================================================== */
/* Additive: runtime state helpers (spec 5.2) and painter hooks            */
/* ====================================================================== */
#define LT_DASH_MAX_SERIES 12            /* MultiChannelTrend: primary + lt_series2..12 */
#define LT_DASH_HIST_LEN   600           /* raw samples kept per series (one per >=100 ms) */
#define LT_DASH_SPIKE_MS   400.0

/* Zones: 0 normal, 1 warning, 2 critical. Thresholds with has_* flags. */
typedef struct lt_dash_limits {
    double min, max;
    lt_bool has_lw, has_hw, has_lc, has_hc;
    double lw, hw, lc, hc;
} lt_dash_limits;
/* limits straight from the file values (no VP resolution) */
void lt_dash_limits_from_gauge(const lt_dash_gauge *g, lt_dash_limits *out);
/* Zone with hysteresis (spec 5.2). *prev_zone may be NULL (then treated as 0, not saved). */
int  lt_dash_zone(const lt_dash_limits *l, double hysteresis, double v, int *prev_zone);

/* pure helpers, spec 8 test list */
lt_bool lt_gauge_is_spike(double raw, double smoothed, double min, double max);
typedef struct lt_peak_state { double peak, clock_ms; lt_bool started; } lt_peak_state;
double  lt_gauge_seed_peak(const lt_dash_gauge *g, double fallback, double min, double max);
/* one frame of peak tracking against `target` at now_ms; returns the peak */
double  lt_gauge_peak_step(lt_peak_state *s, double target, double now_ms, int history_delay);
int     lt_gauge_stagger_slot(const char *channel, int *counter);   /* djb2 mod 16 */

/* per-series ring of raw samples (oldest first via lt_dash_ring_get) */
typedef struct lt_dash_ring { double *d; int cap, head, n; } lt_dash_ring;
void   lt_dash_ring_init(lt_dash_ring *r, int cap);
void   lt_dash_ring_free(lt_dash_ring *r);
void   lt_dash_ring_push(lt_dash_ring *r, double v);
double lt_dash_ring_get(const lt_dash_ring *r, int i);               /* 0 = oldest */

/* Runtime state of one component, valid after lt_dash_view_update. */
typedef struct lt_dash_gstate {
    lt_bool is_gauge, enabled, has_value;
    double raw;                      /* channel value (or file demo value) */
    double display;                  /* smoothed + pegged value, for painters */
    double last_ms, last_spike_ms;   /* last_spike_ms < 0: none */
    lt_peak_state peak;              /* gauges with peak_hold */
    int zone;                        /* remembered zone for hysteresis */
    lt_dash_limits lim;              /* effective limits: *_vp names resolved through env */
    lt_bool on;                      /* indicators */
    int slot;                        /* stagger slot 0..15 */
    long last_frame;
    int n_series;                    /* line graph: 1, multi trend: 1..12 */
    char *series_ch[LT_DASH_MAX_SERIES];
    double series_latest[LT_DASH_MAX_SERIES];
    lt_dash_ring ring[LT_DASH_MAX_SERIES];
    double last_hist_ms;
} lt_dash_gstate;
lt_bool lt_dash_gstate_spiking(const lt_dash_gstate *s, double now_ms);
const lt_dash_gstate *lt_dash_view_state(const lt_dash_view *v, size_t index);
size_t lt_dash_view_count(const lt_dash_view *v);
/* LT_TRUE when component should repaint in this tick given redraw_hz and its phase slot */
lt_bool lt_dash_view_due(lt_dash_view *v, size_t index, double now_ms);

/* Painters are supplied by the GUI layer; without them a plain fallback is drawn. */
typedef struct lt_dash_painters {
    void *ud;
    void (*gauge)(void *ud, lt_gfx *g, lt_rect r, const lt_dash_gauge *cfg,
                  const lt_dash_gstate *st, double now_ms);
    void (*indicator)(void *ud, lt_gfx *g, lt_rect r, const lt_dash_indicator *ind, lt_bool on);
} lt_dash_painters;
void lt_dash_set_painters(const lt_dash_painters *p);   /* NULL restores the fallback */
/* Indicators built from [FrontPage] keep their expression in extras "lt_expression"
 * (evaluated with lt_expr_*, truthy = on); otherwise on = (channel value != 0). */

#endif /* LT_DASH_H */
