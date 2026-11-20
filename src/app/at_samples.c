/* at_samples.c - "Every sample, and whether it counted": the per-sample verdict pane of an
 * AutoTune log replay (G-55).  A timeline strip shaded by the verdict that dominates each pixel
 * column (yellow ticks: counted samples of the grid's cursor cell), a legend that doubles as a
 * reason filter, and a scrollable table of the samples (time, operating point, AFR, cell,
 * verdict) filtered by verdict and optionally by the cursor cell.  The data and the filters are
 * autotune/at_view.c; this file only draws.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "at_ui.h"
#include "view_kit.h"

/* verdict colours (accepted is the only bright one), as in the original's verdict strip */
static lt_color sv_color(const char *reason)
{
    static const char *const names[10] = {
        "rpm/load not steady", "overrun fuel cut", "clt below min_clt", "rpm out of range",
        "afr at sensor rail", "tps_rate above max_tps_rate", "accel enrichment active",
        "load below min_y_axis", "load above max_y_axis", "custom_filter"
    };
    static const lt_u32 cols[10] = {
        0x8A6FD0UL, 0x4A6B8AUL, 0xC98A3AUL, 0x7A7A7AUL, 0xC04A6AUL, 0xB5643CUL, 0x9A7B4FUL,
        0x6B7F99UL, 0x5E7FB0UL, 0x4F8F8FUL
    };
    size_t i;
    lt_u32 c = 0x555555UL;
    if (reason == NULL) { c = 0x3DDC84UL; }
    else {
        for (i = 0; i < LT_COUNTOF(names); i++) {
            if (strcmp(reason, names[i]) == 0) { c = cols[i]; break; }
        }
    }
    return LT_RGB((c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu);
}

static const char *sv_reason(const lt_at_samples *sv, int r)
{
    return r >= 0 && (size_t)r < sv->vd.nreason ? sv->vd.reason[r] : NULL;
}

void lt_at_samples_reset(lt_at_samples *sv)
{
    size_t i;
    sv->fsel = 0;
    sv->sel = -1;
    sv->valid = 0;
    sv->nidx = 0;
    lt_strlcpy(sv->labels[0], "All samples", sizeof(sv->labels[0]));
    lt_snprintf(sv->labels[1], sizeof(sv->labels[1]), "Counted (%lu)", sv->vd.counted);
    lt_snprintf(sv->labels[2], sizeof(sv->labels[2]), "Dropped, any reason (%lu)",
                (unsigned long)(sv->vd.n - (size_t)sv->vd.counted));
    for (i = 0; i < sv->vd.nreason; i++) {
        lt_snprintf(sv->labels[i + 3], sizeof(sv->labels[i + 3]), "%s (%lu)", sv->vd.reason[i], sv->vd.count[i]);
    }
    for (i = 0; i < LT_COUNTOF(sv->lbl); i++) { sv->lbl[i] = sv->labels[i]; }
    if (sv->cap < sv->vd.n + 1) {
        lt_free(sv->idx);
        sv->cap = sv->vd.n + 1;
        sv->idx = (size_t *)lt_calloc(sv->cap, sizeof(size_t));
    }
}

void lt_at_samples_free(lt_at_samples *sv)
{
    lt_atx_verdicts_free(&sv->vd);
    lt_free(sv->idx);
    sv->idx = NULL;
    sv->cap = sv->nidx = 0;
    sv->valid = 0;
}

static void sv_filter(lt_at_samples *sv, int cx, int cy)
{
    lt_atx_vfilter f;
    f.kind = sv->fsel <= 0 ? LT_ATX_VF_ALL : (sv->fsel == 1 ? LT_ATX_VF_COUNTED :
             (sv->fsel == 2 ? LT_ATX_VF_DROPPED : LT_ATX_VF_REASON));
    f.reason = sv->fsel - 3;
    f.cx = sv->cell_only ? cx : -1;
    f.cy = sv->cell_only ? cy : -1;
    if (sv->cell_only && (cx < 0 || cy < 0)) { f.cx = f.cy = -1; }
    sv->nidx = lt_atx_verdicts_select(&sv->vd, &f, sv->idx, sv->cap);
    if (sv->nidx > sv->cap) { sv->nidx = sv->cap; }
    sv->fcx = cx; sv->fcy = cy; sv->fsel_was = sv->fsel; sv->cell_was = sv->cell_only;
    sv->valid = 1;
    sv->sel = -1;
}

/* entry of the filtered list closest to log row `row` (the list is in log order) */
static int sv_nearest(const lt_at_samples *sv, size_t row)
{
    size_t lo = 0, hi = sv->nidx;
    if (sv->nidx == 0) { return -1; }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (sv->idx[mid] < row) { lo = mid + 1; } else { hi = mid; }
    }
    if (lo >= sv->nidx) { return (int)sv->nidx - 1; }
    if (lo > 0 && row - sv->idx[lo - 1] < sv->idx[lo] - row) { return (int)lo - 1; }
    return (int)lo;
}

