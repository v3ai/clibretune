/* dash_io.c - .dash / .gauge XML parse and write (spec 05 section 2).
 * The parser walks a lt_xml DOM; behaviour follows the event-based reader of the spec
 * (forgiving, unknown properties land in extras, colour element text overrides attrs).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "dash_priv.h"

/* ================================================================== */
/* property tables                                                      */
/* ================================================================== */

enum { K_STR, K_OSTR, K_ONULL, K_DBL, K_INT, K_BOOL, K_ODBL };

typedef struct dash_prop {
    const char *name;
    int kind;
    size_t off;      /* field offset */
    size_t off2;     /* has-flag offset (K_ODBL) */
    double dflt;     /* fallback on parse error (K_DBL/K_INT/K_BOOL) */
} dash_prop;

#define G(f) offsetof(lt_dash_gauge, f)
#define I(f) offsetof(lt_dash_indicator, f)

static const dash_prop gauge_props[] = {
    { "Id", K_STR, G(id), 0, 0 },
    { "Title", K_STR, G(title), 0, 0 },
    { "Units", K_STR, G(units), 0, 0 },
    { "OutputChannel", K_STR, G(output_channel), 0, 0 },
    { "Value", K_DBL, G(value), 0, 0.0 },
    { "Min", K_DBL, G(min), 0, 0.0 },
    { "Max", K_DBL, G(max), 0, 100.0 },
    { "MinVP", K_OSTR, G(min_vp), 0, 0 },
    { "MaxVP", K_OSTR, G(max_vp), 0, 0 },
    { "LowWarningVP", K_OSTR, G(low_warning_vp), 0, 0 },
    { "HighWarningVP", K_OSTR, G(high_warning_vp), 0, 0 },
    { "LowCriticalVP", K_OSTR, G(low_critical_vp), 0, 0 },
    { "HighCriticalVP", K_OSTR, G(high_critical_vp), 0, 0 },
    { "DefaultMin", K_ODBL, G(default_min), G(has_default_min), 0 },
    { "DefaultMax", K_ODBL, G(default_max), G(has_default_max), 0 },
    { "LowWarning", K_ODBL, G(low_warning), G(has_low_warning), 0 },
    { "HighWarning", K_ODBL, G(high_warning), G(has_high_warning), 0 },
    { "LowCritical", K_ODBL, G(low_critical), G(has_low_critical), 0 },
    { "HighCritical", K_ODBL, G(high_critical), G(has_high_critical), 0 },
    { "PegLimits", K_BOOL, G(peg_limits), 0, 1 },
    { "ValueDigits", K_INT, G(value_digits), 0, 0 },
    { "LabelDigits", K_INT, G(label_digits), 0, 0 },
    { "FontFamily", K_STR, G(font_family), 0, 0 },
    { "FontSizeAdjustment", K_INT, G(font_size_adjustment), 0, 0 },
    { "ItalicFont", K_BOOL, G(italic_font), 0, 0 },
    { "SweepAngle", K_INT, G(sweep_angle), 0, 270 },
    { "StartAngle", K_INT, G(start_angle), 0, 135 },
    { "FaceAngle", K_INT, G(face_angle), 0, 360 },
    { "SweepBeginDegree", K_INT, G(sweep_begin_degree), 0, 135 },
    { "CounterClockwise", K_BOOL, G(counter_clockwise), 0, 0 },
    { "MajorTicks", K_DBL, G(major_ticks), 0, -1.0 },
    { "MinorTicks", K_DBL, G(minor_ticks), 0, -1.0 },
    { "RelativeX", K_DBL, G(relative_x), 0, 0.0 },
    { "RelativeY", K_DBL, G(relative_y), 0, 0.0 },
    { "RelativeWidth", K_DBL, G(relative_width), 0, 0.25 },
    { "RelativeHeight", K_DBL, G(relative_height), 0, 0.25 },
    { "BorderWidth", K_INT, G(border_width), 0, 3 },
    { "ShortestSize", K_INT, G(shortest_size), 0, 50 },
    { "ShapeLockedToAspect", K_BOOL, G(shape_locked_to_aspect), 0, 0 },
    { "AntialiasingOn", K_BOOL, G(antialiasing_on), 0, 1 },
    { "BackgroundImageFileName", K_ONULL, G(background_image_file_name), 0, 0 },
    { "NeedleImageFileName", K_ONULL, G(needle_image_file_name), 0, 0 },
    { "ShowHistory", K_BOOL, G(peak_hold), 0, 0 },
    { "HistoryValue", K_DBL, G(history_value), 0, 0.0 },
    { "HistoryDelay", K_INT, G(history_delay), 0, 15000 },
    { "NeedleSmoothing", K_INT, G(needle_smoothing), 0, 1 },
    { "ShortClickAction", K_ONULL, G(short_click_action), 0, 0 },
    { "LongClickAction", K_ONULL, G(long_click_action), 0, 0 },
    { "DisplayValueAt180", K_BOOL, G(display_value_at_180), 0, 0 },
    { "GaugeStyle", K_STR, G(gauge_style), 0, 0 },
    { "RunDemo", K_BOOL, G(run_demo), 0, 0 },
    { "EnabledCondition", K_OSTR, G(enabled_condition), 0, 0 },
    { "Hysteresis", K_ODBL, G(hysteresis), G(has_hysteresis), 0 },
    { "NeedleLength", K_ODBL, G(needle_length), G(has_needle_length), 0 },
    { "NeedlePivotOffsetX", K_ODBL, G(needle_pivot_offset_x), G(has_pivot), 0 },
    { "NeedlePivotOffsetY", K_ODBL, G(needle_pivot_offset_y), G(has_pivot), 0 },
    { "NeedleImageOffsetX", K_ODBL, G(needle_image_offset_x), G(has_needle_image_offset), 0 },
    { "NeedleImageOffsetY", K_ODBL, G(needle_image_offset_y), G(has_needle_image_offset), 0 }
};

