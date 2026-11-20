/* ini.h - ECU definition model (TunerStudio/MegaSquirt .ini) + parser + value codec.
 *
 * Specs: docs/spec/01-ini-core.md (parser/model), docs/spec/02-ini-expr-ui.md
 * (channels, menus, dialogs, gauges, frontpage, datalog, loggers).
 *
 * One parse produces one immutable-ish lt_ini. Consumers (proto, tune, dash,
 * gui, sim, autotune, basemap) only read it, with these mutable exceptions:
 *   - lt_ini_resolve_dynamic()  updates scale/translate/min/max of constants
 *     whose INI fields were {expr}s (after a tune is loaded)
 *   - lt_ini_std_panel()        lazily synthesises built-in panels
 *
 * MEMORY: everything reachable from an lt_ini (strings, arrays, element
 * structs) is owned by it and freed by lt_ini_free(). Element pointers are
 * stable for the life of the lt_ini (individually allocated or arena backed),
 * so the GUI may keep `const lt_ini_const *` etc. across frames. Never free or
 * modify them yourself. Arrays of elements are in FILE ORDER; name lookups are
 * via the lt_ini_find_* functions (names are case-SENSITIVE; section names,
 * keys and keywords in the file are case-insensitive).
 *
 * Strings that may hold NUL bytes (ECU commands) are lt_ini_blob (p,n).
 * Conditions ("vis_cond", "en_cond", "condition", "*_expr") are the trimmed
 * INNER text of { ... }, ready for lt_expr_* (see expr.h); NULL = none.
 *
 * Errors: loaders return NULL and set lt_err_set() (code in *err_out if given).
 * Malformed individual lines are silently dropped (spec 01 section 1).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_INI_H
#define LT_INI_H

#include "../lt.h"
#include "../lt_codes.h"
#include "expr.h"

/* ====================================================================== */
/* Basic types                                                             */
/* ====================================================================== */

typedef enum lt_endian { LT_ENDIAN_BIG = 0, LT_ENDIAN_LITTLE = 1 } lt_endian;

typedef enum lt_dtype {
    LT_DT_U08 = 0, LT_DT_S08, LT_DT_U16, LT_DT_S16, LT_DT_U32, LT_DT_S32,
    LT_DT_F32, LT_DT_F64, LT_DT_STRING, LT_DT_BITS
} lt_dtype;

typedef enum lt_ecu_type {
    LT_ECU_UNKNOWN = 0, LT_ECU_SPEEDUINO, LT_ECU_RUSEFI, LT_ECU_FOME,
    LT_ECU_EPICEFI, LT_ECU_MS2, LT_ECU_MS3
} lt_ecu_type;

/* Byte string with length; p is always NUL-terminated for convenience, n
 * excludes that terminator and the content may contain NUL bytes. */
typedef struct lt_ini_blob { char *p; size_t n; } lt_ini_blob;

typedef enum lt_shape_kind { LT_SH_SCALAR = 0, LT_SH_1D, LT_SH_2D } lt_shape_kind;
typedef struct lt_shape {
    lt_shape_kind kind;
    size_t n;            /* 1D element count */
    size_t rows, cols;   /* 2D (note "[AxB]" = cols A, rows B: column first!) */
} lt_shape;
size_t lt_shape_count(const lt_shape *s);   /* element count: scalar 1, 1D n, 2D rows*cols */
size_t lt_shape_x_size(const lt_shape *s);  /* scalar 1, 1D n, 2D cols */
size_t lt_shape_y_size(const lt_shape *s);  /* 2D rows, else 1 */

/* Dynamic array dimensions "[{cols}x{rows}]" / "[{count}]" (names of constants). */
typedef struct lt_dynsize {
    char *cols_const;    /* NULL for 1D */
    char *rows_const;    /* count constant for 1D */
} lt_dynsize;

/* ---- data type helpers (spec 01 section 6.3) ---- */
/* Accepts U08/UINT8/BYTE/BU08/... (case-insensitive). endian_override gets -1
 * (none) or LT_ENDIAN_BIG for the B* spellings. Returns 0 or LT_EINVAL. */
int    lt_dtype_parse(const char *s, lt_dtype *t, int *endian_override);
const char *lt_dtype_name(lt_dtype t);        /* "U08","S08",...,"String","Bits" (manifest names) */
size_t lt_dtype_size(lt_dtype t);             /* bytes: 1,1,2,2,4,4,4,8, STRING 0, BITS 1 */
/* Read one element at byte offset off; LT_ERANGE if off+size > len. */
int    lt_dtype_read(lt_dtype t, const lt_u8 *data, size_t len, size_t off, lt_endian e,
                     double *out);
/* Write one element. Integers ROUND (half away from zero) then saturate; F32
 * narrows; out of range offset -> LT_ERANGE and nothing written. */
int    lt_dtype_write(lt_dtype t, lt_u8 *data, size_t len, size_t off, lt_endian e, double v);
/* Raw (stored) value range of an integer type; returns LT_FALSE for F32/F64/STRING. */
lt_bool lt_dtype_range(lt_dtype t, double *lo, double *hi);

