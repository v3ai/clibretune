/* dash_edit.h - dashboard editing logic used by the designer: undo/redo history, component
 * clone / copy / paste, z-order, hide flag, alignment, embedded assets, INI range sync,
 * indicator grid layout. No UI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DASH_EDIT_H
#define LT_DASH_EDIT_H

#include "dash.h"

LT_BEGIN_DECLS

/* ---- geometry of a component (relative x, y, w, h in 0..1) ---- */
void lt_dash_comp_get_rect(const lt_dash_comp *c, double v[4]);
void lt_dash_comp_set_rect(lt_dash_comp *c, const double v[4]);
/* "Title (channel)" style label for layer lists; written to buf */
void lt_dash_comp_label(const lt_dash_comp *c, char *buf, size_t cap);

/* ---- undo / redo: a stack of serialized snapshots; entry [pos] is the current state ---- */
#define LT_DASH_HIST_MAX 100
typedef struct lt_dash_hist { char **s; int n, cap, pos; } lt_dash_hist;
void lt_dash_hist_init(lt_dash_hist *h);
void lt_dash_hist_free(lt_dash_hist *h);
/* record d as the new current state (drops the redo tail); returns 1 when it differed from the
 * current entry, 0 when nothing changed. The first push is the baseline. */
int  lt_dash_hist_push(lt_dash_hist *h, const lt_dash_file *d);
int  lt_dash_hist_can_undo(const lt_dash_hist *h);
int  lt_dash_hist_can_redo(const lt_dash_hist *h);
/* move back / forward; returns the restored file (caller owns) or NULL */
lt_dash_file *lt_dash_hist_undo(lt_dash_hist *h);
lt_dash_file *lt_dash_hist_redo(lt_dash_hist *h);

/* ---- components ---- */
/* deep copy of src->cluster.comps[idx] appended to dst (a fresh id), offset by dx, dy;
 * returns the new index or -1 */
int  lt_dash_comp_copy_to(lt_dash_file *dst, const lt_dash_file *src, size_t idx, double dx, double dy);
/* same file: duplicate with a small offset */
int  lt_dash_comp_clone(lt_dash_file *d, size_t idx);
/* clipboard text (a .dash with only the given components) and paste back; paste returns the
 * number of components added and the index of the first one */
char  *lt_dash_clip_copy(const lt_dash_file *d, const size_t *idx, size_t n);
size_t lt_dash_clip_paste(lt_dash_file *d, const char *text, double dx, double dy, size_t *first);
/* z-order: move the component to position `to` (later = on top); returns the new index */
size_t lt_dash_comp_move_z(lt_dash_file *d, size_t idx, size_t to);
lt_bool lt_dash_comp_hidden(const lt_dash_comp *c);
void    lt_dash_comp_set_hidden(lt_dash_comp *c, lt_bool hidden);   /* keeps the real condition */

/* ---- alignment of a selection (idx[n]) ---- */
typedef enum lt_align_op {
    LT_ALIGN_LEFT = 0, LT_ALIGN_HCENTER, LT_ALIGN_RIGHT, LT_ALIGN_TOP, LT_ALIGN_VCENTER,
    LT_ALIGN_BOTTOM, LT_ALIGN_DIST_H, LT_ALIGN_DIST_V, LT_ALIGN_SAME_W, LT_ALIGN_SAME_H
} lt_align_op;
void lt_dash_align(lt_dash_file *d, const size_t *idx, size_t n, lt_align_op op);
/* snap x to a grid of `cells` cells per unit (cells <= 0: no snapping) */
double lt_dash_snap(double x, int cells);

/* ---- embedded assets (cluster images) ---- */
size_t lt_dash_b64_encode(const void *data, size_t n, char *out, size_t cap);   /* out has room for 4*ceil(n/3)+1 */
/* adds (or replaces, same image id) an asset; type from the extension; returns its index */
int  lt_dash_asset_add(lt_dash_file *d, const char *name, const void *data, size_t len);
void lt_dash_asset_remove(lt_dash_file *d, size_t idx);
lt_bool lt_dash_asset_used(const lt_dash_file *d, size_t idx);   /* referenced by a component or the background */

/* ---- INI ---- */
/* adopt range, warning/critical levels, units and digits of the INI gauge with the same output
 * channel (or title); returns the number of gauges changed */
size_t lt_dash_sync_ranges(lt_dash_file *d, const lt_ini *ini, const lt_expr_env *env);
/* re-grid all indicators into `columns` columns (0 = keep the widest existing row) inside their
 * common bounding box; fill_empty stretches the last row to the full width */
void lt_dash_layout_indicators(lt_dash_file *d, int columns, lt_bool fill_empty);

LT_END_DECLS

#endif
