/* feat_firmware.c - Tools > Firmware Update: flash an ECU over USB (ST DfuSe ROM bootloader
 * or OpenBLT), with board presets (miata-ecus, rusEFI family, generic STM32), the reboot
 * into the bootloader through the INI controller commands, and the miata-ecus on-board
 * wideband flasher (board command / rusEFI console wbo_uart_flash + wbo_uart_status).
 * The flashing job (src/fw) is stepped from the per-frame tick in a small time budget, so
 * the UI stays responsive. With --demo (or LT_FW_SIM=1) simulated bootloaders from
 * src/fw/fw_sim.c are listed so the whole flow runs without hardware.
 * Screenshot helpers (headless only): LT_SHOT_FW_PRESET=n, LT_SHOT_FW_METHOD=n,
 * LT_SHOT_FW_START=1 (start without the confirmation), LT_SHOT_FW_WBO=1.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "feat.h"
#include "dialogs.h"
#include "sync_logic.h"
#include "../fw/fw.h"

#define CH 8
#define FD_FW_IMAGE (LT_APP_FD_USER0 + 90)
#define FW_LOG 150
#define FW_LOG_LEN 200
#define FW_MAX_DEV 8
#define FW_STEP_BUDGET_MS 12.0
#define FW_WAIT_DEV_MS 25000.0
#define WB_POLL_MS 1000.0
#define WB_LIMIT_MS 60000.0

enum { WB_IDLE = 0, WB_RUN, WB_DONE, WB_FAIL };

typedef struct fw_state {
    int init;
    int preset, method;
    int verify, leave, keep;
    char path[512], loaded[512];
    int loaded_preset;
    lt_fw_image img;
    int img_ok, demo_img;
    char img_err[240], img_sum[200];
    lt_fw_facts facts;
    /* bootloader devices */
    int sim_mode, sim_gone;
    lt_fw_found dev[FW_MAX_DEV];
    int n_dev, scan_rc;
    double next_scan;
    /* reboot into the bootloader, then wait for it to enumerate */
    int waiting, reboot_confirm, reboot_method;
    double wait_until;
    /* the flashing job */
    lt_fw_job *job;
    lt_fw_io io;
    lt_fw_usb *usb;
    lt_fw_sim *sim;
    int running, finished, final_phase, usb_err, confirm, was_demo;
    lt_fw_progress prog;
    char result[240];
    char (*log)[FW_LOG_LEN];
    int n_log;
    /* on-board wideband */
    int wb, wb_confirm, wb_polls;
    double wb_next, wb_t0;
    char wb_msg[200];
    int shot_start, shot_wb;
} fw_state;

static void job_close(fw_state *st)
{
    lt_fw_job_free(st->job);
    st->job = NULL;
    lt_fw_usb_close(st->usb);
    st->usb = NULL;
    lt_fw_sim_free(st->sim);
    st->sim = NULL;
    st->running = 0;
}

static void fw_free(lt_app *app, void *p)
{
    fw_state *st = (fw_state *)p;
    LT_UNUSED(app);
    job_close(st);
    lt_fw_image_free(&st->img);
    lt_free(st->log);
    st->log = NULL;
}

static fw_state *fw_get(lt_app *app)
{
    return (fw_state *)lt_ext_state(app, "firmware", sizeof(fw_state), fw_free);
}

static int is_online(const lt_app *app)
{
    return (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) && app->proto != NULL;
}

static int env_int(const char *name, int def)
{
    char *e = lt_plat_getenv(name);
    int v = def;
    if (e != NULL) { if (e[0] != '\0') { v = atoi(e); } lt_free(e); }
    return v;
}

/* ------------------------------------------------------------------ log */
static void add_log(fw_state *st, int level, const char *msg)
{
    static const char tags[4] = { 'i', 'o', 'w', 'e' };
    if (st->log == NULL) {
        st->log = (char (*)[FW_LOG_LEN])lt_calloc(FW_LOG, FW_LOG_LEN);
        if (st->log == NULL) { return; }
    }
    if (st->n_log >= FW_LOG) {
        memmove(st->log[0], st->log[1], (size_t)(FW_LOG - 1) * FW_LOG_LEN);
        st->n_log = FW_LOG - 1;
    }
    st->log[st->n_log][0] = tags[level < 0 || level > 3 ? 0 : level];
    lt_strlcpy(st->log[st->n_log] + 1, msg, FW_LOG_LEN - 1);
    st->n_log++;
}

static void logf_(fw_state *st, int level, const char *fmt, ...)
{
    char b[FW_LOG_LEN];
    va_list ap;
    va_start(ap, fmt);
    (void)lt_vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    add_log(st, level, b);
}

static void job_log(void *user, int level, const char *msg)
{
    add_log((fw_state *)user, level, msg);
}

static double sim_clock(void *u)
{
    LT_UNUSED(u);
    return lt_plat_now_ms();
}