/* ---- bit fields spanning bytes (spec 01 6.3 "Multi-byte tune bit extraction") ---- */
/* bit_pos is relative to byte at `off`; pos>=8 reaches following bytes. */
int lt_bits_get(const lt_u8 *data, size_t len, size_t off, int bit_pos, int bit_size,
                lt_u32 *out);
int lt_bits_set(lt_u8 *data, size_t len, size_t off, int bit_pos, int bit_size, lt_u32 value);

/* ====================================================================== */
/* Page access abstraction                                                 */
/* ====================================================================== */
/* Anything that holds page images (lt_ecu_mem in proto.h, lt_tune in tune.h,
 * a test buffer) exposes them through this pair of callbacks, so the INI
 * codec, table views, basemap and sim need no knowledge of who owns the bytes.
 * `page` is the 0-based page index. */
typedef struct lt_pagesrc {
    void *ud;
    /* read view of a page image or NULL if the page is not available/loaded. */
    const lt_u8 *(*page)(void *ud, int page, size_t *len);
    /* write bytes into a loaded page and mark them dirty. 0 ok, <0 LT_ERANGE/LT_ENOENT. */
    int (*write)(void *ud, int page, size_t off, const lt_u8 *data, size_t n);
} lt_pagesrc;

/* ====================================================================== */
/* [Constants], [PcVariables]                                              */
/* ====================================================================== */

typedef struct lt_ini_const {
    char     *name;
    lt_u8     page;            /* 0-based; 255 for PC variables */
    lt_u32    offset;          /* byte offset in page (lastOffset already resolved) */
    lt_dtype  type;            /* LT_DT_BITS for bits constants */
    int       endian_override; /* -1 none, else lt_endian (B* type spellings) */
    lt_shape  shape;           /* resolved (dynamic shapes already resolved, see dyn) */
    lt_dynsize *dyn;           /* non-NULL if the INI used {name} dimensions */
    int       bit_pos, bit_size, bit_disp;  /* bits only (-1 if absent); bit_disp = display offset */
    char     *units;           /* may be a "{expr}" text, "" if none */
    double    scale, translate, min, max;   /* display = raw*scale + translate */
    int       digits;
    char     *help;            /* NULL if none */
    char    **bit_options;     /* positional by raw value, NULL = hole/unnamed */
    size_t    n_bit_options;
    lt_bool   is_pc_var;
    /* deferred {expr} fields (NULL if literal); see lt_ini_resolve_dynamic */
    char     *scale_expr, *translate_expr, *min_expr, *max_expr;
    lt_bool   range_resolved;  /* min/max usable for clamping */
    size_t    size_bytes;      /* bytes occupied in the page (BITS: 0; STRING: length) */
    size_t    order;           /* index in lt_ini.constants */
} lt_ini_const;

/* label of option `idx` or NULL */
const char *lt_ini_bit_option(const lt_ini_const *c, size_t idx);
/* index of option with this label (exact, then case-insensitive) or -1 */
int         lt_ini_bit_index(const lt_ini_const *c, const char *label);
double lt_const_raw_to_display(const lt_ini_const *c, double raw);
double lt_const_display_to_raw(const lt_ini_const *c, double display);  /* clamps if range_resolved */
lt_bool lt_const_in_range(const lt_ini_const *c, double display);

/* ====================================================================== */
/* [OutputChannels]                                                        */
/* ====================================================================== */

typedef struct lt_ini_channel {
    char     *name;
    lt_dtype  type;            /* LT_DT_BITS for bit channels; F32 for computed */
    lt_u32    offset;
    int       bit_pos, bit_count;   /* -1 if not a positioned bit channel */
    lt_dtype  bit_storage;     /* backing word type of a bits channel (default U08) */
    char     *units;
    double    scale, translate;
    char     *expression;      /* computed channel (inner text) or NULL */
    lt_expr  *ast;             /* compiled expression (may be NULL on parse error) */
    size_t    index;           /* position in lt_ini.channels (= slot in lt_ini_rt) */
} lt_ini_channel;

/* Decode a raw (non computed) channel from a realtime block. LT_ERANGE if the
 * block is too short, LT_EINVAL for computed channels. Applies scale/translate. */
int lt_ini_channel_decode(const lt_ini_channel *ch, lt_endian e, const lt_u8 *block,
                          size_t len, double *out);
size_t lt_ini_channel_size(const lt_ini_channel *ch);   /* 0 for computed */

/* ====================================================================== */
/* Protocol settings (spec 01 sections 5, 7.1, 7.2, 7.13)                  */
/* ====================================================================== */

typedef struct lt_ini_cmdlist { lt_ini_blob *v; size_t n; } lt_ini_cmdlist;

