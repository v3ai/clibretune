/* feat_error.c - error details dialog (G-102): a failure shows the short message, the protocol
 * detail (code, CRC, page/offset) and a "Copy for Bug Report" button.  Wording follows the
 * original ErrorDetailsDialog.
 *   lt_feat_error_show(app, title, message, details)   open the dialog
 *   lt_feat_error_proto(app, title)                    same, details from lt_proto_errinfo
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../plat/plat.h"

#define DET_CAPACITY 1536

typedef struct err_state {
    char title[96], msg[512], det[DET_CAPACITY];
} err_state;

typedef struct err_dlg { char title[96], msg[512], det[DET_CAPACITY]; int copied; double t_copy; } err_dlg;

static err_state *es(lt_app *app)
{
    return (err_state *)lt_ext_state(app, "error_details", sizeof(err_state), NULL);
}

const char *lt_feat_error_code_name(int code)
{
    switch (code) {
    case LT_PE_SERIAL: return "serial port error";
    case LT_PE_TIMEOUT: return "timeout";
    case LT_PE_NOT_CONNECTED: return "not connected";
    case LT_PE_CONNECTION_FAILED: return "connection failed";
    case LT_PE_CANCELLED: return "cancelled";
    case LT_PE_ALREADY_CONNECTED: return "already connected";
    case LT_PE_INVALID_RESPONSE: return "invalid response";
    case LT_PE_CRC_MISMATCH: return "CRC mismatch";
    case LT_PE_SIGNATURE_MISMATCH: return "signature mismatch";
    case LT_PE_ECU_STATUS: return "ECU status error";
    case LT_PE_BUFFER_OVERFLOW: return "buffer overflow";
    case LT_PE_PROTOCOL: return "protocol error";
    case LT_PE_WRITE_VERIFY_FAILED: return "write verification failed";
    case LT_PE_PAGE_CRC_MISMATCH: return "page CRC mismatch";
    case LT_PE_VERIFY_UNAVAILABLE: return "verification unavailable";
    case LT_PE_PORT_NOT_FOUND: return "port not found";
    case LT_PE_IO: return "I/O error";
    default: return "none";
    }
}

/* multi-line protocol detail text of an error record */
void lt_feat_error_describe(const lt_proto_err *e, char *out, size_t cap)
{
    char tmp[96];
    lt_snprintf(out, cap, "Code: %s (%d)\nMessage: %s\n", lt_feat_error_code_name((int)e->code),
                (int)e->code, e->msg);
    if (e->code == LT_PE_CRC_MISMATCH || e->code == LT_PE_PAGE_CRC_MISMATCH) {
        lt_snprintf(tmp, sizeof(tmp), "CRC expected 0x%08lX, got 0x%08lX\n", (unsigned long)e->a,
                    (unsigned long)e->b);
        lt_strlcat(out, tmp, cap);
    } else if (e->code == LT_PE_WRITE_VERIFY_FAILED) {
        lt_snprintf(tmp, sizeof(tmp), "Page %d offset %d: expected %d, read back %d\n", e->page,
                    e->offset, e->expected, e->actual);
        lt_strlcat(out, tmp, cap);
    } else if (e->code == LT_PE_ECU_STATUS) {
        lt_snprintf(tmp, sizeof(tmp), "ECU status code: 0x%02lX\n", (unsigned long)e->a);
        lt_strlcat(out, tmp, cap);
    }
}

/* the text copied by "Copy for Bug Report" */
void lt_feat_error_report(const char *title, const char *msg, const char *det, char *out, size_t cap)
{
    char stamp[32];
    lt_plat_timestamp(stamp, sizeof(stamp));
    lt_snprintf(out, cap, "clibretune Error Report\n========================\nTitle: %s\n"
                "Message: %s\n%s%s========================\nDate: %s\nPlatform: %s\nVersion: %s\n",
                title, msg, det[0] != '\0' ? "\nDetails:\n" : "", det, stamp, lt_plat_os_name(),
                LT_APP_VERSION);
}