/* ------------------------------------------------------------------ image */
static void load_image(fw_state *st)
{
    const lt_fw_preset *p = lt_fw_preset_get(st->preset);
    lt_fw_image_free(&st->img);
    st->img_ok = 0;
    st->demo_img = 0;
    st->img_err[0] = '\0';
    st->img_sum[0] = '\0';
    memset(&st->facts, 0, sizeof(st->facts));
    lt_strlcpy(st->loaded, st->path, sizeof(st->loaded));
    st->loaded_preset = st->preset;
    if (st->path[0] == '\0') {
        if (!st->sim_mode) { return; }
        lt_fw_sim_demo_image(&st->img, 1, 400);     /* demo: a synthetic merged OpenBLT + rusEFI image */
        st->demo_img = 1;
    } else if (lt_fw_image_load(&st->img, st->path, p->bin_base, st->img_err, sizeof(st->img_err)) != 0) {
        return;
    }
    st->img_ok = st->img.n_segs > 0;
    lt_fw_image_summary(&st->img, st->img_sum, sizeof(st->img_sum));
    lt_fw_image_facts(&st->img, &st->facts);
}

/* ------------------------------------------------------------------ devices */
static void scan(const lt_app *app, fw_state *st)
{
    int n = lt_fw_scan(st->dev, FW_MAX_DEV), k;
    st->scan_rc = n < 0 ? n : 0;
    st->n_dev = n < 0 ? 0 : n;
    /* demo: the simulated ECU is "in its bootloader" while the demo connection is down */
    if (st->sim_mode && !st->sim_gone && app->conn != LT_APP_DEMO) {
        for (k = 0; k < 2 && st->n_dev < FW_MAX_DEV; k++) {
            lt_fw_found *f = &st->dev[st->n_dev++];
            memset(f, 0, sizeof(*f));
            f->method = k == 0 ? LT_FW_DFUSE : LT_FW_OPENBLT;
            f->sim = 1;
            f->vid = (lt_u16)(k == 0 ? LT_FW_DFU_VID : LT_FW_OPENBLT_VID);
            f->pid = (lt_u16)(k == 0 ? LT_FW_DFU_PID : LT_FW_OPENBLT_PID);
            lt_snprintf(f->label, sizeof(f->label), "%s %04x:%04x (simulated, demo)",
                        k == 0 ? "STM32 BOOTLOADER" : "OpenBLT", (unsigned)f->vid, (unsigned)f->pid);
        }
    }
}

static int find_dev(const fw_state *st, int method)
{
    int i;
    for (i = 0; i < st->n_dev; i++) {
        if (st->dev[i].method == method && !st->dev[i].sim) { return i; }
    }
    for (i = 0; i < st->n_dev; i++) {
        if (st->dev[i].method == method) { return i; }
    }
    return -1;
}

/* INI controller command that reboots into the bootloader: cmd_dfu / cmd_openblt, else by name */
static const lt_ini_cmd *reboot_cmd(const lt_app *app, int method)
{
    const char *exact = method == LT_FW_DFUSE ? "cmd_dfu" : "cmd_openblt";
    const char *part = method == LT_FW_DFUSE ? "dfu" : "openblt";
    const lt_ini_cmd *c;
    size_t i;
    if (app->ini == NULL) { return NULL; }
    c = lt_ini_find_command(app->ini, exact);
    if (c != NULL) { return c; }
    for (i = 0; i < app->ini->n_commands; i++) {
        c = app->ini->commands[i];
        if (c->name != NULL && lt_dlg_contains_ci(c->name, part)) { return c; }
    }
    return NULL;
}

static void do_reboot(lt_app *app, fw_state *st)
{
    const lt_ini_cmd *c = reboot_cmd(app, st->method);
    const char *what = st->method == LT_FW_DFUSE ? "DFU" : "OpenBLT";
    if (!is_online(app)) { lt_app_toast(app, 2, "Connect to the ECU first"); return; }
    if (c == NULL && app->conn != LT_APP_DEMO) {
        lt_app_toast(app, 3, "The ECU definition has no command to reboot into %s", what);
        return;
    }
    if (c != NULL && lt_proto_run_command(app->proto, c) < 0 && app->conn != LT_APP_DEMO) {
        lt_app_toast(app, 3, "Command failed: %s", lt_err_get());
        return;
    }
    st->was_demo = app->conn == LT_APP_DEMO;
    logf_(st, 0, "Sent %s: the ECU reboots into %s; disconnected", c != NULL ? c->name : "(demo)", what);
    lt_app_disconnect(app);
    st->waiting = 1;
    st->sim_gone = 0;
    st->reboot_method = st->method;
    st->wait_until = lt_plat_now_ms() + FW_WAIT_DEV_MS;
    st->next_scan = lt_plat_now_ms() + (st->sim_mode ? 1500.0 : 500.0);
}