typedef struct lt_ini_protocol {
    char        *message_envelope_format;  /* NULL, or e.g. "msEnvelope_1.0" */
    lt_bool      uses_modern;              /* envelope format contains "msEnvelope" */
    lt_ini_blob  query_command;            /* default "Q"; text with \xNN escapes (NOT decoded) */
    unsigned long delay_after_port_open;   /* ms, default 0 */
    /* per page (index = page number); empty blob = "none for this page" */
    lt_ini_blob *page_identifiers;         /* DECODED bytes (parse_escape + $var substitution) */
    size_t       n_page_identifiers;
    unsigned long *page_sizes;             /* bytes */
    size_t       n_page_sizes;
    lt_ini_cmdlist page_read_commands;     /* "R%2i%2o%2c" etc, escapes left as text */
    lt_ini_cmdlist page_chunk_write_commands;
    lt_ini_cmdlist burn_commands;
    lt_ini_cmdlist crc32_check_commands;
    lt_ini_cmdlist page_activate_commands;
    lt_ini_blob  och_get_command;          /* n==0 if none */
    unsigned long och_block_size;          /* 0 unknown */
    unsigned long max_unused_runtime_range;
    lt_ini_blob  burst_get_command;        /* default "A" */
    char        *retrieve_config_error;
    unsigned long page_activation_delay;   /* ms, default 500 */
    unsigned long blocking_factor;         /* default 256 */
    unsigned long inter_write_delay;       /* ms, default 0 */
    unsigned long block_read_timeout;      /* ms, default 1000 */
    int          write_blocks;             /* default 1 */
    int          enable_can_id;            /* enable2ndByteCanID, default 1 */
    unsigned long default_baud_rate;       /* 115200 */
    char        *default_ip_address;       /* NULL */
    int          default_ip_port;          /* 29001 */
    char        *table_write_command;      /* [ReferenceTables] */
    char        *table_crc_command;
    int          table_blocking_factor;    /* -1 none */
    unsigned long no_comm_read_delay;
    int          default_runtime_record_per_sec;   /* -1 none */
    /* misc boolean flags (all default 0) */
    int refresh_local_store_on_activity, restrict_squirt_relationship,
        force_big_endian_protocol, use_legacy_f_temp_units, suppress_config_error_verbiage,
        validate_array_bounds, ignore_missing_bit_options, filter_echo_bytes,
        enveloped_scan_commands;
    /* [BurstMode] getCommand is burst_get_command above */
    char       **page_identifiers_raw;     /* INI text before $var substitution */
    size_t       n_page_identifiers_raw;
} lt_ini_protocol;

/* ====================================================================== */
/* Tables and curves ([TableEditor], [CurveEditor])                        */
/* ====================================================================== */

typedef enum lt_table_role {
    LT_ROLE_OTHER = 0, LT_ROLE_VE, LT_ROLE_IGNITION, LT_ROLE_AFR_TARGET, LT_ROLE_WARMUP
} lt_table_role;

typedef struct lt_ini_table {
    char *name, *map_name, *title;
    char *z_const;                 /* zBins constant (the cell values) */
    char *x_const, *x_channel;     /* xBins constant, optional live output channel (cursor) */
    char *y_const, *y_channel;     /* yBins (NULL for 2-D "curve like" tables) */
    char *help;                    /* topicHelp */
    char *x_label, *y_label;       /* xyLabels (may be {expr}: use lt_expr_display) */
    char *up_label, *down_label;   /* upDownLabel */
    lt_bool is_3d;
    int   page;                    /* NOT normalised to 0-based (see spec 01 7.9) */
    size_t x_size, y_size;         /* resolved after parse (cols, rows) */
    lt_bool has_grid_height; double grid_height;
    lt_bool has_orient; double orient[3];     /* yaw, pitch, roll */
    char *rows_size_const, *cols_size_const; /* resizable table: size constants */
    size_t max_elements;
    lt_table_role role;
} lt_ini_table;

typedef struct lt_ini_curve {
    char *name, *title;
    char *x_const, *x_channel;     /* xBins */
    char *y_const;                 /* yBins (first one) */
    char *y_const2;                /* second yBins line if the curve overlays two, else NULL */
    char *col_x, *col_y;           /* columnLabel */
    lt_bool has_xaxis; double xaxis[3];   /* min,max,step */
    lt_bool has_yaxis; double yaxis[3];
    int   size;                    /* -1 unknown */
    char *help, *gauge;
    char **line_labels; size_t n_line_labels;
} lt_ini_curve;

/* ====================================================================== */
/* Menus, dialogs, help ([Menu], [UserDefined])                            */
/* ====================================================================== */

typedef enum lt_menu_kind {
    LT_MENU_DIALOG = 0, LT_MENU_TABLE, LT_MENU_SUBMENU, LT_MENU_STD, LT_MENU_HELP,
    LT_MENU_SEPARATOR
} lt_menu_kind;

/* Visibility/enable are evaluated by the GUI each frame with
 * lt_expr_cache_test(cache, item->vis_cond, env) (NULL cond = true). */
typedef struct lt_ini_menu_item {
    lt_menu_kind kind;
    char *label, *target;          /* target: dialog/table/curve/help name or std_* id */
    char *vis_cond, *en_cond;
    struct lt_ini_menu_item **items;   /* SUBMENU children */
    size_t n_items;
} lt_ini_menu_item;

