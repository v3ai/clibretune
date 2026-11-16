/* dash_valid.c - dashboard validation (spec 05 section 3).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "dash_priv.h"

static void add_issue(lt_dash_report *r, lt_dash_issue_kind k, lt_bool err, const char *id,
                      const char *fmt, const char *a, const char *b)
{
    lt_dash_issue *is;
    r->issues = (lt_dash_issue *)lt_realloc(r->issues, (r->n + 1) * sizeof(lt_dash_issue));
    is = &r->issues[r->n++];
    memset(is, 0, sizeof(*is));
    is->kind = k;
    is->is_error = err;
    lt_strlcpy(is->gauge_id, id ? id : "", sizeof is->gauge_id);
    lt_snprintf(is->message, sizeof is->message, fmt, a ? a : "", b ? b : "");
}

static lt_bool has_channel(const char *const *ch, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (ch[i] && strcmp(ch[i], name) == 0) return LT_TRUE;
    return LT_FALSE;
}

static lt_bool has_image(const lt_dash_cluster *c, const char *fn)
{
    size_t i;
    for (i = 0; i < c->n_images; i++)
        if (c->images[i].file_name && strcmp(c->images[i].file_name, fn) == 0) return LT_TRUE;
    return LT_FALSE;
}

void lt_dash_validate(const lt_dash_file *d, const char *const *channels, size_t n_channels,
                      lt_dash_report *out)
{
    const lt_dash_cluster *c = &d->cluster;
    lt_map uniq;
    size_t i, j;
    const lt_dash_gauge **gs = NULL;
    size_t ng = 0;

    memset(out, 0, sizeof(*out));
    lt_map_init(&uniq, LT_FALSE);
    if (c->n_comps == 0) add_issue(out, LT_DI_EMPTY, LT_TRUE, "", "dashboard has no components", NULL, NULL);
    gs = (const lt_dash_gauge **)lt_calloc(c->n_comps + 1, sizeof(*gs));
    for (i = 0; i < c->n_images; i++)
        if (c->images[i].type == LT_RES_TTF) out->has_embedded_fonts = LT_TRUE;
    out->embedded_image_count = c->n_images;

    for (i = 0; i < c->n_comps; i++) {
        const lt_dash_gauge *g = c->comps[i].gauge;
        const lt_dash_indicator *in = c->comps[i].indicator;
        if (g) {
            gs[ng++] = g;
            out->gauge_count++;
            if (g->output_channel && g->output_channel[0]) {
                lt_map_put(&uniq, g->output_channel, (void *)&uniq);
                if (channels && !has_channel(channels, n_channels, g->output_channel))
                    add_issue(out, LT_DI_UNKNOWN_CHANNEL, LT_TRUE, g->id,
                              "unknown output channel '%s'", g->output_channel, NULL);
            }
            if (g->min >= g->max)
                add_issue(out, LT_DI_INVALID_RANGE, LT_TRUE, g->id, "min >= max", NULL, NULL);
            if (g->background_image_file_name && !has_image(c, g->background_image_file_name))
                add_issue(out, LT_DI_MISSING_IMAGE, LT_TRUE, g->id,
                          "background image '%s' is not embedded", g->background_image_file_name, NULL);
            if (g->needle_image_file_name && !has_image(c, g->needle_image_file_name))
                add_issue(out, LT_DI_MISSING_IMAGE, LT_TRUE, g->id,
                          "needle image '%s' is not embedded", g->needle_image_file_name, NULL);
            if (g->relative_width < 0.05 || g->relative_height < 0.05)
                add_issue(out, LT_DI_TINY_GAUGE, LT_FALSE, g->id, "gauge is very small", NULL, NULL);
            if (g->relative_x + g->relative_width > 1.0 || g->relative_y + g->relative_height > 1.0)
                add_issue(out, LT_DI_OUT_OF_BOUNDS, LT_FALSE, g->id,
                          "gauge extends beyond the dashboard", NULL, NULL);
        } else if (in) {
            out->indicator_count++;
            if (channels && in->output_channel && in->output_channel[0] &&
                !has_channel(channels, n_channels, in->output_channel))
                add_issue(out, LT_DI_UNKNOWN_CHANNEL, LT_TRUE, in->id,
                          "unknown indicator channel '%s'", in->output_channel, NULL);
        }
    }
    for (i = 0; i < ng; i++) {
        const lt_dash_gauge *a = gs[i];
        for (j = i + 1; j < ng; j++) {
            const lt_dash_gauge *b = gs[j];
            if (!(a->relative_x + a->relative_width < b->relative_x ||
                  b->relative_x + b->relative_width < a->relative_x ||
                  a->relative_y + a->relative_height < b->relative_y ||
                  b->relative_y + b->relative_height < a->relative_y))
                add_issue(out, LT_DI_OVERLAP, LT_FALSE, a->id, "gauge overlaps '%s'", b->id, NULL);
        }
    }
    if (c->n_comps > 50)
        add_issue(out, LT_DI_PERFORMANCE, LT_FALSE, "", "more than 50 components may be slow", NULL, NULL);
    out->unique_channels = uniq.len;
    out->is_valid = LT_TRUE;
    for (i = 0; i < out->n; i++)
        if (out->issues[i].is_error) out->is_valid = LT_FALSE;
    lt_map_free(&uniq);
    lt_free((void *)gs);
}

void lt_dash_report_free(lt_dash_report *r)
{
    lt_free(r->issues);
    memset(r, 0, sizeof(*r));
}