/* ------------------------------------------------------------------ the job */
static void start_job(lt_app *app, fw_state *st)
{
    const lt_fw_preset *p = lt_fw_preset_get(st->preset);
    lt_fw_opts o;
    int di = find_dev(st, st->method);
    char err[240];
    if (!st->img_ok || di < 0 || st->running) { return; }
    job_close(st);
    st->usb_err = 0;
    st->result[0] = '\0';
    st->finished = 0;
    if (st->dev[di].sim) {
        lt_fw_sim_opts so;
        memset(&so, 0, sizeof(so));
        so.kind = st->method == LT_FW_DFUSE ? LT_FW_SIM_DFUSE : LT_FW_SIM_OPENBLT;
        so.erase_ms_16k = 110;
        so.write_ms = 6;
        so.busy_ms = 2500;
        st->sim = lt_fw_sim_new(&so, sim_clock, NULL);
        if (st->sim == NULL) { lt_strlcpy(st->result, "out of memory", sizeof(st->result)); return; }
        lt_fw_sim_io(st->sim, &st->io);
    } else {
        err[0] = '\0';
        st->usb = lt_fw_usb_open(st->dev[di].path, &st->io, err, sizeof(err));
        if (st->usb == NULL) {
            if (lt_dlg_contains_ci(err, "permission") || lt_dlg_contains_ci(err, "udev") ||
                lt_dlg_contains_ci(err, "winusb") || lt_dlg_contains_ci(err, "driver")) {
                st->usb_err = LT_USB_EACCES;
            } else if (lt_dlg_contains_ci(err, "busy") || lt_dlg_contains_ci(err, "in use")) {
                st->usb_err = LT_USB_EBUSY;
            }
            lt_snprintf(st->result, sizeof(st->result), "Cannot open %s: %s", st->dev[di].label,
                        err[0] != '\0' ? err : "unknown error");
            add_log(st, 3, st->result);
            st->finished = 1;
            st->final_phase = LT_FW_PH_FAILED;
            return;
        }
    }
    memset(&o, 0, sizeof(o));
    o.verify = st->verify;
    o.leave = st->leave;
    o.keep_settings = st->keep && p->n_protect > 0;
    o.protect = p->protect;
    o.n_protect = p->n_protect;
    o.app_base = st->method == LT_FW_OPENBLT ? p->app_base : 0;
    logf_(st, 0, "Flashing %s with %s (%s)", st->demo_img ? "the demo image" : lt_path_base(st->path),
          st->method == LT_FW_DFUSE ? "USB DFU" : "OpenBLT", st->dev[di].label);
    st->job = lt_fw_job_new(st->method, &st->io, &st->img, &o, job_log, st);
    if (st->job == NULL) {
        job_close(st);
        lt_strlcpy(st->result, "cannot start the job", sizeof(st->result));
        return;
    }
    st->running = 1;
    LT_UNUSED(app);
}

static void job_finished(lt_app *app, fw_state *st)
{
    lt_fw_job_progress(st->job, &st->prog);
    st->final_phase = st->prog.phase;
    st->usb_err = st->prog.usb_err;
    lt_strlcpy(st->result, st->prog.msg, sizeof(st->result));
    st->finished = 1;
    if (st->sim != NULL && lt_fw_sim_left(st->sim)) { st->sim_gone = 1; }
    job_close(st);
    if (st->final_phase == LT_FW_PH_DONE) {
        lt_app_toast(app, 1, "Firmware update finished");
        if (st->leave && !st->was_demo && app->settings.port[0] != '\0' && app->conn == LT_APP_OFFLINE) {
            (void)lt_app_reconnect_request(app, "firmware", -1, -1);
        }
    } else if (st->final_phase == LT_FW_PH_FAILED) {
        lt_app_toast(app, 3, "Firmware update failed");
    }
}

static void wb_tick(lt_app *app, fw_state *st);

static void tick(lt_app *app)
{
    fw_state *st = fw_get(app);
    double t0, now;
    int n = 0;
    if (st->running && st->job != NULL) {
        t0 = lt_plat_now_ms();
        now = t0;
        while (n < 400 && now - t0 < FW_STEP_BUDGET_MS) {
            double nx = lt_fw_next_ms(st->job);
            if (now < nx) {                     /* a short device poll delay: wait for it inside the budget */
                if (nx - t0 > FW_STEP_BUDGET_MS) { break; }
                lt_plat_sleep_ms((int)(nx - now) + 1);
                now = lt_plat_now_ms();
                continue;
            }
            if (!lt_fw_step(st->job, now)) { job_finished(app, st); break; }
            n++;
            now = lt_plat_now_ms();
        }
        if (st->job != NULL) { lt_fw_job_progress(st->job, &st->prog); }
    }
    if (st->wb == WB_RUN) { wb_tick(app, st); }
}

/* ------------------------------------------------------------------ on-board wideband */
static const lt_ini_cmd *wb_cmd(const lt_app *app)
{
    return app->ini != NULL ? lt_ini_find_command(app->ini, "cmd_miata_wbo_uart_flash") : NULL;
}

/* the last "WBO UART:" line of a console reply, or the last line */
static void wb_line(const char *text, char *out, size_t cap)
{
    const char *p = text, *best = NULL;
    out[0] = '\0';
    while (p != NULL && *p != '\0') {
        const char *e = strchr(p, '\n');
        size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
        if (len > 0 && (best == NULL || lt_dlg_contains_ci(p, "WBO UART"))) {
            if (len >= cap) { len = cap - 1; }
            memcpy(out, p, len);
            out[len] = '\0';
            while (len > 0 && (out[len - 1] == '\r' || out[len - 1] == ' ')) { out[--len] = '\0'; }
            best = p;
        }
        p = e != NULL ? e + 1 : NULL;
    }
}