static const dash_prop ind_props[] = {
    { "Id", K_STR, I(id), 0, 0 },
    { "Value", K_DBL, I(value), 0, 0.0 },
    { "OutputChannel", K_STR, I(output_channel), 0, 0 },
    { "OnText", K_STR, I(on_text), 0, 0 },
    { "OffText", K_STR, I(off_text), 0, 0 },
    { "OnImageFileName", K_ONULL, I(on_image_file_name), 0, 0 },
    { "OffImageFileName", K_ONULL, I(off_image_file_name), 0, 0 },
    { "RelativeX", K_DBL, I(relative_x), 0, 0.0 },
    { "RelativeY", K_DBL, I(relative_y), 0, 0.0 },
    { "RelativeWidth", K_DBL, I(relative_width), 0, 0.1 },
    { "RelativeHeight", K_DBL, I(relative_height), 0, 0.05 },
    { "FontFamily", K_STR, I(font_family), 0, 0 },
    { "ItalicFont", K_BOOL, I(italic_font), 0, 0 },
    { "AntialiasingOn", K_BOOL, I(antialiasing_on), 0, 1 },
    { "ShortClickAction", K_ONULL, I(short_click_action), 0, 0 },
    { "LongClickAction", K_ONULL, I(long_click_action), 0, 0 },
    { "EcuConfigurationName", K_OSTR, I(ecu_configuration_name), 0, 0 },
    { "RunDemo", K_BOOL, I(run_demo), 0, 0 },
    { "EnabledCondition", K_OSTR, I(enabled_condition), 0, 0 }
};

static lt_bool parse_bool(const char *s, int *out)
{
    if (strcmp(s, "true") == 0) {
        *out = 1;
        return LT_TRUE;
    }
    if (strcmp(s, "false") == 0) {
        *out = 0;
        return LT_TRUE;
    }
    return LT_FALSE;
}

/* typed field access through memcpy (no misaligned/aliasing casts) */
static void put_str_at(char *base, size_t off, const char *v)
{
    char *cur;
    memcpy(&cur, base + off, sizeof cur);
    dash_set_str(&cur, v);
    memcpy(base + off, &cur, sizeof cur);
}

static void put_dbl_at(char *base, size_t off, double v)
{
    memcpy(base + off, &v, sizeof v);
}

static void put_int_at(char *base, size_t off, int v)
{
    memcpy(base + off, &v, sizeof v);
}