static void sv_timeline(lt_app *app, lt_at_samples *sv, lt_rect r, int cx, int cy)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    size_t n = sv->vd.n, from, to, i;
    int x, run_x = 0, run_v = -3, hit, tick = lt_ui_cell_mode(ui) ? 0 : lt_ui_px(ui, 4);
    lt_color tickc = LT_RGB(0xFF, 0xD5, 0x4A);
    if (r.w <= 0 || n == 0) { return; }
    lt_ui_draw_fill(ui, r, th->table_bg);
    for (x = 0; x <= r.w; x++) {
        int dom = -3, in_cell = 0;
        if (x < r.w) {
            from = (size_t)((double)x * (double)n / (double)r.w);
            to = (size_t)((double)(x + 1) * (double)n / (double)r.w);
            if (to <= from) { to = from + 1; }
            dom = lt_atx_verdict_dominant(&sv->vd, from, to);
            for (i = from; i < to && i < n && cx >= 0; i++) {
                const lt_atx_vrow *vr = &sv->vd.rows[i];
                if (vr->reason < 0 && (int)vr->cx == cx && (int)vr->cy == cy) { in_cell = 1; break; }
            }
            if (in_cell && tick > 0) {
                lt_ui_draw_fill(ui, lt_rect_make(r.x + x, r.y + r.h - tick, 1, tick), tickc);
            }
        }
        if (dom != run_v || x == r.w) {
            if (run_v >= -1 && x > run_x) {
                lt_rect seg = lt_rect_make(r.x + run_x, r.y, x - run_x, r.h - tick);
                lt_ui_draw_fill(ui, seg, sv_color(sv_reason(sv, run_v)));
            }
            run_v = dom; run_x = x;
        }
    }
    if (sv->sel >= 0 && (size_t)sv->sel < sv->nidx) {
        int sx = (int)((double)sv->idx[sv->sel] * (double)r.w / (double)n);
        lt_ui_draw_fill(ui, lt_rect_make(r.x + sx - (tick > 0 ? 1 : 0), r.y, tick > 0 ? 2 : 1, r.h), th->text);
    }
    hit = lt_ui_hit(ui, "##svtl", r);
    if (hit & (LT_UI_HIT_PRESS | LT_UI_HIT_DOWN)) {
        lt_point m = lt_ui_mouse(ui);
        int k = sv_nearest(sv, lt_atx_verdict_row_at(n, r.w, m.x - r.x));
        if (k >= 0 && k != sv->sel) { sv->sel = k; sv->ensure = 1; sv->jumped = 1; }
    }
    if (hit & LT_UI_HIT_HOVER) {
        lt_point m = lt_ui_mouse(ui);
        size_t row = lt_atx_verdict_row_at(n, r.w, m.x - r.x);
        const char *why = sv_reason(sv, sv->vd.rows[row].reason);
        lt_snprintf(sv->hover, sizeof(sv->hover), "sample %lu at %.2f s: %s", (unsigned long)row + 1ul,
                    sv->vd.rows[row].t, why != NULL ? why : "counted");
    }
}

/* legend chips; a click filters to that verdict (again: back to all) */
static void sv_legend(lt_app *app, lt_at_samples *sv, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int x = r.x, k, cell = lt_ui_cell_mode(ui), sq = cell ? 8 : lt_ui_px(ui, 9);
    char b[96];
    lt_ui_clip_push(ui, r);
    for (k = -1; k < (int)sv->vd.nreason; k++) {
        int want = k < 0 ? 1 : k + 3, w, hit, on = sv->fsel == want;
        lt_rect cr;
        unsigned long cnt = k < 0 ? sv->vd.counted : sv->vd.count[k];
        lt_snprintf(b, sizeof(b), "%s (%lu)", k < 0 ? "counted" : sv->vd.reason[k], cnt);
        w = lt_ui_text_width(ui, b) + sq + (cell ? 24 : lt_ui_px(ui, 20));
        cr = lt_rect_make(x, r.y, w, r.h);
        lt_ui_push_id_int(ui, k + 2);
        hit = lt_ui_hit(ui, "##svchip", cr);
        lt_ui_pop_id(ui);
        if (on || (hit & LT_UI_HIT_HOVER)) { lt_ui_draw_box(ui, cr, on ? th->selected : th->hover, 0, lt_ui_px(ui, 4)); }
        lt_ui_draw_fill(ui, lt_rect_make(x + (cell ? 0 : lt_ui_px(ui, 6)), r.y + (r.h - sq) / 2, sq, cell ? 16 : sq),
                        sv_color(sv_reason(sv, k)));
        lt_ui_draw_text_in(ui, lt_rect_make(x + sq + (cell ? 8 : lt_ui_px(ui, 11)), r.y, w, r.h), b,
                           on ? th->text : th->text2, LT_UI_ALIGN_LEFT);
        if (hit & LT_UI_HIT_PRESS) { sv->fsel = on ? 0 : want; }
        x += w + (cell ? 8 : lt_ui_px(ui, 4));
        if (x >= r.x + r.w) { break; }
    }
    lt_ui_clip_pop(ui);
}