static void wb_start(lt_app *app, fw_state *st)
{
    const lt_ini_cmd *c = wb_cmd(app);
    char *out = NULL;
    if (!is_online(app)) { lt_app_toast(app, 2, "Connect to the ECU first"); return; }
    st->wb_msg[0] = '\0';
    if (c != NULL) {
        if (lt_proto_run_command(app->proto, c) < 0) {
            lt_snprintf(st->wb_msg, sizeof(st->wb_msg), "Command failed: %s", lt_err_get());
            st->wb = WB_FAIL;
            return;
        }
        logf_(st, 0, "Wideband: sent %s", c->name);
    } else {
        if (lt_proto_console(app->proto, "wbo_uart_flash", 1500, &out) < 0) {
            lt_snprintf(st->wb_msg, sizeof(st->wb_msg), "Console command failed: %s", lt_err_get());
            st->wb = WB_FAIL;
            lt_free(out);
            return;
        }
        if (out != NULL) { wb_line(out, st->wb_msg, sizeof(st->wb_msg)); }
        logf_(st, 0, "Wideband: wbo_uart_flash -> %s", st->wb_msg[0] != '\0' ? st->wb_msg : "(no reply)");
        lt_free(out);
        if (lt_dlg_contains_ci(st->wb_msg, "refused") || lt_dlg_contains_ci(st->wb_msg, "already running")) {
            st->wb = WB_FAIL;
            return;
        }
    }
    st->wb = WB_RUN;
    st->wb_polls = 0;
    st->wb_t0 = lt_plat_now_ms();
    st->wb_next = st->wb_t0 + WB_POLL_MS;
}

static void wb_tick(lt_app *app, fw_state *st)
{
    char *out = NULL;
    char line[200];
    double now = lt_plat_now_ms();
    if (now < st->wb_next) { return; }
    st->wb_next = now + WB_POLL_MS;
    if (!is_online(app)) {
        st->wb = WB_FAIL;
        lt_strlcpy(st->wb_msg, "The connection dropped while the wideband was being flashed", sizeof(st->wb_msg));
        return;
    }
    if (now - st->wb_t0 > WB_LIMIT_MS) {
        st->wb = WB_FAIL;
        lt_strlcpy(st->wb_msg, "No result after a minute - check wbo_uart_status in the ECU console", sizeof(st->wb_msg));
        return;
    }
    st->wb_polls++;
    if (lt_proto_console(app->proto, "wbo_uart_status", 1500, &out) < 0) {
        lt_free(out);
        return;                                     /* try again next second */
    }
    line[0] = '\0';
    if (out != NULL) { wb_line(out, line, sizeof(line)); }
    lt_free(out);
    if (line[0] == '\0') { return; }
    if (strcmp(line, st->wb_msg) != 0) { logf_(st, 0, "Wideband: %s", line); }
    lt_strlcpy(st->wb_msg, line, sizeof(st->wb_msg));
    if (lt_dlg_contains_ci(line, "FAILED")) {
        st->wb = WB_FAIL;
        lt_app_toast(app, 3, "Wideband flash failed");
    } else if (lt_dlg_contains_ci(line, "done")) {
        st->wb = WB_DONE;
        lt_app_toast(app, 1, "On-board wideband flashed and verified");
    } else if (!lt_dlg_contains_ci(line, "running") && st->wb_polls > 3) {
        st->wb = WB_FAIL;                           /* idle without a result: refused or unknown */
    }
}