typedef struct lt_ini_menu {       /* a top level menu-bar entry */
    char *name, *title;
    lt_ini_menu_item **items; size_t n_items;
} lt_ini_menu;

typedef enum lt_comp_kind {
    LT_COMP_LABEL = 0, LT_COMP_PANEL, LT_COMP_FIELD, LT_COMP_CHECKBOX, LT_COMP_SLIDER,
    LT_COMP_DISPLAYONLY, LT_COMP_SETTINGSEL, LT_COMP_RUNTIMEVALUE, LT_COMP_LIVEGRAPH,
    LT_COMP_TABLE, LT_COMP_INDICATOR, LT_COMP_CMDBUTTON, LT_COMP_GAUGE
} lt_comp_kind;

typedef struct lt_ini_assign { char *name; double value; } lt_ini_assign;
typedef struct lt_ini_setting_opt {
    char *label; lt_ini_assign *assigns; size_t n_assigns;
} lt_ini_setting_opt;

typedef enum lt_close_behavior {
    LT_CLOSE_NONE = 0, LT_CLOSE_ALWAYS, LT_CLOSE_IF_ENABLED, LT_CLOSE_IF_DISABLED
} lt_close_behavior;

/* One dialog component. Which fields are meaningful depends on `kind`:
 *  LABEL        label (text; leading '#' = section header, '!' = note)
 *  PANEL        name (dialog or std_* panel to embed), position, vis/en
 *  FIELD        label, name (constant), vis/en
 *  CHECKBOX     label, name, flip, vis/en
 *  SLIDER       label, name, orientation ("horizontal"/"vertical"/NULL), vis/en
 *  DISPLAYONLY  label, name (may be NULL), vis/en
 *  SETTINGSEL   label, options[], vis
 *  RUNTIMEVALUE label, name (output channel), vis
 *  LIVEGRAPH    name, label (title), position, channels[]
 *  TABLE        name
 *  INDICATOR    expression, label_off, label_on (labels may be {expr})
 *  CMDBUTTON    label, name (controller command), en, close
 *  GAUGE        name (gauge config), position */
typedef struct lt_ini_comp {
    lt_comp_kind kind;
    char *label, *name, *position, *vis_cond, *en_cond;
    lt_bool flip;
    char *orientation;
    lt_close_behavior close;
    char *expression, *label_on, *label_off;
    char **channels; size_t n_channels;
    lt_ini_setting_opt *options; size_t n_options;
} lt_ini_comp;

typedef struct lt_ini_dialog {
    char *name, *title;
    char *layout_hint;             /* "xAxis", "border" or NULL */
    char *help_topic;              /* help url / topic or NULL */
    lt_ini_comp *comps; size_t n_comps;      /* in order */
} lt_ini_dialog;

typedef struct lt_ini_indicator {
    char *expression, *label_off, *label_on;
    char *off_fg, *off_bg, *on_fg, *on_bg;   /* colour names or #rrggbb; NULL = default. INI order:
                                             * offBg, offFg, onBg, onFg (as [FrontPage]) */
} lt_ini_indicator;
typedef struct lt_ini_indicator_panel {
    char *name; int columns; char *vis_cond;
    lt_ini_indicator *inds; size_t n_inds;
} lt_ini_indicator_panel;
typedef struct lt_ini_readout { char *channel, *title, *units; int digits, precision; } lt_ini_readout;
typedef struct lt_ini_readout_panel {
    char *name; int columns; char *vis_cond;
    lt_ini_readout *readouts; size_t n_readouts;
} lt_ini_readout_panel;

typedef struct lt_ini_help {
    char *name, *title, *web_url;
    char **lines; size_t n_lines;
} lt_ini_help;

typedef struct lt_ini_setting_group {
    char *name, *label;
    struct { char *value, *label; } *options; size_t n_options;
} lt_ini_setting_group;

/* [ControllerCommands]: name = "raw", otherCmd, ... */
typedef struct lt_ini_cmd_part { lt_bool is_ref; lt_ini_blob data; /* raw bytes (escapes as text) or ref name */ } lt_ini_cmd_part;
typedef struct lt_ini_cmd {
    char *name; char *enable_cond;
    lt_ini_cmd_part *parts; size_t n_parts;     /* execute in order */
} lt_ini_cmd;

/* ====================================================================== */
/* Gauges, FrontPage, datalog, loggers                                     */
/* ====================================================================== */

/* [GaugeConfigurations] entry. NOTE: dash.h has its own richer gauge type. */
typedef struct lt_ini_gauge {
    char *name, *channel, *title, *units;      /* title/units may be {expr} */
    double lo, hi, low_danger, low_warning, high_warning, high_danger;
    int digits;
    char *lo_expr, *hi_expr, *low_danger_expr, *low_warning_expr,
         *high_warning_expr, *high_danger_expr;   /* resolve with lt_expr against the tune */
    char *category;                              /* last gaugeCategory seen, or NULL */
} lt_ini_gauge;
lt_bool lt_ini_gauge_is_danger(const lt_ini_gauge *g, double v);
lt_bool lt_ini_gauge_is_warning(const lt_ini_gauge *g, double v);
lt_bool lt_ini_gauge_is_normal(const lt_ini_gauge *g, double v);