static void sv_cell_text(char *b, size_t cap, const lt_atx_vrow *vr, const double *xb, size_t nx,
                         const double *yb, size_t ny)
{
    if (xb != NULL && yb != NULL && vr->cx < nx && vr->cy < ny) {
        lt_snprintf(b, cap, "%.0f x %.0f", xb[vr->cx], yb[vr->cy]);
    } else {
        lt_snprintf(b, cap, "%u,%u", vr->cx, vr->cy);
    }
}

void lt_at_samples_draw(lt_app *app, lt_at_samples *sv, const double *xb, size_t nx, const double *yb,
                        size_t ny, int cur_cx, int cur_cy, int *go_cx, int *go_cy)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui), w1 = -1, w[4], rh, i, fsel, cw[7], ncol = 7, k;
    static const char *const heads[7] = { "#", "Time s", "RPM", "Load", "AFR", "Cell", "Verdict" };
    static const int widths[7] = { 64, 72, 70, 64, 60, 110, 0 };
    lt_rect hr, lr;
    char b[160];
    *go_cx = *go_cy = -1;
    if (sv->vd.n == 0) { return; }
    if (!sv->valid || sv->fsel != sv->fsel_was || sv->cell_only != sv->cell_was ||
        (sv->cell_only && (sv->fcx != cur_cx || sv->fcy != cur_cy))) {
        sv_filter(sv, cur_cx, cur_cy);
    }
    /* title, filter, cell filter, count */
    w[0] = 270; w[1] = 260; w[2] = 190; w[3] = -1;
    lt_ui_layout_row(ui, 4, w, cell ? 0 : 30);
    if (cell) { lt_ui_label_ex(ui, "Every sample, and whether it counted", th->text, LT_UI_ALIGN_LEFT); }
    else {
        lt_rect tr = lt_ui_layout_next(ui);
        lt_vk_text(ui, tr, (int)LT_FONT_SANS_SEMIBOLD, 13.0, "Every sample, and whether it counted", th->text, 0);
    }
    fsel = sv->fsel;
    if (lt_ui_combo(ui, "svfilter", &fsel, sv->lbl, (int)sv->vd.nreason + 3)) { sv->fsel = fsel; }
    (void)lt_ui_checkbox(ui, "Only the cursor cell", &sv->cell_only);
    lt_snprintf(b, sizeof(b), "%lu of %lu samples shown", (unsigned long)sv->nidx, (unsigned long)sv->vd.n);
    lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_RIGHT);
    /* timeline */
    lt_ui_layout_row(ui, 1, &w1, cell ? 16 : 26);
    sv->hover[0] = '\0';
    sv->jumped = 0;
    sv_timeline(app, sv, lt_ui_layout_next(ui), cur_cx, cur_cy);
    if (sv->jumped && !sv->cell_only && sv->sel >= 0 && (size_t)sv->sel < sv->nidx) {
        /* the grid follows the sample found on the strip */
        *go_cx = (int)sv->vd.rows[sv->idx[sv->sel]].cx;
        *go_cy = (int)sv->vd.rows[sv->idx[sv->sel]].cy;
    }
    /* legend / hover line */
    lt_ui_layout_row(ui, 1, &w1, cell ? 16 : 24);
    sv_legend(app, sv, lt_ui_layout_next(ui));
    lt_ui_layout_row(ui, 1, &w1, cell ? 16 : 18);
    if (sv->hover[0] != '\0') { lt_ui_label_ex(ui, sv->hover, th->text2, LT_UI_ALIGN_LEFT); }
    else {
        lt_ui_label_ex(ui, cur_cx >= 0 ? "Yellow ticks: counted samples of the cursor cell. Click the strip to find a sample."
                                       : "Click the strip to find a sample.", th->muted, LT_UI_ALIGN_LEFT);
    }
    /* column header */
    lt_ui_layout_row(ui, 1, &w1, cell ? 16 : 22);
    hr = lt_ui_layout_next(ui);
    lt_ui_draw_fill(ui, hr, th->table_header);
    for (k = 0, i = hr.x; k < ncol; k++) {
        cw[k] = widths[k] > 0 ? (cell ? widths[k] / 8 * 8 : lt_ui_px(ui, widths[k])) : LT_MAX(hr.x + hr.w - i, 40);
        lt_ui_draw_text_in(ui, lt_rect_make(i + (cell ? 0 : lt_ui_px(ui, 6)), hr.y, cw[k] - (cell ? 8 : lt_ui_px(ui, 12)), hr.h),
                           heads[k], th->text2, k == 6 || k == 5 ? LT_UI_ALIGN_LEFT : LT_UI_ALIGN_RIGHT);
        i += cw[k];
    }
    /* rows */
    rh = cell ? 16 : lt_ui_px(ui, 22);
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "atsvlist")) {
        lt_rect view = lt_ui_scroll_view(ui), body;
        int first, last, hit;
        lt_ui_scroll_set_content(ui, 0, (int)LT_MIN(sv->nidx, (size_t)2000000L) * rh);
        body = lt_ui_layout_body(ui);
        if (sv->ensure && sv->sel >= 0) {
            lt_ui_scroll_ensure_visible(ui, lt_rect_make(body.x, body.y + sv->sel * rh, body.w, rh));
            sv->ensure = 0;
        }
        lt_ui_draw_fill(ui, view, th->table_bg);
        first = (view.y - body.y) / rh;
        last = (view.y + view.h - body.y) / rh;
        if (first < 0) { first = 0; }
        if (last >= (int)sv->nidx) { last = (int)sv->nidx - 1; }
        lr = lt_rect_make(view.x, view.y, view.w - (cell ? 8 : th->scrollbar_w), view.h);
        hit = lt_ui_hit(ui, "##svrows", lr);
        if (hit & LT_UI_HIT_PRESS) {
            lt_point m = lt_ui_mouse(ui);
            int row = (m.y - body.y) / rh;
            if (row >= 0 && row < (int)sv->nidx) {
                const lt_atx_vrow *vr = &sv->vd.rows[sv->idx[row]];
                sv->sel = row;
                *go_cx = (int)vr->cx; *go_cy = (int)vr->cy;
            }
        }
        for (i = first; i <= last; i++) {
            const lt_atx_vrow *vr = &sv->vd.rows[sv->idx[i]];
            const char *why = sv_reason(sv, vr->reason);
            lt_rect rr = lt_rect_make(body.x, body.y + i * rh, body.w, rh);
            int x = rr.x;
            lt_color fg = vr->reason < 0 ? th->text : th->text2;
            if (i == sv->sel) { lt_ui_draw_fill(ui, rr, th->table_selected); }
            else if (i % 2 == 1 && !cell) { lt_ui_draw_fill(ui, rr, th->table_cell); }
            for (k = 0; k < ncol; k++) {
                lt_rect c = lt_rect_make(x + (cell ? 0 : lt_ui_px(ui, 6)), rr.y, cw[k] - (cell ? 8 : lt_ui_px(ui, 12)), rh);
                switch (k) {
                case 0: lt_snprintf(b, sizeof(b), "%lu", (unsigned long)sv->idx[i] + 1ul); break;
                case 1: lt_snprintf(b, sizeof(b), "%.2f", vr->t); break;
                case 2: lt_snprintf(b, sizeof(b), "%.0f", vr->rpm); break;
                case 3: lt_snprintf(b, sizeof(b), "%.1f", vr->load); break;
                case 4: lt_snprintf(b, sizeof(b), "%.2f", vr->afr); break;
                case 5: sv_cell_text(b, sizeof(b), vr, xb, nx, yb, ny); break;
                default: lt_strlcpy(b, why != NULL ? why : "counted", sizeof(b)); break;
                }
                if (k == 6) {
                    int sq = cell ? 8 : lt_ui_px(ui, 8);
                    lt_ui_draw_fill(ui, lt_rect_make(c.x, rr.y + (rh - (cell ? 16 : sq)) / 2, sq, cell ? 16 : sq), sv_color(why));
                    c.x += sq + (cell ? 8 : lt_ui_px(ui, 7));
                    c.w -= sq + (cell ? 8 : lt_ui_px(ui, 7));
                }
                lt_ui_draw_text_in(ui, c, b, k == 6 ? (vr->reason < 0 ? th->success : fg) : fg,
                                   k >= 5 ? LT_UI_ALIGN_LEFT : LT_UI_ALIGN_RIGHT);
                x += cw[k];
            }
        }
        if (sv->nidx == 0) {
            lt_ui_draw_text_in(ui, lt_rect_inset(view, cell ? 8 : lt_ui_px(ui, 10), 0),
                               "No sample matches this filter.", th->muted, LT_UI_ALIGN_LEFT);
        }
        lt_ui_scroll_end(ui);
    }
}