/* ------------------------------------------------------------------ drawing */
static void row_label(lt_ui *ui, const char *label)
{
    lt_ui_label_ex(ui, label, lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
}

static void init_state(lt_app *app, fw_state *st, const char *target)
{
    char *e;
    st->init = 1;
    st->preset = lt_fw_preset_for_signature(app->ini != NULL ? app->ini->signature : NULL);
    if (app->signature[0] != '\0' && st->preset == LT_FW_PRESET_GENERIC) {
        st->preset = lt_fw_preset_for_signature(app->signature);
    }
    e = lt_plat_getenv("LT_FW_SIM");
    st->sim_mode = app->conn == LT_APP_DEMO || (e != NULL && e[0] != '\0' && e[0] != '0');
    lt_free(e);
    if (app->headless) {
        st->preset = env_int("LT_SHOT_FW_PRESET", st->preset);
        if (st->preset < 0 || st->preset >= LT_FW_PRESET_COUNT) { st->preset = LT_FW_PRESET_GENERIC; }
        st->method = env_int("LT_SHOT_FW_METHOD", 0) != 0 ? LT_FW_OPENBLT : LT_FW_DFUSE;
        st->shot_start = env_int("LT_SHOT_FW_START", 0);
        st->shot_wb = env_int("LT_SHOT_FW_WBO", 0);
    }
    st->verify = 1;
    st->leave = 1;
    st->keep = lt_fw_preset_get(st->preset)->n_protect > 0;
    if (!lt_fw_preset_get(st->preset)->openblt) { st->method = LT_FW_DFUSE; }
    st->was_demo = app->conn == LT_APP_DEMO;
    load_image(st);
    scan(app, st);
    st->next_scan = lt_plat_now_ms() + 1500.0;
    if (target != NULL && strcmp(target, "dfu") == 0) {      /* opened after "Enter DFU Mode" */
        st->method = LT_FW_DFUSE;
        st->waiting = 1;
        st->wait_until = lt_plat_now_ms() + FW_WAIT_DEV_MS;
    }
}

static void preset_changed(fw_state *st)
{
    const lt_fw_preset *p = lt_fw_preset_get(st->preset);
    st->keep = p->n_protect > 0;
    if (!p->openblt) { st->method = LT_FW_DFUSE; }
    if (p->bin_base != lt_fw_preset_get(st->loaded_preset)->bin_base) { load_image(st); }
    st->loaded_preset = st->preset;
}

static void draw_image(lt_app *app, fw_state *st, int busy)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    const lt_fw_preset *p = lt_fw_preset_get(st->preset);
    int w4[4], w1 = -1, i;
    char b[260];
    const char *picked = lt_app_file_result(app, FD_FW_IMAGE);
    if (picked != NULL) { lt_strlcpy(st->path, picked, sizeof(st->path)); load_image(st); }
    w4[0] = 110; w4[1] = -1; w4[2] = 100; w4[3] = p->url != NULL ? 120 : 1;
    lt_ui_layout_row(ui, 4, w4, 0);
    row_label(ui, "Image file");
    lt_ui_disabled_begin(ui, busy ? LT_TRUE : LT_FALSE);
    if (lt_ui_text(ui, "fwpath", st->path, sizeof(st->path), 0) & LT_UI_SUBMIT) { load_image(st); }
    if (lt_ui_button(ui, "Browse...")) {
        lt_app_file_dialog(app, FD_FW_IMAGE, LT_UI_FILE_OPEN, NULL, "",
                           "Firmware|*.bin;*.hex;*.dfu;*.srec;*.s19;*.ihex|All files|*");
    }
    lt_ui_disabled_end(ui);
    if (p->url != NULL) {
        if (lt_ui_link(ui, "Get firmware")) {
            if (lt_plat_open_url(p->url) != 0) { lt_app_toast(app, 2, "Open %s in a browser", p->url); }
        }
        lt_ui_tooltip(ui, "GitHub Actions of v3ai/miata-ecus: open the latest Firmware run and download "
                          "rusefi_miata-ecu.bin (or .dfu / .hex) from its artifacts");
    } else {
        lt_ui_label(ui, "");
    }
    if (!busy && strcmp(st->path, st->loaded) != 0 && !lt_ui_text_focused(ui)) { load_image(st); }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (st->img_err[0] != '\0') {
        lt_ui_label_wrap(ui, st->img_err, th->error);
        return;
    }
    if (!st->img_ok) {
        lt_ui_label_wrap(ui, p->url != NULL
                         ? "Pick rusefi_miata-ecu.bin (OpenBLT + rusEFI, written at 0x08000000), .dfu or .hex "
                           "from the Firmware workflow artifacts (Get firmware)."
                         : "Pick a firmware image: .bin (written at 0x08000000), Intel .hex, .srec or ST .dfu.",
                         th->muted);
        return;
    }
    lt_snprintf(b, sizeof(b), "%s%s", st->demo_img ? "Demo image (synthetic): " : "", st->img_sum);
    lt_ui_label_wrap(ui, b, th->text);
    if (st->facts.merged_openblt) {
        lt_snprintf(b, sizeof(b), "OpenBLT bootloader + rusEFI app at 0x08008000; app CRC header %s",
                    st->facts.app_crc == 1 ? "OK" : "does not match");
        lt_ui_label_wrap(ui, b, st->facts.app_crc == 1 ? th->success : th->warning);
    } else if (st->facts.vectors == 1) {
        lt_snprintf(b, sizeof(b), "Vector table OK (stack 0x%08lX, reset 0x%08lX)", (unsigned long)st->facts.sp,
                    (unsigned long)st->facts.reset);
        lt_ui_label_wrap(ui, b, th->success);
    }
    for (i = 0; i < st->facts.n_warn; i++) { lt_ui_label_wrap(ui, st->facts.warn[i], th->warning); }
    if (st->preset == LT_FW_PRESET_MIATA && !st->facts.merged_openblt && st->img.lo == LT_FW_STM32_FLASH) {
        lt_ui_label_wrap(ui, "Not a merged OpenBLT + rusEFI image: writing it at 0x08000000 replaces the "
                             "bootloader, and later updates without buttons stop working.", th->warning);
    }
    for (i = 0; i < p->n_protect; i++) {
        if (lt_fw_image_touches(&st->img, p->protect[i].addr, p->protect[i].len)) {
            lt_snprintf(b, sizeof(b), "The image covers 0x%08lX-0x%08lX (%s).", (unsigned long)p->protect[i].addr,
                        (unsigned long)(p->protect[i].addr + p->protect[i].len - 1u), p->protect[i].what);
            lt_ui_label_wrap(ui, b, th->warning);
        }
    }
    if (st->method == LT_FW_OPENBLT && p->app_base != 0 && st->img.last < p->app_base) {
        lt_ui_label_wrap(ui, "This image holds only the bootloader: nothing to send over OpenBLT.", th->error);
    }
}

static void draw_usb_hint(lt_app *app, fw_state *st, int code)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int win = lt_plat_os() == LT_OS_WINDOWS, w1 = -1, w2[2];
    const char *h = lt_fw_usb_hint(code, win);
    LT_UNUSED(st);
    if (h == NULL) { return; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, h, th->warning);
    if (code == LT_USB_EACCES && !win) {
        const char *p = lt_fw_udev_rule;
        while (*p != '\0') {
            char line[200];
            size_t n = 0;
            while (p[n] != '\0' && p[n] != '\n' && n < sizeof(line) - 1) { n++; }
            memcpy(line, p, n);
            line[n] = '\0';
            lt_ui_label_ex(ui, line, th->text2, LT_UI_ALIGN_LEFT);
            p += n;
            if (*p == '\n') { p++; }
        }
        w2[0] = 140; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "Copy rule")) {
            lt_ui_clipboard_set(ui, lt_fw_udev_rule);
            lt_app_toast(app, 1, "udev rule copied");
        }
        lt_ui_label_ex(ui, "then: sudo udevadm control --reload-rules", th->muted, LT_UI_ALIGN_LEFT);
    }
}