void lt_feat_error_show(lt_app *app, const char *title, const char *message, const char *details)
{
    err_state *s = es(app);
    lt_strlcpy(s->title, title != NULL ? title : "Error", sizeof(s->title));
    lt_strlcpy(s->msg, message != NULL ? message : "", sizeof(s->msg));
    lt_strlcpy(s->det, details != NULL ? details : "", sizeof(s->det));
    (void)lt_app_dialog_push(app, "error_details", "", s->title);
}

void lt_feat_error_proto(lt_app *app, const char *title)
{
    char det[DET_CAPACITY];
    const char *e = lt_err_get();
    det[0] = '\0';
    if (app->proto != NULL) {
        const lt_proto_err *pe = lt_proto_errinfo(app->proto);
        lt_feat_error_describe(pe, det, sizeof(det));
        if (e != NULL && e[0] != '\0' && strcmp(e, pe->msg) != 0) {
            lt_strlcat(det, "Detail: ", sizeof(det));
            lt_strlcat(det, e, sizeof(det));
            lt_strlcat(det, "\n", sizeof(det));
        }
        lt_feat_error_show(app, title, pe->msg[0] != '\0' ? pe->msg : "The ECU did not respond", det);
    } else {
        lt_feat_error_show(app, title, e != NULL && e[0] != '\0' ? e : "Unknown error", "");
    }
}

static void err_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    err_dlg *s = (err_dlg *)d->data;
    int open = d->open, w1 = -1, w2[3], fh = lt_ui_cell_mode(ui) ? 16 : 28;
    if (s == NULL) {
        err_state *st = es(app);
        s = (err_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        lt_strlcpy(s->title, st->title, sizeof(s->title));
        lt_strlcpy(s->msg, st->msg, sizeof(s->msg));
        lt_strlcpy(s->det, st->det, sizeof(s->det));
        if (s->msg[0] == '\0') {            /* LT_SHOT_OPEN preview */
            lt_strlcpy(s->title, "Connection Failed", sizeof(s->title));
            lt_strlcpy(s->msg, "The ECU did not answer within the timeout.", sizeof(s->msg));
            lt_strlcpy(s->det, "Code: timeout (2)\nMessage: no reply to the query command\n"
                       "Port: /dev/ttyUSB0 at 115200 baud\n", sizeof(s->det));
        }
    }
    if (lt_ui_modal_begin(ui, s->title, 560, 380, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->msg, t->error);
        if (s->det[0] != '\0') {
            const char *p = s->det;
            char line[200];
            lt_ui_layout_space(ui, 4);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Error Details", t->text, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, -(fh * 3 + 24));
            if (lt_ui_scroll_begin(ui, "errdet")) {
                while (*p != '\0') {
                    size_t k = 0;
                    while (*p != '\0' && *p != '\n' && k + 1 < sizeof(line)) { line[k++] = *p++; }
                    line[k] = '\0';
                    if (*p == '\n') { p++; }
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
                }
                lt_ui_scroll_end(ui);
            }
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "If this error persists, please file a bug report with the error "
                             "details above.", t->muted);
        w2[0] = -1; w2[1] = 190; w2[2] = 90;
        lt_ui_layout_row(ui, 3, w2, fh);
        lt_ui_label(ui, "");
        if (lt_ui_button(ui, s->copied ? "Copied!" : "Copy for Bug Report")) {
            char rep[DET_CAPACITY + 512];
            lt_feat_error_report(s->title, s->msg, s->det, rep, sizeof(rep));
            lt_ui_clipboard_set(ui, rep);
            s->copied = 1;
        }
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void err_close(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    lt_free(d->data);
    d->data = NULL;
}

void lt_feat_error_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "error_details"; dk.draw = err_draw; dk.close = err_close;
    lt_ext_add_dialog_kind(reg, &dk);
}