typedef struct lt_ini_fp_indicator {
    char *expression, *label_off, *label_on;
    char *bg_off, *fg_off, *bg_on, *fg_on;      /* INI order offBg, offFg, onBg, onFg */
} lt_ini_fp_indicator;
typedef struct lt_ini_frontpage {
    char **gauges; size_t n_gauges;             /* gauge1..gaugeN names ("" = unset) */
    lt_ini_fp_indicator *inds; size_t n_inds;
} lt_ini_frontpage;
/* Colour name (white, red, lime, ...) or "#rgb"/"#rrggbb" -> 0xAARRGGBB; LT_FALSE if unknown. */
lt_bool lt_ini_parse_color(const char *s, lt_u32 *argb);

/* [Datalog] entry = channel, "Label", type, "fmt"[, {cond}] */
typedef struct lt_ini_dl_entry {
    char *channel, *label, *type, *format, *condition;
    lt_bool enabled;
} lt_ini_dl_entry;
typedef struct lt_ini_dl_view { char *name, *label; char **channels; size_t n_channels; } lt_ini_dl_view;

/* [LoggerDefinition] block (tooth/composite logger): spec 02 section 5.2 */
typedef struct lt_ini_logger_field {
    char *name, *label, *units; unsigned start_bit, bit_count; double scale;
} lt_ini_logger_field;
typedef struct lt_ini_logger {
    char *name, *label, *kind;
    lt_ini_blob start_command, stop_command, data_read_command;  /* escapes as text, $var substituted */
    unsigned long data_read_timeout_ms; lt_bool continuous_read;
    char *data_ready_condition;
    size_t header_len, footer_len, record_len;
    lt_ini_logger_field *fields; size_t n_fields;
} lt_ini_logger;
/* Decode one record: vals[n_fields], present[n_fields]; returns count decoded. */
size_t  lt_ini_logger_decode(const lt_ini_logger *lg, const lt_u8 *rec, size_t len,
                             double *vals, int *present);
lt_bool lt_ini_logger_is_empty(const lt_ini_logger *lg, const lt_u8 *rec, size_t len);
size_t  lt_ini_logger_record_count(const lt_ini_logger *lg, size_t payload_len);

/* [PortEditor]: TunerStudio programmable on/off outputs (MS2/MS3 "std_port_edit").
 * Either "portEditor = name, \"Title\"" or "name = \"Title\"" starts an editor; the keys below
 * attach to the last one. Each *_const names an array constant with one element per port
 * (NULL when the INI does not map it):
 *   enabledPorts = c[, "port 1", "port 2", ...]   per port on/off (labels name the ports)
 *   outputCanID, outputOffset, outputSize          channel of condition 1 (offset/size of the
 *                                                  output channel in the realtime block)
 *   operators = c[, "<", "=", ">", "&"]            operator; stored as the character code
 *   threshold, hysteresis                          condition 1 values (raw channel units)
 *   powerOnValue, triggerValue                     pin state at power on / when active
 *   conditionRelationship = c[, " ", "&", "|"]     joins condition 1 and 2 (char code)
 *   outputOffset2, outputSize2, operators2, threshold2, hysteresis2   condition 2
 *   portNames = "a", "b", ...                      alternative to the enabledPorts labels */
typedef struct lt_ini_port_editor {
    char *name, *label, *enable_cond, *help_topic;
    char *enabled_const, *can_id_const, *offset_const, *size_const, *op_const,
         *threshold_const, *hysteresis_const, *power_on_const, *trigger_const,
         *connector_const, *offset2_const, *size2_const, *op2_const, *threshold2_const,
         *hysteresis2_const;
    char **port_names; size_t n_port_names;
    char **op_labels;  size_t n_op_labels;      /* empty = "<", "=", ">", "&" */
    char **conn_labels; size_t n_conn_labels;   /* empty = " ", "&", "|" */
} lt_ini_port_editor;
typedef struct lt_ini_key_action { char *key, *action, *label, *cond; } lt_ini_key_action;

/* ====================================================================== */
/* Reference tables (sensor calibration), analyze sections                 */
/* ====================================================================== */