static void draw_device(lt_app *app, fw_state *st, int busy)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    const lt_fw_preset *p = lt_fw_preset_get(st->preset);
    int w3[3], w1 = -1, di, online = is_online(app);
    char b[240];
    double now = lt_plat_now_ms();
    w3[0] = 110; w3[1] = -1; w3[2] = -1;
    lt_ui_layout_row(ui, 3, w3, 0);
    row_label(ui, "Method");
    lt_ui_disabled_begin(ui, busy ? LT_TRUE : LT_FALSE);
    lt_ui_radio(ui, "USB DFU (ST bootloader)", &st->method, LT_FW_DFUSE);
    if (p->openblt) { lt_ui_radio(ui, "OpenBLT over USB (no buttons)", &st->method, LT_FW_OPENBLT); }
    else { lt_ui_label(ui, ""); }
    lt_ui_disabled_end(ui);
    if (!busy && (now >= st->next_scan)) {
        scan(app, st);
        st->next_scan = now + (st->waiting ? 500.0 : 1500.0);
    }
    di = find_dev(st, st->method);
    if (st->waiting && (di >= 0 || now > st->wait_until)) {
        st->waiting = 0;
        if (di >= 0) { logf_(st, 1, "Found %s", st->dev[di].label); }
        else { logf_(st, 2, "The bootloader did not show up; use the buttons (see below) and scan again"); }
    }
    w3[0] = 110; w3[1] = -1; w3[2] = 90;
    lt_ui_layout_row(ui, 3, w3, 0);
    row_label(ui, "Device");
    if (di >= 0) {
        lt_snprintf(b, sizeof(b), "%s found", st->dev[di].label);
        lt_ui_label_ex(ui, b, th->success, LT_UI_ALIGN_LEFT);
    } else if (st->waiting) {
        lt_snprintf(b, sizeof(b), "Waiting for the %s to appear... %d s", st->method == LT_FW_DFUSE ? "STM32 BOOTLOADER" : "OpenBLT device",
                    (int)((st->wait_until - now) / 1000.0) + 1);
        lt_ui_label_ex(ui, b, th->accent, LT_UI_ALIGN_LEFT);
    } else if (st->scan_rc == LT_USB_ENOTSUP) {
        lt_ui_label_ex(ui, "USB access is not available in this build", th->warning, LT_UI_ALIGN_LEFT);
    } else if (st->scan_rc < 0) {
        lt_snprintf(b, sizeof(b), "Cannot list USB devices: %s", lt_usb_strerror(st->scan_rc));
        lt_ui_label_ex(ui, b, th->warning, LT_UI_ALIGN_LEFT);
    } else {
        lt_snprintf(b, sizeof(b), "No %s found", st->method == LT_FW_DFUSE ? "STM32 BOOTLOADER (0483:df11)"
                                                                           : "OpenBLT device (1d50:60ac)");
        lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
    }
    if (lt_ui_button_ex(ui, "Scan", busy ? LT_UI_BTN_DISABLED : 0) && !busy) {
        scan(app, st);
        st->next_scan = now + 1500.0;
    }
    if (st->scan_rc < 0 && st->scan_rc != LT_USB_ENOTSUP) { draw_usb_hint(app, st, st->scan_rc); }
    if (st->scan_rc == LT_USB_ENOTSUP && !st->sim_mode) { draw_usb_hint(app, st, LT_USB_ENOTSUP); }
    /* reboot into the bootloader through the connection */
    if (online && di < 0 && !st->waiting) {
        const lt_ini_cmd *c = reboot_cmd(app, st->method);
        if (c != NULL || app->conn == LT_APP_DEMO) {
            int w2[2];
            w2[0] = 220; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            if (lt_ui_button_ex(ui, st->method == LT_FW_DFUSE ? "Reboot ECU into DFU" : "Reboot into OpenBLT",
                                busy ? LT_UI_BTN_DISABLED : 0) && !busy) {
                st->reboot_confirm = 1;
            }
            lt_snprintf(b, sizeof(b), "sends %s, then waits for the bootloader", c != NULL ? c->name : "a demo reboot");
            lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
        }
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (st->method == LT_FW_DFUSE) {
        lt_ui_label_wrap(ui, st->preset == LT_FW_PRESET_MIATA
            ? "First time (or no working firmware): hold BOOT (SW1 on the daughterboard), tap RESET (SW2), "
              "then release BOOT. The board shows up as STM32 BOOTLOADER on its USB-C port."
            : "No working firmware: hold the board's BOOT0 button while you reset or power it, then release it. "
              "The board shows up as STM32 BOOTLOADER.", th->muted);
    } else {
        lt_ui_label_wrap(ui, "OpenBLT takes the update over the normal USB port without buttons: connect to the "
                             "ECU and use Reboot into OpenBLT, or reset the ECU and start within its short "
                             "bootloader window.", th->muted);
    }
}