/* returns 1 when `name` is in the table (value applied) */
static int apply_prop(void *obj, const dash_prop *tbl, size_t n, const char *name, const char *val)
{
    size_t i;
    char *base = (char *)obj;
    for (i = 0; i < n; i++) {
        const dash_prop *p = &tbl[i];
        if (strcmp(p->name, name) != 0) continue;
        switch (p->kind) {
        case K_STR:
            put_str_at(base, p->off, val);
            break;
        case K_OSTR:
            put_str_at(base, p->off, val[0] ? val : NULL);
            break;
        case K_ONULL:
            put_str_at(base, p->off, (val[0] && strcmp(val, "null") != 0) ? val : NULL);
            break;
        case K_DBL: {
            double d;
            put_dbl_at(base, p->off, lt_parse_double(val, &d) ? d : p->dflt);
            break;
        }
        case K_INT: {
            long l;
            put_int_at(base, p->off, dash_parse_i32(val, &l) ? (int)l : (int)p->dflt);
            break;
        }
        case K_BOOL: {
            int b;
            put_int_at(base, p->off, parse_bool(val, &b) ? b : (int)p->dflt);
            break;
        }
        case K_ODBL: {
            double d;
            if (lt_parse_double(val, &d)) {
                put_dbl_at(base, p->off, d);
                put_int_at(base, p->off2, 1);
            } else {
                put_int_at(base, p->off2, 0);
            }
            break;
        }
        default:
            break;
        }
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* colours                                                              */
/* ================================================================== */

static const char *const gauge_color_names[] = {
    "BackColor", "FontColor", "TrimColor", "WarnColor", "CriticalColor", "NeedleColor"
};
static const char *const ind_color_names[] = {
    "OnTextColor", "OffTextColor", "OnBackgroundColor", "OffBackgroundColor"
};

static int name_index(const char *const *tbl, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(tbl[i], name) == 0) return (int)i;
    return -1;
}

static int attr_u8(const lt_xml_node *n, const char *name, int absent, int fail)
{
    const char *a = lt_xml_attr_get(n, name);
    long l;
    if (a == NULL) return absent;
    return (dash_parse_i32(a, &l) && l >= 0 && l <= 255) ? (int)l : fail;
}

static lt_color color_from_node(const lt_xml_node *n, const char *text)
{
    int a = attr_u8(n, "alpha", 0, 255);
    int r = attr_u8(n, "red", 0, 0);
    int g = attr_u8(n, "green", 0, 0);
    int b = attr_u8(n, "blue", 0, 0);
    long l;
    lt_color c = LT_RGBA(r, g, b, a);
    if (text[0] && dash_parse_i32(text, &l)) c = lt_color_from_argb((lt_s32)l);
    return c;
}

/* ================================================================== */
/* component parsing                                                    */
/* ================================================================== */

static char *trimmed_text(const lt_xml_node *n)
{
    char *t = lt_strdup(n->text ? n->text : "");
    lt_str_trim(t);
    return t;
}

static void parse_gauge_comp(lt_dash_gauge *g, const lt_xml_node *comp)
{
    size_t i;
    for (i = 0; i < comp->n_kids; i++) {
        const lt_xml_node *k = comp->kids[i];
        char *val = trimmed_text(k);
        int ci = name_index(gauge_color_names, LT_COUNTOF(gauge_color_names), k->name);
        if (ci >= 0) {
            lt_color c = color_from_node(k, val);
            switch (ci) {
            case 0: g->back_color = c; break;
            case 1: g->font_color = c; break;
            case 2: g->trim_color = c; break;
            case 3: g->warn_color = c; break;
            case 4: g->critical_color = c; break;
            default: g->needle_color = c; break;
            }
        } else if (name_index(ind_color_names, LT_COUNTOF(ind_color_names), k->name) >= 0) {
            /* indicator colour on a gauge: dropped */
        } else if (strcmp(k->name, "GaugePainter") == 0) {
            g->painter = lt_painter_from_ts(val);
        } else if (!apply_prop(g, gauge_props, LT_COUNTOF(gauge_props), k->name, val)) {
            lt_dash_extras_set(&g->extras, k->name, val);
        }
        lt_free(val);
    }
}

static void parse_ind_comp(lt_dash_indicator *in, const lt_xml_node *comp)
{
    size_t i;
    for (i = 0; i < comp->n_kids; i++) {
        const lt_xml_node *k = comp->kids[i];
        char *val = trimmed_text(k);
        int ci = name_index(ind_color_names, LT_COUNTOF(ind_color_names), k->name);
        if (ci >= 0) {
            lt_color c = color_from_node(k, val);
            switch (ci) {
            case 0: in->on_text_color = c; break;
            case 1: in->off_text_color = c; break;
            case 2: in->on_background_color = c; break;
            default: in->off_background_color = c; break;
            }
        } else if (name_index(gauge_color_names, LT_COUNTOF(gauge_color_names), k->name) >= 0) {
            /* gauge colour on an indicator: dropped */
        } else if (strcmp(k->name, "Painter") == 0) {
            in->painter = lt_indpainter_from_ts(val);
        } else if (!apply_prop(in, ind_props, LT_COUNTOF(ind_props), k->name, val)) {
            lt_dash_extras_set(&in->extras, k->name, val);
        }
        lt_free(val);
    }
}

static lt_bool type_is_indicator(const lt_xml_node *comp)
{
    const char *t = lt_xml_attr_get(comp, "type");
    char *l, *p;
    lt_bool r;
    if (t == NULL) return LT_FALSE;
    l = lt_strdup(t);
    for (p = l; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    r = strstr(l, "indicator") != NULL;
    lt_free(l);
    return r;
}

/* ================================================================== */
/* cluster / bibliography / images                                      */
/* ================================================================== */

static const char *const cluster_known[] = {
    "antiAliasing", "forceAspect", "forceAspectWidth", "forceAspectHeight",
    "clusterBackgroundColor", "backgroundDitherColor", "clusterBackgroundImageFileName",
    "clusterBackgroundImageStyle", "clusterLayout", "enabledCondition"
};

static lt_color attr_color(const char *v, lt_color dflt)
{
    long l;
    return dash_parse_i32(v, &l) ? lt_color_from_argb((lt_s32)l) : dflt;
}

static void parse_cluster_attrs(lt_dash_cluster *c, const lt_xml_node *n)
{
    size_t i;
    dash_cluster_reset_attrs(c);
    for (i = 0; i < n->n_attrs; i++) {
        const char *nm = n->attrs[i].name, *v = n->attrs[i].value;
        int b;
        double d;
        if (strcmp(nm, "antiAliasing") == 0) {
            c->anti_aliasing = parse_bool(v, &b) ? b : LT_TRUE;
        } else if (strcmp(nm, "forceAspect") == 0) {
            c->force_aspect = parse_bool(v, &b) ? b : LT_FALSE;
        } else if (strcmp(nm, "forceAspectWidth") == 0) {
            c->force_aspect_width = lt_parse_double(v, &d) ? d : 0.0;
        } else if (strcmp(nm, "forceAspectHeight") == 0) {
            c->force_aspect_height = lt_parse_double(v, &d) ? d : 0.0;
        } else if (strcmp(nm, "clusterBackgroundColor") == 0) {
            c->background_color = attr_color(v, LT_RGB(0, 0, 0));
        } else if (strcmp(nm, "backgroundDitherColor") == 0) {
            if (v[0]) {
                long l;
                if (dash_parse_i32(v, &l)) {
                    c->has_dither = LT_TRUE;
                    c->dither_color = lt_color_from_argb((lt_s32)l);
                }
            }
        } else if (strcmp(nm, "clusterBackgroundImageFileName") == 0) {
            if (v[0]) dash_set_str(&c->background_image_file_name, v);
        } else if (strcmp(nm, "clusterBackgroundImageStyle") == 0) {
            c->background_image_style = strcmp(v, "Stretch") == 0 ? LT_BG_STRETCH
                : strcmp(v, "Center") == 0 ? LT_BG_CENTER
                : strcmp(v, "Fit") == 0 ? LT_BG_FIT : LT_BG_TILE;
        } else if (strcmp(nm, "clusterLayout") == 0) {
            if (v[0]) dash_set_str(&c->layout, v);
        } else if (strcmp(nm, "enabledCondition") == 0) {
            if (v[0]) dash_set_str(&c->enabled_condition, v);
        } else {
            size_t k;
            lt_bool known = LT_FALSE;
            for (k = 0; k < LT_COUNTOF(cluster_known); k++)
                if (lt_stricmp(cluster_known[k], nm) == 0) known = LT_TRUE;
            if (!known) lt_dash_extras_set(&c->extras, nm, v);
        }
    }
}

static void parse_image(lt_dash_image *im, const lt_xml_node *n)
{
    const char *a, *ty;
    char *t;
    memset(im, 0, sizeof(*im));
    a = lt_xml_attr_get(n, "fileName");
    im->file_name = lt_strdup(a ? a : "");
    a = lt_xml_attr_get(n, "imageId");
    im->image_id = lt_strdup(a ? a : "");
    ty = lt_xml_attr_get(n, "type");
    im->type = LT_RES_PNG;
    if (ty && lt_stricmp(ty, "gif") == 0) im->type = LT_RES_GIF;
    else if (ty && lt_stricmp(ty, "ttf") == 0) im->type = LT_RES_TTF;
    t = trimmed_text(n);
    im->data_base64 = t;
}

static void push_image(lt_dash_image **arr, size_t *n, const lt_xml_node *node)
{
    *arr = (lt_dash_image *)lt_realloc(*arr, (*n + 1) * sizeof(lt_dash_image));
    parse_image(&(*arr)[*n], node);
    (*n)++;
}

static void parse_bib(const lt_xml_node *n, char **au, char **co, char **wd)
{
    const char *a;
    a = lt_xml_attr_get(n, "author");
    dash_set_str(au, a ? a : "");
    a = lt_xml_attr_get(n, "company");
    dash_set_str(co, a ? a : "");
    a = lt_xml_attr_get(n, "writeDate");
    dash_set_str(wd, a ? a : "");
}

static void parse_version(const lt_xml_node *n, char **fmt, char **sig, const char *dflt)
{
    const char *a = lt_xml_attr_get(n, "fileFormat");
    dash_set_str(fmt, a ? a : dflt);
    if (sig) {
        a = lt_xml_attr_get(n, "firmwareSignature");
        dash_set_str(sig, a);
    }
}

/* ================================================================== */
/* .dash parse                                                          */
/* ================================================================== */

static void dash_walk(lt_dash_file *d, const lt_xml_node *n)
{
    size_t i;
    if (strcmp(n->name, "gaugeCluster") == 0) {
        parse_cluster_attrs(&d->cluster, n);
    } else if (strcmp(n->name, "dashComp") == 0) {
        if (type_is_indicator(n)) {
            lt_dash_indicator *in = lt_dash_add_indicator(d);
            parse_ind_comp(in, n);
        } else {
            lt_dash_gauge *g = lt_dash_add_gauge(d);
            parse_gauge_comp(g, n);
        }
        return;
    } else if (strcmp(n->name, "imageFile") == 0) {
        push_image(&d->cluster.images, &d->cluster.n_images, n);
        return;
    } else if (strcmp(n->name, "bibliography") == 0) {
        parse_bib(n, &d->author, &d->company, &d->write_date);
    } else if (strcmp(n->name, "versionInfo") == 0) {
        parse_version(n, &d->file_format, &d->firmware_signature, "3.0");
    }
    for (i = 0; i < n->n_kids; i++) dash_walk(d, n->kids[i]);
}

lt_dash_file *lt_dash_parse(const char *xml, size_t len, char *err, size_t errlen)
{
    lt_xml_node *root;
    lt_dash_file *d;
    char e[160];

    e[0] = '\0';
    root = lt_xml_parse(xml, len, 0, e, sizeof e);
    if (root == NULL) {
        if (err && errlen) lt_strlcpy(err, e[0] ? e : "invalid XML", errlen);
        lt_err_set("dash: %s", e[0] ? e : "invalid XML");
        return NULL;
    }
    d = lt_dash_new();
    dash_set_str(&d->author, "");
    dash_set_str(&d->company, "");
    dash_set_str(&d->write_date, "");
    dash_walk(d, root);
    lt_xml_free(root);
    if (err && errlen) err[0] = '\0';
    return d;
}

lt_dash_file *lt_dash_load(const char *path)
{
    size_t n = 0;
    char *txt = lt_file_read(path, &n);
    lt_dash_file *d;
    char e[160];
    if (txt == NULL) return NULL;
    d = lt_dash_parse(txt, n, e, sizeof e);
    lt_free(txt);
    return d;
}

int lt_dash_save(const lt_dash_file *d, const char *path)
{
    lt_buf b;
    int rc;
    lt_buf_init(&b);
    lt_dash_write(d, &b);
    rc = lt_file_write(path, b.data, b.len);
    lt_buf_free(&b);
    return rc;
}

/* ================================================================== */
/* .gauge parse                                                         */
/* ================================================================== */

static lt_bool gauge_walk(lt_gauge_file *o, const lt_xml_node *n, int depth, lt_bool *got)
{
    size_t i;
    if (strcmp(n->name, "gauge") == 0) depth++;
    if (strcmp(n->name, "dashComp") == 0) {
        if (depth > 0 && !*got) {
            parse_gauge_comp(&o->gauge, n);
            *got = LT_TRUE;
        }
        return LT_TRUE;
    }
    if (strcmp(n->name, "imageFile") == 0) {
        push_image(&o->images, &o->n_images, n);
        return LT_TRUE;
    }
    if (strcmp(n->name, "bibliography") == 0) parse_bib(n, &o->author, &o->company, &o->write_date);
    else if (strcmp(n->name, "versionInfo") == 0) parse_version(n, &o->file_format, NULL, "1.0");
    for (i = 0; i < n->n_kids; i++) gauge_walk(o, n->kids[i], depth, got);
    return LT_TRUE;
}

int lt_gaugefile_parse(const char *xml, size_t len, lt_gauge_file *out, char *err, size_t errlen)
{
    lt_xml_node *root;
    char e[160];
    lt_bool got = LT_FALSE;

    memset(out, 0, sizeof(*out));
    lt_dash_gauge_init(&out->gauge);
    out->author = lt_strdup("");
    out->company = lt_strdup("");
    out->write_date = lt_strdup("");
    out->file_format = lt_strdup("1.0");
    e[0] = '\0';
    root = lt_xml_parse(xml, len, 0, e, sizeof e);
    if (root == NULL) {
        if (err && errlen) lt_strlcpy(err, e[0] ? e : "invalid XML", errlen);
        lt_err_set("gauge: %s", e[0] ? e : "invalid XML");
        lt_gaugefile_free(out);
        return LT_EDATA;
    }
    gauge_walk(out, root, 0, &got);
    lt_xml_free(root);
    if (err && errlen) err[0] = '\0';
    return 0;
}

void lt_gaugefile_free(lt_gauge_file *g)
{
    size_t i;
    lt_free(g->author);
    lt_free(g->company);
    lt_free(g->write_date);
    lt_free(g->file_format);
    for (i = 0; i < g->n_images; i++) dash_image_free(&g->images[i]);
    lt_free(g->images);
    lt_dash_gauge_free(&g->gauge);
    memset(g, 0, sizeof(*g));
}

/* ================================================================== */
/* writer                                                               */
/* ================================================================== */

static void w_str(lt_xmlw *w, const char *name, const char *v)
{
    if (v == NULL) v = "";
    lt_xmlw_open(w, name);
    lt_xmlw_attr(w, "type", "String");
    if (v[0] == '\0') lt_xmlw_raw(w, "");
    else if (strpbrk(v, "<>&")) lt_xmlw_cdata(w, v);
    else lt_xmlw_text(w, v);
    lt_xmlw_close(w);
}

static void w_ostr(lt_xmlw *w, const char *name, const char *v)
{
    w_str(w, name, v ? v : "null");
}

static void w_dbl(lt_xmlw *w, const char *name, double v)
{
    char b[400];
    dash_fmt_f64(b, sizeof b, v);
    lt_xmlw_open(w, name);
    lt_xmlw_attr(w, "type", "double");
    lt_xmlw_raw(w, b);
    lt_xmlw_close(w);
}

static void w_int(lt_xmlw *w, const char *name, int v)
{
    char b[32];
    lt_snprintf(b, sizeof b, "%d", v);
    lt_xmlw_open(w, name);
    lt_xmlw_attr(w, "type", "int");
    lt_xmlw_raw(w, b);
    lt_xmlw_close(w);
}

static void w_bool(lt_xmlw *w, const char *name, int v)
{
    lt_xmlw_open(w, name);
    lt_xmlw_attr(w, "type", "boolean");
    lt_xmlw_raw(w, v ? "true" : "false");
    lt_xmlw_close(w);
}

static void w_color(lt_xmlw *w, const char *name, lt_color c)
{
    char b[32];
    lt_xmlw_open(w, name);
    lt_xmlw_attr(w, "type", "java.awt.Color");
    lt_xmlw_attr_int(w, "alpha", LT_COLOR_A(c));
    lt_xmlw_attr_int(w, "red", LT_COLOR_R(c));
    lt_xmlw_attr_int(w, "green", LT_COLOR_G(c));
    lt_xmlw_attr_int(w, "blue", LT_COLOR_B(c));
    lt_snprintf(b, sizeof b, "%ld", (long)lt_color_to_argb(c));
    lt_xmlw_raw(w, b);
    lt_xmlw_close(w);
}

static void w_opt_dbl(lt_xmlw *w, const char *name, int has, double v)
{
    if (has) w_dbl(w, name, v);
}

static void w_opt_vp(lt_xmlw *w, const char *name, const char *v)
{
    if (v && v[0]) w_str(w, name, v);
}

static void w_extras(lt_xmlw *w, const lt_dash_extras *e)
{
    size_t i;
    for (i = 0; i < e->n; i++) w_str(w, e->a[i].k, e->a[i].v);
}

static void w_gauge(lt_xmlw *w, const lt_dash_gauge *g)
{
    lt_xmlw_open(w, "dashComp");
    lt_xmlw_attr(w, "type", "Gauge");
    w_str(w, "Id", g->id);
    w_str(w, "Title", g->title);
    w_str(w, "Units", g->units);
    w_dbl(w, "Value", g->value);
    w_str(w, "OutputChannel", g->output_channel);
    w_dbl(w, "Min", g->min);
    w_dbl(w, "Max", g->max);
    w_opt_vp(w, "MinVP", g->min_vp);
    w_opt_vp(w, "MaxVP", g->max_vp);
    w_opt_dbl(w, "DefaultMin", g->has_default_min, g->default_min);
    w_opt_dbl(w, "DefaultMax", g->has_default_max, g->default_max);
    w_bool(w, "PegLimits", g->peg_limits);
    w_opt_dbl(w, "LowWarning", g->has_low_warning, g->low_warning);
    w_opt_dbl(w, "HighWarning", g->has_high_warning, g->high_warning);
    w_opt_dbl(w, "LowCritical", g->has_low_critical, g->low_critical);
    w_opt_dbl(w, "HighCritical", g->has_high_critical, g->high_critical);
    w_opt_vp(w, "LowWarningVP", g->low_warning_vp);
    w_opt_vp(w, "HighWarningVP", g->high_warning_vp);
    w_opt_vp(w, "LowCriticalVP", g->low_critical_vp);
    w_opt_vp(w, "HighCriticalVP", g->high_critical_vp);
    w_int(w, "ValueDigits", g->value_digits);
    w_int(w, "LabelDigits", g->label_digits);
    w_str(w, "FontFamily", g->font_family);
    w_int(w, "FontSizeAdjustment", g->font_size_adjustment);
    w_bool(w, "ItalicFont", g->italic_font);
    w_int(w, "SweepAngle", g->sweep_angle);
    w_int(w, "StartAngle", g->start_angle);
    w_int(w, "FaceAngle", g->face_angle);
    w_int(w, "SweepBeginDegree", g->sweep_begin_degree);
    w_bool(w, "CounterClockwise", g->counter_clockwise);
    w_dbl(w, "MajorTicks", g->major_ticks);
    w_dbl(w, "MinorTicks", g->minor_ticks);
    w_color(w, "BackColor", g->back_color);
    w_color(w, "FontColor", g->font_color);
    w_color(w, "TrimColor", g->trim_color);
    w_color(w, "WarnColor", g->warn_color);
    w_color(w, "CriticalColor", g->critical_color);
    w_color(w, "NeedleColor", g->needle_color);
    w_dbl(w, "RelativeX", g->relative_x);
    w_dbl(w, "RelativeY", g->relative_y);
    w_dbl(w, "RelativeWidth", g->relative_width);
    w_dbl(w, "RelativeHeight", g->relative_height);
    w_int(w, "BorderWidth", g->border_width);
    w_int(w, "ShortestSize", g->shortest_size);
    w_bool(w, "ShapeLockedToAspect", g->shape_locked_to_aspect);
    w_bool(w, "AntialiasingOn", g->antialiasing_on);
    w_ostr(w, "BackgroundImageFileName", g->background_image_file_name);
    w_ostr(w, "NeedleImageFileName", g->needle_image_file_name);
    w_opt_dbl(w, "NeedleLength", g->has_needle_length, g->needle_length);
    if (g->has_pivot) {
        w_dbl(w, "NeedlePivotOffsetX", g->needle_pivot_offset_x);
        w_dbl(w, "NeedlePivotOffsetY", g->needle_pivot_offset_y);
    }
    if (g->has_needle_image_offset) {
        w_dbl(w, "NeedleImageOffsetX", g->needle_image_offset_x);
        w_dbl(w, "NeedleImageOffsetY", g->needle_image_offset_y);
    }
    w_bool(w, "ShowHistory", g->peak_hold);
    w_dbl(w, "HistoryValue", g->history_value);
    w_int(w, "HistoryDelay", g->history_delay);
    w_int(w, "NeedleSmoothing", g->needle_smoothing);
    w_ostr(w, "ShortClickAction", g->short_click_action);
    w_ostr(w, "LongClickAction", g->long_click_action);
    w_bool(w, "DisplayValueAt180", g->display_value_at_180);
    w_str(w, "GaugeStyle", g->gauge_style);
    w_str(w, "GaugePainter", lt_painter_to_ts(g->painter));
    w_bool(w, "RunDemo", g->run_demo);
    if (g->enabled_condition && g->enabled_condition[0])
        w_str(w, "EnabledCondition", g->enabled_condition);
    w_opt_dbl(w, "Hysteresis", g->has_hysteresis, g->hysteresis);
    w_extras(w, &g->extras);
    lt_xmlw_close(w);
}

static void w_indicator(lt_xmlw *w, const lt_dash_indicator *in)
{
    lt_xmlw_open(w, "dashComp");
    lt_xmlw_attr(w, "type", "Indicator");
    w_str(w, "Id", in->id);
    w_dbl(w, "Value", in->value);
    w_str(w, "OutputChannel", in->output_channel);
    w_str(w, "OnText", in->on_text);
    w_str(w, "OffText", in->off_text);
    w_ostr(w, "OnImageFileName", in->on_image_file_name);
    w_ostr(w, "OffImageFileName", in->off_image_file_name);
    w_color(w, "OnTextColor", in->on_text_color);
    w_color(w, "OffTextColor", in->off_text_color);
    w_color(w, "OnBackgroundColor", in->on_background_color);
    w_color(w, "OffBackgroundColor", in->off_background_color);
    w_dbl(w, "RelativeX", in->relative_x);
    w_dbl(w, "RelativeY", in->relative_y);
    w_dbl(w, "RelativeWidth", in->relative_width);
    w_dbl(w, "RelativeHeight", in->relative_height);
    w_str(w, "FontFamily", in->font_family);
    w_bool(w, "ItalicFont", in->italic_font);
    w_bool(w, "AntialiasingOn", in->antialiasing_on);
    w_ostr(w, "ShortClickAction", in->short_click_action);
    w_ostr(w, "LongClickAction", in->long_click_action);
    w_opt_vp(w, "EcuConfigurationName", in->ecu_configuration_name);
    w_str(w, "Painter", lt_indpainter_to_ts(in->painter));
    w_bool(w, "RunDemo", in->run_demo);
    if (in->enabled_condition && in->enabled_condition[0])
        w_str(w, "EnabledCondition", in->enabled_condition);
    w_extras(w, &in->extras);
    lt_xmlw_close(w);
}

static void w_images(lt_xmlw *w, const lt_dash_image *im, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        lt_xmlw_open(w, "imageFile");
        lt_xmlw_attr(w, "fileName", im[i].file_name ? im[i].file_name : "");
        lt_xmlw_attr(w, "imageId", im[i].image_id ? im[i].image_id : "");
        lt_xmlw_attr(w, "type", im[i].type == LT_RES_GIF ? "gif" : im[i].type == LT_RES_TTF ? "ttf" : "png");
        lt_xmlw_raw(w, im[i].data_base64 ? im[i].data_base64 : "");
        lt_xmlw_close(w);
    }
}

static void w_bib(lt_xmlw *w, const char *au, const char *co, const char *wd)
{
    lt_xmlw_open(w, "bibliography");
    lt_xmlw_attr(w, "author", au ? au : "");
    lt_xmlw_attr(w, "company", co ? co : "");
    lt_xmlw_attr(w, "writeDate", wd ? wd : "");
    lt_xmlw_close(w);
}

static void w_cluster_attrs(lt_xmlw *w, const lt_dash_cluster *c)
{
    char b[400];
    size_t i;
    lt_xmlw_attr(w, "antiAliasing", c->anti_aliasing ? "true" : "false");
    if (c->force_aspect) {
        lt_xmlw_attr(w, "forceAspect", "true");
        if (c->force_aspect_width > 0.0) {
            dash_fmt_f64(b, sizeof b, c->force_aspect_width);
            lt_xmlw_attr(w, "forceAspectWidth", b);
        }
        if (c->force_aspect_height > 0.0) {
            dash_fmt_f64(b, sizeof b, c->force_aspect_height);
            lt_xmlw_attr(w, "forceAspectHeight", b);
        }
    }
    lt_xmlw_attr_int(w, "clusterBackgroundColor", (long)lt_color_to_argb(c->background_color));
    if (c->has_dither)
        lt_xmlw_attr_int(w, "backgroundDitherColor", (long)lt_color_to_argb(c->dither_color));
    if (c->background_image_file_name && c->background_image_file_name[0])
        lt_xmlw_attr(w, "clusterBackgroundImageFileName", c->background_image_file_name);
    lt_xmlw_attr(w, "clusterBackgroundImageStyle",
                 c->background_image_style == LT_BG_STRETCH ? "Stretch"
                 : c->background_image_style == LT_BG_CENTER ? "Center"
                 : c->background_image_style == LT_BG_FIT ? "Fit" : "Tile");
    if (c->layout && c->layout[0]) lt_xmlw_attr(w, "clusterLayout", c->layout);
    if (c->enabled_condition && c->enabled_condition[0])
        lt_xmlw_attr(w, "enabledCondition", c->enabled_condition);
    for (i = 0; i < c->extras.n; i++) lt_xmlw_attr(w, c->extras.a[i].k, c->extras.a[i].v);
}

void lt_dash_write(const lt_dash_file *d, lt_buf *out)
{
    lt_xmlw w;
    size_t i;
    const lt_dash_cluster *c = &d->cluster;

    lt_xmlw_init(&w, out, 4, 0);
    lt_xmlw_decl(&w, "UTF-8", "no");
    lt_xmlw_open(&w, "dsh");
    lt_xmlw_attr(&w, "xmlns", "http://www.EFIAnalytics.com/:dsh");
    w_bib(&w, d->author, d->company, d->write_date);
    lt_xmlw_open(&w, "versionInfo");
    lt_xmlw_attr(&w, "fileFormat", d->file_format ? d->file_format : "3.0");
    if (d->firmware_signature) lt_xmlw_attr(&w, "firmwareSignature", d->firmware_signature);
    lt_xmlw_close(&w);
    lt_xmlw_open(&w, "gaugeCluster");
    w_cluster_attrs(&w, c);
    w_images(&w, c->images, c->n_images);
    for (i = 0; i < c->n_comps; i++) {
        if (c->comps[i].gauge) w_gauge(&w, c->comps[i].gauge);
        else if (c->comps[i].indicator) w_indicator(&w, c->comps[i].indicator);
    }
    lt_xmlw_finish(&w);
    lt_xmlw_free(&w);
}

void lt_gaugefile_write(const lt_gauge_file *g, lt_buf *out)
{
    lt_xmlw w;

    lt_xmlw_init(&w, out, 4, 0);
    lt_xmlw_decl(&w, "UTF-8", "no");
    lt_xmlw_open(&w, "gauge");
    lt_xmlw_attr(&w, "xmlns", "http://www.EFIAnalytics.com/:gauge");
    w_bib(&w, g->author, g->company, g->write_date);
    lt_xmlw_open(&w, "versionInfo");
    lt_xmlw_attr(&w, "fileFormat", g->file_format ? g->file_format : "1.0");
    lt_xmlw_close(&w);
    w_images(&w, g->images, g->n_images);
    lt_xmlw_open(&w, "gauge");
    w_gauge(&w, &g->gauge);
    lt_xmlw_finish(&w);
    lt_xmlw_free(&w);
}