typedef struct lt_ini_ref_ident {
    lt_u8 id; char *label;
    lt_bool has_limits; double lim_min, lim_max, lim_default;
} lt_ini_ref_ident;
typedef struct lt_ini_therm_opt {
    char *name; double bias_resistor; double t[3], r[3];   /* (tempC, ohms) x3 */
} lt_ini_therm_opt;
typedef struct lt_ini_solution {
    char *label;
    char *expression;     /* inner text, or NULL when generator */
    char *generator;      /* generator name, or NULL */
} lt_ini_solution;
typedef struct lt_ini_generator {
    char *kind, *label, *x_units, *y_units;
    lt_bool has_bounds; double x_lo, x_hi, y_lo, y_hi;
} lt_ini_generator;
typedef struct lt_ini_ref_table {
    char *name, *label, *help_topic, *solutions_label;
    lt_ini_ref_ident *idents; size_t n_idents;
    size_t adc_count; unsigned bytes_per_adc; double scale;
    lt_ini_generator *generators; size_t n_generators;
    lt_ini_therm_opt *therm_opts; size_t n_therm_opts;
    lt_ini_solution *solutions; size_t n_solutions;
} lt_ini_ref_table;
size_t lt_ini_ref_wire_bytes(const lt_ini_ref_table *t);   /* adc_count*bytes_per_adc */

typedef enum lt_filter_op {
    LT_FOP_EQ = 0, LT_FOP_LT, LT_FOP_GT, LT_FOP_NE, LT_FOP_BITAND, LT_FOP_BITOR
} lt_filter_op;
typedef struct lt_ini_filter {      /* [VeAnalyze]/[WueAnalyze]/[GammaE] filter = ... */
    char *name, *display, *channel; lt_filter_op op; double dflt; lt_bool adjustable;
} lt_ini_filter;
typedef struct lt_ini_analyze {
    lt_bool present;
    char *ve_table, *target_table, *lambda_channel, *ego_channel, *active_cond;   /* VeAnalyze */
    char *wue_curve, *afr_comp_curve, *wue_channel, *coolant_channel;               /* WueAnalyze */
    char *gamma_table;                                                              /* GammaE */
    char **lambda_target_tables; size_t n_lambda_target_tables;
    double wue_percent_offset;
    lt_ini_filter *filters; size_t n_filters;
    char **options; size_t n_options;
} lt_ini_analyze;

/* ====================================================================== */
/* The definition                                                          */
/* ====================================================================== */

typedef struct lt_ini {
    /* identity */
    lt_ecu_type ecu_type;
    char *signature, *signature_prefix, *version_info, *ini_spec_version;
    char *source_path;               /* file parsed, or NULL for string parses */
    lt_endian endian;                /* default BIG; governs %2i/%2o/%2c and page data */
    size_t n_pages;
    lt_ini_protocol proto;

    /* [Constants] + [PcVariables] (pc variables have is_pc_var and page 255) */
    lt_ini_const   **constants; size_t n_constants;
    lt_map           constant_idx;   /* name -> lt_ini_const* */
    lt_ini_channel **channels;  size_t n_channels;
    lt_map           channel_idx;    /* name -> lt_ini_channel* */
    lt_ini_table   **tables;    size_t n_tables;
    lt_map           table_idx;      /* name AND map_name -> lt_ini_table* */
    lt_ini_curve   **curves;    size_t n_curves;
    lt_map           curve_idx;
    lt_ini_gauge   **gauges;    size_t n_gauges;
    lt_map           gauge_idx;
    lt_ini_menu     *menus;     size_t n_menus;           /* in file order */
    lt_ini_dialog  **dialogs;   size_t n_dialogs;
    lt_map           dialog_idx;
    lt_ini_indicator_panel **ind_panels; size_t n_ind_panels;
    lt_ini_readout_panel   **readout_panels; size_t n_readout_panels;
    lt_ini_help    **helps;     size_t n_helps;
    lt_map           help_idx;
    lt_ini_setting_group **setting_groups; size_t n_setting_groups;
    lt_ini_cmd     **commands;  size_t n_commands;        /* [ControllerCommands] */
    lt_map           command_idx;
    lt_ini_dl_entry *dl_entries; size_t n_dl_entries;     /* [Datalog] in order */
    lt_ini_dl_view  *dl_views;   size_t n_dl_views;
    lt_ini_logger   *loggers;    size_t n_loggers;
    lt_ini_port_editor *port_editors; size_t n_port_editors;
    lt_ini_key_action  *key_actions;  size_t n_key_actions;
    lt_ini_ref_table  **ref_tables; size_t n_ref_tables;
    lt_ini_frontpage *frontpage;                          /* NULL if none */
    lt_ini_analyze   ve_analyze, wue_analyze, gamma_e;

    /* [Defaults]/[ConstantsExtensions]/[PcVariables] side tables */
    lt_map pc_variables;      /* name -> (void*)(size_t)(byte value + 1) */
    lt_map default_values;    /* name -> double* */
    lt_map maximum_elements;  /* name -> (void*)(size_t)count */
    char **maintain_constants; size_t n_maintain_constants;   /* "name" ; expr in maintain_exprs */
    char **maintain_exprs;
    char **requires_power_cycle; size_t n_requires_power_cycle;

    /* preprocessor memory */
    lt_map active_symbols, tested_symbols;

    void *priv;               /* parser-private (arena etc.) */
} lt_ini;