static void draw_progress(lt_app *app, fw_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, i, first, cell = lt_ui_cell_mode(ui);
    char b[200];
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (st->running) {
        const lt_fw_progress *p = &st->prog;
        if (p->goal > 0) {
            lt_snprintf(b, sizeof(b), "%s  %lu / %lu KB  (%.0f%% overall)", lt_fw_phase_name(p->phase),
                        (unsigned long)(p->done / 1024u), (unsigned long)(p->goal / 1024u), p->frac * 100.0);
        } else {
            lt_snprintf(b, sizeof(b), "%s", lt_fw_phase_name(p->phase));
        }
        lt_ui_progress(ui, p->frac > 0.0 ? p->frac : -1.0, b);
        lt_ui_label_ex(ui, p->msg, th->text, LT_UI_ALIGN_LEFT);
    } else if (st->finished) {
        int ok = st->final_phase == LT_FW_PH_DONE;
        lt_ui_progress(ui, ok ? 1.0 : 0.0, ok ? "done" : lt_fw_phase_name(st->final_phase));
        lt_ui_label_wrap(ui, st->result, ok ? th->success : (st->final_phase == LT_FW_PH_CANCELLED ? th->warning : th->error));
        if (st->usb_err != 0) { draw_usb_hint(app, st, st->usb_err); }
        lt_ui_layout_row(ui, 1, &w1, 0);
    }
    first = st->n_log - (cell ? 5 : 7);
    if (first < 0) { first = 0; }
    for (i = first; i < st->n_log; i++) {
        const char *ln = st->log[i];
        lt_color c = ln[0] == 'e' ? th->error : (ln[0] == 'w' ? th->warning : (ln[0] == 'o' ? th->success : th->muted));
        lt_ui_label_ex(ui, ln + 1, c, LT_UI_ALIGN_LEFT);
    }
}

static void draw_wideband(lt_app *app, fw_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], online = is_online(app), can = online && st->wb != WB_RUN && !st->running;
    char b[260];
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_ui_heading(ui, "On-board wideband (STM32F103)");
    lt_ui_label_wrap(ui, "The main MCU programs the wideband MCU over its serial bootloader (about 15 s, the ECU "
                         "stays connected). Do it once on a new board and whenever the wideband firmware changes. "
                         "Engine off.", th->muted);
    w2[0] = 220; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    if (lt_ui_button_ex(ui, "Flash on-board wideband", can ? 0 : LT_UI_BTN_DISABLED) && can) { st->wb_confirm = 1; }
    if (!online) {
        lt_ui_label_ex(ui, "connect to the ECU first", th->muted, LT_UI_ALIGN_LEFT);
    } else if (st->wb == WB_IDLE) {
        lt_snprintf(b, sizeof(b), "uses %s", wb_cmd(app) != NULL ? "cmd_miata_wbo_uart_flash" : "the console command wbo_uart_flash");
        lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
    } else {
        lt_color c = st->wb == WB_DONE ? th->success : (st->wb == WB_FAIL ? th->error : th->accent);
        if (st->wb == WB_RUN) {
            lt_snprintf(b, sizeof(b), "%s  (%d s)", st->wb_msg[0] != '\0' ? st->wb_msg : "starting",
                        (int)((lt_plat_now_ms() - st->wb_t0) / 1000.0));
        } else {
            lt_strlcpy(b, st->wb_msg[0] != '\0' ? st->wb_msg : (st->wb == WB_DONE ? "done" : "failed"), sizeof(b));
        }
        lt_ui_label_ex(ui, b, c, LT_UI_ALIGN_LEFT);
    }
}

static void fw_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    fw_state *st = fw_get(app);
    const lt_fw_preset *p;
    static const char *const preset_names[LT_FW_PRESET_COUNT] = {
        "miata-ecus (rusEFI, STM32F407)", "rusEFI / FOME / epicEFI (STM32F4/F7/H7 DFU)", "Generic STM32 DFU (raw .bin)"
    };
    int open = d->open, w1 = -1, w2[2], wb[4], busy, fh, di, ready, ps;
    if (!st->init) { init_state(app, st, d->target); }
    p = lt_fw_preset_get(st->preset);
    busy = st->running || st->waiting;
    if (st->running) { app->anim_ms = 16.0; }
    else if (st->waiting || st->wb == WB_RUN) { app->anim_ms = 100.0; }
    if (lt_ui_modal_begin(ui, "Firmware Update", 800, 700, &open)) {
        fh = lt_ui_cell_mode(ui) ? 16 : 30;
        lt_ui_layout_row(ui, 1, &w1, -(fh + 8));
        if (lt_ui_scroll_begin(ui, "fwbody")) {
            w2[0] = 110; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            row_label(ui, "Board");
            ps = st->preset;
            lt_ui_disabled_begin(ui, busy ? LT_TRUE : LT_FALSE);
            if (lt_ui_combo(ui, "fwpreset", &ps, preset_names, LT_FW_PRESET_COUNT) && !busy && ps != st->preset) {
                st->preset = ps;
                preset_changed(st);
                p = lt_fw_preset_get(st->preset);
            }
            lt_ui_disabled_end(ui);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_wrap(ui, st->preset == LT_FW_PRESET_GENERIC
                ? "Raw images go to 0x08000000. Only the flash sectors the image covers are erased."
                : "OpenBLT at 0x08000000, rusEFI at 0x08008000. The ECU settings in flash sectors 10-11 "
                  "(0x080C0000) are kept unless you untick Keep ECU settings.", th->muted);
            draw_image(app, st, busy);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_separator(ui);
            draw_device(app, st, st->running);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_separator(ui);
            lt_ui_disabled_begin(ui, st->running ? LT_TRUE : LT_FALSE);
            lt_ui_checkbox(ui, "Verify after writing (read the flash back)", &st->verify);
            lt_ui_checkbox(ui, "Start the firmware when done", &st->leave);
            if (p->n_protect > 0) {
                lt_ui_checkbox(ui, "Keep ECU settings (flash sectors 10-11, 0x080C0000-0x080FFFFF)", &st->keep);
            }
            lt_ui_disabled_end(ui);
            if (st->running || st->finished || st->n_log > 0) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_separator(ui);
                draw_progress(app, st);
            }
            if (p->wideband) { draw_wideband(app, st); }
            lt_ui_scroll_end(ui);
        }
        di = find_dev(st, st->method);
        ready = st->img_ok && di >= 0 && !st->running && !st->waiting &&
                !(st->method == LT_FW_OPENBLT && p->app_base != 0 && st->img.last < p->app_base);
        wb[0] = -1; wb[1] = 120; wb[2] = 100; wb[3] = 110;
        lt_ui_layout_row(ui, 4, wb, 0);
        if (st->running) { lt_ui_label_ex(ui, "Engine off. Do not unplug the USB cable or the power.", th->warning, LT_UI_ALIGN_LEFT); }
        else if (st->finished && st->final_phase == LT_FW_PH_DONE) { lt_ui_label_ex(ui, "Firmware updated", th->success, LT_UI_ALIGN_LEFT); }
        else if (!st->img_ok) { lt_ui_label_ex(ui, "Pick an image file", th->muted, LT_UI_ALIGN_LEFT); }
        else if (di < 0) { lt_ui_label_ex(ui, "No bootloader device", th->muted, LT_UI_ALIGN_LEFT); }
        else { lt_ui_label(ui, ""); }
        if (st->finished && st->final_phase == LT_FW_PH_DONE && !is_online(app)) {
            if (lt_ui_button(ui, "Reconnect")) {
                if (st->was_demo) { (void)lt_app_connect_demo(app); }
                else if (app->settings.port[0] != '\0') {
                    (void)lt_app_connect_serial(app, app->settings.port, app->settings.baud > 0 ? app->settings.baud : 115200);
                } else { (void)lt_app_connect_dialog(app); }
            }
        } else {
            lt_ui_label(ui, "");
        }
        if (st->running) {
            if (lt_ui_button_ex(ui, "Cancel", LT_UI_BTN_DANGER)) { lt_fw_cancel(st->job); }
        } else if (lt_ui_button(ui, "Close")) {
            open = 0;
        }
        if (lt_ui_button_ex(ui, "Start", ready ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && ready) { st->confirm = 1; }
        lt_ui_modal_end(ui);
    }
    if (st->shot_start && app->headless && st->img_ok) {   /* screenshots: reboot (demo), then flash */
        if (find_dev(st, st->method) >= 0 && !st->waiting) { st->shot_start = 0; start_job(app, st); }
        else if (is_online(app) && !st->waiting) { do_reboot(app, st); }
    }
    if (st->shot_wb && app->headless && is_online(app)) {
        st->shot_wb = 0;
        wb_start(app, st);
    }
    if (st->confirm) {
        char m[480];
        int r;
        lt_snprintf(m, sizeof(m), "Write %s to the ECU over %s?\n\nThe engine must be off. Do not unplug the USB "
                    "cable or the ECU power until it finishes.%s",
                    st->demo_img ? "the demo image" : lt_path_base(st->path),
                    st->method == LT_FW_DFUSE ? "USB DFU" : "OpenBLT",
                    st->keep && p->n_protect > 0 ? " Your ECU settings are kept." : " The ECU settings may be erased.");
        r = lt_ui_msgbox(ui, "fwconfirm", "Flash Firmware", m, "Flash|Cancel", &st->confirm);
        if (r == 1) { start_job(app, st); }
    }
    if (st->reboot_confirm) {
        int r = lt_ui_msgbox(ui, "fwreboot", "Reboot into the Bootloader",
                             "The ECU reboots into its bootloader and the connection drops. Engine off. Continue?",
                             "Reboot|Cancel", &st->reboot_confirm);
        if (r == 1) { do_reboot(app, st); }
    }
    if (st->wb_confirm) {
        int r = lt_ui_msgbox(ui, "fwwbo", "Flash On-board Wideband",
                             "Flash the on-board wideband MCU now? The engine must be off; it takes about 15 seconds "
                             "and the ECU stays connected.", "Flash|Cancel", &st->wb_confirm);
        if (r == 1) { wb_start(app, st); }
    }
    if (!open && st->running) {
        open = 1;                                   /* closing while flashing: ask to cancel instead */
        lt_app_toast(app, 2, "Flashing in progress - use Cancel to stop it");
    }
    d->open = open;
}

static void fw_close(lt_app *app, lt_app_dialog *d)
{
    fw_state *st = fw_get(app);
    LT_UNUSED(d);
    if (!st->running) {
        job_close(st);
        st->init = 0;                               /* fresh defaults (preset, scan) next time */
        st->finished = 0;
        st->waiting = 0;
    }
}

static void act_firmware(lt_app *app) { (void)lt_app_dialog_push(app, "firmware", "", "Firmware Update"); }

static int open_firmware(lt_app *app, const char *t)
{
    LT_UNUSED(t);
    return lt_app_dialog_push(app, "firmware", "", "Firmware Update") != NULL ? 0 : -1;
}

/* the Tools menu needs an ECU definition: offer the flasher in File on the welcome screen */
static int pred_no_ini(lt_app *app) { return app->ini == NULL; }

void lt_feat_firmware_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "firmware"; dk.draw = fw_draw; dk.close = fw_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_target(reg, "firmware", open_firmware);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Firmware &Update...", NULL, act_firmware, NULL);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Firmware &Update...", NULL, act_firmware, pred_no_ini);
    lt_ext_add_tick(reg, tick);
}