/* ---- loading ---- */
typedef struct lt_ini_options {
    const char *const *symbols;  /* preprocessor seed symbols (#set), e.g. "CELSIUS", "LAMBDA" */
    size_t n_symbols;
    /* symbols the caller decides (project settings): "#set"/"#unset" lines in the file that
     * name one of them, or one of the seed symbols, are ignored, so the file's own defaults
     * cannot override the project's choice. NULL = only the seed symbols are protected. */
    const char *const *managed;
    size_t n_managed;
} lt_ini_options;
void     lt_ini_options_default(lt_ini_options *o);   /* empty seed (NULL opts == this) */

/* Parse a file (resolves #include relative to it). NULL + lt_err on I/O or
 * include errors only (never for malformed lines). */
lt_ini *lt_ini_load_file(const char *path, const lt_ini_options *opts);
/* Parse text; #include relative to base_dir (NULL = cwd). */
lt_ini *lt_ini_load_string(const char *text, size_t len, const char *base_dir,
                           const lt_ini_options *opts);
void    lt_ini_free(lt_ini *ini);
/* Bytes -> UTF-8 text per spec 01 1.2 (BOM strip, strict UTF-8 else cp1252). malloc'd. */
char   *lt_ini_decode(const lt_u8 *bytes, size_t n, size_t *out_len);

/* ---- lookups (return NULL if absent) ---- */
const lt_ini_const   *lt_ini_find_const(const lt_ini *ini, const char *name);
const lt_ini_channel *lt_ini_find_channel(const lt_ini *ini, const char *name);
const lt_ini_table   *lt_ini_find_table(const lt_ini *ini, const char *name_or_map);
const lt_ini_curve   *lt_ini_find_curve(const lt_ini *ini, const char *name);
const lt_ini_gauge   *lt_ini_find_gauge(const lt_ini *ini, const char *name);
const lt_ini_dialog  *lt_ini_find_dialog(const lt_ini *ini, const char *name);
const lt_ini_help    *lt_ini_find_help(const lt_ini *ini, const char *name);
const lt_ini_cmd     *lt_ini_find_command(const lt_ini *ini, const char *name);
const lt_ini_ref_table *lt_ini_find_ref_table(const lt_ini *ini, const char *name);
const lt_ini_indicator_panel *lt_ini_find_ind_panel(const lt_ini *ini, const char *name);
const lt_ini_readout_panel   *lt_ini_find_readout_panel(const lt_ini *ini, const char *name);
const lt_ini_port_editor     *lt_ini_find_port_editor(const lt_ini *ini, const char *name);   /* NULL = first */
/* constant listed under [ConstantsExtensions] requiresPowerCycle */
lt_bool lt_ini_needs_power_cycle(const lt_ini *ini, const char *name);
/* [SettingGroups]: a group without options is an on/off flag (its name is the symbol); else
 * each option value is a symbol and "DEFAULT" means "none of them". */
lt_bool lt_ini_setting_is_flag(const lt_ini_setting_group *g);
/* the option of g active in this parse (index), or -1 for DEFAULT / none */
int     lt_ini_setting_active(const lt_ini *ini, const lt_ini_setting_group *g);
/* Built-in std_* dialog synthesised from constants (std_injection, std_ms3Rtc);
 * owned by ini; NULL if the name is not synthesisable. */
const lt_ini_dialog  *lt_ini_std_panel(lt_ini *ini, const char *name);

lt_bool lt_ini_symbol_active(const lt_ini *ini, const char *sym);   /* #set at end of parse */
lt_bool lt_ini_symbol_tested(const lt_ini *ini, const char *sym);
/* pc variable byte value for $var substitution (-1 if not a pc variable) */
int     lt_ini_pcvar_byte(const lt_ini *ini, const char *name);
lt_bool lt_ini_default_value(const lt_ini *ini, const char *name, double *out);
size_t  lt_ini_total_memory(const lt_ini *ini);              /* sum of page sizes */
lt_ecu_type lt_ecu_detect(const char *signature, const char *filename_hint);
const char *lt_ecu_type_name(lt_ecu_type t);                 /* "Speeduino","rusEFI","FOME","epicEFI","MegaSquirt 2/3","Unknown" */
lt_bool lt_ecu_supports_console(lt_ecu_type t);              /* rusEFI/FOME/epicEFI */
lt_bool lt_ini_supports_console(const lt_ini *ini);          /* type supports it AND has commands */
/* Infer table roles from analyze sections (done at load; callable again). */
void    lt_ini_infer_table_roles(lt_ini *ini);

/* ====================================================================== */
/* Value codec against page images                                         */
/* ====================================================================== */
/* All functions take the constant and a pagesrc. `idx` is the element index
 * (0 for scalars and bits; row-major for 2-D arrays). Display values use
 * scale/translate; "raw" does not. Bits constants: value = field value (the
 * option index); add c->bit_disp for human numbering. */
lt_endian lt_ini_const_endian(const lt_ini *ini, const lt_ini_const *c);
int lt_ini_const_get_raw(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                         size_t idx, double *raw);
int lt_ini_const_set_raw(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                         size_t idx, double raw);
int lt_ini_const_get(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                     size_t idx, double *display);
int lt_ini_const_set(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                     size_t idx, double display);
/* whole array -> doubles (display units); returns element count or <0. out may
 * be NULL to query the count. Writes only the bytes that change. */
int lt_ini_const_get_array(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                           double *out, size_t cap);
int lt_ini_const_set_array(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                           const double *v, size_t n);
/* STRING constants (NUL padded in the page) */
int lt_ini_const_get_string(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                            char *buf, size_t cap);
int lt_ini_const_set_string(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                            const char *s);
/* By name, element 0 (scalar/bits): LT_ENOENT if unknown constant. Falls back
 * to [Defaults] when the page is not loaded. */
int lt_ini_get(const lt_ini *ini, const lt_pagesrc *src, const char *name, double *display);
int lt_ini_set(const lt_ini *ini, lt_pagesrc *src, const char *name, double display);

/* Re-evaluate {expr} scale/translate/min/max of all constants against `env`
 * (tune values + computed channels). Returns number of constants changed. */
int     lt_ini_resolve_dynamic(lt_ini *ini, const lt_expr_env *env);
/* True if changing constant `name` can change another constant's scale (so the
 * GUI must refresh displayed values), spec 01 section 10. */
lt_bool lt_ini_feeds_dynamic_scale(const lt_ini *ini, const char *name);

/* Clamp / heal helper: write default into size-count constants whose stored
 * byte is out of range (spec 08 3). Returns number patched. */
int lt_ini_patch_invalid_sizes(const lt_ini *ini, lt_pagesrc *src);

/* ====================================================================== */
/* Realtime evaluator: raw ochBlock -> named values (incl. computed)       */
/* ====================================================================== */
/* Holds one double per output channel (indexed by lt_ini_channel.index). Call
 * lt_ini_rt_update() with each realtime frame (status byte already stripped,
 * length = proto.och_block_size). Computed channels are evaluated up to 3
 * passes in name order against: this frame's channels, then `parent` (typically
 * the tune constants, see lt_varenv in tune.h). Non-finite results are stored
 * as 0 for display. Single-threaded; not shared. */
typedef struct lt_ini_rt lt_ini_rt;
lt_ini_rt *lt_ini_rt_new(const lt_ini *ini);
void       lt_ini_rt_free(lt_ini_rt *rt);
void       lt_ini_rt_set_parent(lt_ini_rt *rt, const lt_expr_env *parent);  /* not owned */
/* 0 ok; LT_ERANGE if frame shorter than any raw channel needs (the channels that
 * fit are still decoded). */
int        lt_ini_rt_update(lt_ini_rt *rt, const lt_u8 *frame, size_t len);
/* Set/override one channel directly (demo mode, replay of logged values). */
void       lt_ini_rt_set(lt_ini_rt *rt, size_t channel_index, double v);
lt_bool    lt_ini_rt_get(const lt_ini_rt *rt, const char *name, double *out);
/* Virtual channels: lt_ini_rt_get asks fn for names that are not INI output channels (the app
 * installs its live math channels here, so gauges, the status bar, dashboards and the recorder
 * resolve them like real channels). Computed INI channels do not see them. fn NULL = none. */
typedef lt_bool (*lt_ini_rt_virtual_fn)(void *ud, const char *name, double *out);
void       lt_ini_rt_set_virtual(lt_ini_rt *rt, lt_ini_rt_virtual_fn fn, void *ud);
double     lt_ini_rt_value(const lt_ini_rt *rt, size_t channel_index);   /* 0 if never set */
lt_bool    lt_ini_rt_valid(const lt_ini_rt *rt, size_t channel_index);   /* decoded in last update */
size_t     lt_ini_rt_count(const lt_ini_rt *rt);
/* expr env resolving channel names (then parent). Valid while rt lives. */
const lt_expr_env *lt_ini_rt_env(lt_ini_rt *rt);
unsigned long lt_ini_realtime_size(const lt_ini *ini);   /* och_block_size or max channel end */

/* ====================================================================== */
/* .inc lookup tables (spec 01 section 11)                                 */
/* ====================================================================== */
typedef struct lt_inc_table {
    char *name;
    lt_bool direct;                 /* DB/DW list indexed by round(key) */
    double *keys, *vals; size_t n;  /* keys unused when direct */
} lt_inc_table;
lt_inc_table *lt_inc_parse(const char *text, const char *name);
void          lt_inc_free(lt_inc_table *t);
lt_bool       lt_inc_lookup(const lt_inc_table *t, double key, double *out);

typedef struct lt_inc_cache {
    char **search_paths; size_t n_paths;
    lt_map tables;                  /* filename -> lt_inc_table* */
} lt_inc_cache;
void          lt_inc_cache_init(lt_inc_cache *c);
void          lt_inc_cache_free(lt_inc_cache *c);
void          lt_inc_cache_add_path(lt_inc_cache *c, const char *dir);   /* deduped */
const lt_inc_table *lt_inc_cache_get(lt_inc_cache *c, const char *filename);  /* tries name, name.inc */

#endif /* LT_INI_H */
