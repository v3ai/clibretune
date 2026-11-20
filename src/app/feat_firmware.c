/* feat_firmware.c - Tools > Firmware Update: flash an ECU with no external tool. Methods (src/fw):
 * ST DfuSe ROM bootloader and OpenBLT over USB (miata-ecus, rusEFI family, generic STM32), the
 * Arduino Mega 2560 STK500v2 bootloader over serial (Speeduino), Teensy HalfKay over USB
 * (Speeduino on Teensy 3.5 / 3.6 / 4.1) and OpenBLT over a UART or over CAN through an SLCAN
 * adapter. Board presets pick the methods; the reboot into the bootloader goes through the INI
 * controller commands; the miata-ecus on-board wideband flasher (board command / rusEFI console
 * wbo_uart_flash + wbo_uart_status) is here too.
 * The flashing job (src/fw) is stepped from the per-frame tick in a small time budget, so
 * the UI stays responsive. With --demo (or LT_FW_SIM=1) simulated bootloaders from
 * src/fw/fw_sim*.c are listed (USB devices and a simulated serial port) so the whole flow runs
 * without hardware.
 * Screenshot helpers (headless only): LT_SHOT_FW_PRESET=n, LT_SHOT_FW_METHOD=n (LT_FW_*),
 * LT_SHOT_FW_START=1 (start without the confirmation), LT_SHOT_FW_WBO=1, LT_SHOT_FW_EACCES=1
 * (pretend the USB scan had no permission: shows the udev rule actions).
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
#define FD_FW_UDEV (LT_APP_FD_USER0 + 91)
#define FW_LOG 150
#define FW_LOG_LEN 200
#define FW_MAX_DEV 8
#define FW_MAX_PORTS 24
#define FW_STEP_BUDGET_MS 12.0
#define FW_WAIT_DEV_MS 25000.0
#define WB_POLL_MS 1000.0
#define WB_LIMIT_MS 60000.0
#define SIM_PORT "sim:"

enum { WB_IDLE = 0, WB_RUN, WB_DONE, WB_FAIL };

static const char *const uart_baud_names[6] = { "9600", "19200", "38400", "57600", "115200", "230400" };
static const long uart_bauds[6] = { 9600L, 19200L, 38400L, 57600L, 115200L, 230400L };
static const char *const can_rate_names[5] = { "125 kbit/s", "250 kbit/s", "500 kbit/s", "800 kbit/s",
                                               "1000 kbit/s" };
static const int can_rates[5] = { 125, 250, 500, 800, 1000 };

typedef struct fw_state {
    int init;
    int preset, method;
    int verify, leave, keep;
    char path[512], loaded[512];
    int loaded_preset;
    lt_fw_image img;
    int img_ok, demo_img;
    char img_err[240], img_sum[200], img_fit[240];
    lt_fw_facts facts;
    /* bootloader devices */
    int sim_mode, sim_gone;
    lt_fw_found dev[FW_MAX_DEV];
    int n_dev, scan_rc;
    double next_scan;
    /* serial ports (STK500, OpenBLT UART / CAN) */
    char *ports[FW_MAX_PORTS];
    const char *port_items[FW_MAX_PORTS];
    int n_ports;
    char port[256];
    int uart_baud, can_rate;
    char can_tx[16], can_rx[16];
    double next_ports;
    /* reboot into the bootloader, then wait for it to enumerate */
    int waiting, reboot_confirm, reboot_method;
    double wait_until;
    /* the flashing job */
    lt_fw_job *job;
    lt_fw_io io;
    lt_fw_usb *usb;
    lt_fw_serial *ser;
    lt_fw_sim *sim;
    int running, finished, final_phase, usb_err, confirm, was_demo;
    lt_fw_progress prog;
    char result[240], ser_err[240];
    char (*log)[FW_LOG_LEN];
    int n_log;
    /* udev rule saved by "Save udev rule..." */
    char udev_path[512];
    /* on-board wideband */
    int wb, wb_confirm, wb_polls;
    double wb_next, wb_t0;
    char wb_msg[200];
    int shot_start, shot_wb, shot_eacces;
} fw_state;

static void ports_free(fw_state *st)
{
    int i;
    for (i = 0; i < st->n_ports; i++) { lt_free(st->ports[i]); st->ports[i] = NULL; }
    st->n_ports = 0;
}

static void job_close(fw_state *st)
{
    lt_fw_job_free(st->job);
    st->job = NULL;
    lt_fw_usb_close(st->usb);
    st->usb = NULL;
    lt_fw_serial_close(st->ser);
    st->ser = NULL;
    lt_fw_sim_free(st->sim);
    st->sim = NULL;
    st->running = 0;
}

static void fw_free(lt_app *app, void *p)
{
    fw_state *st = (fw_state *)p;
    LT_UNUSED(app);
    job_close(st);
    ports_free(st);
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

static int has_method(const lt_fw_preset *p, int m)
{
    return m >= 0 && m < LT_FW_METHOD_COUNT && (p->methods & (1 << m)) != 0;
}

static int first_method(const lt_fw_preset *p)
{
    int m;
    for (m = 0; m < LT_FW_METHOD_COUNT; m++) {
        if (has_method(p, m)) { return m; }
    }
    return LT_FW_DFUSE;
}

/* STM32 presets get the vector table / OpenBLT image checks */
static int stm32_preset(const lt_fw_preset *p)
{
    return (p->methods & ((1 << LT_FW_DFUSE) | (1 << LT_FW_OPENBLT) | (1 << LT_FW_OPENBLT_UART) |
                          (1 << LT_FW_OPENBLT_CAN))) != 0;
}

static int is_openblt(int m)
{
    return m == LT_FW_OPENBLT || m == LT_FW_OPENBLT_UART || m == LT_FW_OPENBLT_CAN;
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
    st->img_fit[0] = '\0';
    memset(&st->facts, 0, sizeof(st->facts));
    lt_strlcpy(st->loaded, st->path, sizeof(st->loaded));
    st->loaded_preset = st->preset;
    if (st->path[0] == '\0') {
        if (!st->sim_mode) { return; }
        /* demo: a synthetic image that suits the board */
        if (has_method(p, LT_FW_STK500)) { lt_fw_sim_avr_image(&st->img, 60); }
        else if (has_method(p, LT_FW_HALFKAY)) {
            lt_fw_sim_teensy_image(&st->img, p->dev_base == LT_FW_TEENSY4_FLASH, 180);
        } else { lt_fw_sim_demo_image(&st->img, st->preset != LT_FW_PRESET_OPENBLT, 400); }
        st->demo_img = 1;
    } else if (lt_fw_image_load(&st->img, st->path, p->bin_base, st->img_err, sizeof(st->img_err)) != 0) {
        return;
    }
    st->img_ok = st->img.n_segs > 0;
    lt_fw_image_summary(&st->img, st->img_sum, sizeof(st->img_sum));
    if (stm32_preset(p)) { lt_fw_image_facts(&st->img, &st->facts); }
    if (p->dev_size != 0 && st->img_ok) {
        (void)lt_fw_image_fits(&st->img, p->dev_base, p->dev_size, st->img_fit, sizeof(st->img_fit));
    }
}

/* ------------------------------------------------------------------ devices */
static void scan(const lt_app *app, fw_state *st)
{
    static const int sim_m[3] = { LT_FW_DFUSE, LT_FW_OPENBLT, LT_FW_HALFKAY };
    static const char *const sim_n[3] = { "STM32 BOOTLOADER", "OpenBLT", "Teensy HalfKay" };
    int n = lt_fw_scan(st->dev, FW_MAX_DEV), k;
    st->scan_rc = n < 0 ? n : 0;
    st->n_dev = n < 0 ? 0 : n;
    if (st->shot_eacces) { st->scan_rc = LT_USB_EACCES; st->n_dev = 0; }
    /* demo: the simulated ECU is "in its bootloader" while the demo connection is down; the
     * simulated Teensy is always there (its bootloader is entered with a button, not a command) */
    if (st->sim_mode && !st->sim_gone) {
        for (k = 0; k < 3 && st->n_dev < FW_MAX_DEV; k++) {
            lt_fw_found *f;
            if (k < 2 && app->conn == LT_APP_DEMO) { continue; }
            f = &st->dev[st->n_dev++];
            memset(f, 0, sizeof(*f));
            f->method = sim_m[k];
            f->sim = 1;
            f->vid = (lt_u16)(k == 0 ? LT_FW_DFU_VID : (k == 1 ? LT_FW_OPENBLT_VID : LT_FW_TEENSY_VID));
            f->pid = (lt_u16)(k == 0 ? LT_FW_DFU_PID : (k == 1 ? LT_FW_OPENBLT_PID : LT_FW_TEENSY_PID));
            lt_snprintf(f->label, sizeof(f->label), "%s %04x:%04x (simulated, demo)", sim_n[k], (unsigned)f->vid,
                        (unsigned)f->pid);
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

/* serial ports for the serial methods: a simulated one first in demo mode, then the OS list */
static void list_ports(const lt_app *app, fw_state *st)
{
    char **names = NULL;
    int n = lt_serial_list(&names), i, keep = 0;
    ports_free(st);
    if (st->sim_mode) { st->ports[st->n_ports++] = lt_strdup(SIM_PORT); }
    for (i = 0; i < n && st->n_ports < FW_MAX_PORTS; i++) { st->ports[st->n_ports++] = lt_strdup(names[i]); }
    if (names != NULL) { lt_plat_free_list(names, n > 0 ? n : 0); }
    for (i = 0; i < st->n_ports; i++) {
        st->port_items[i] = strcmp(st->ports[i], SIM_PORT) == 0 ? "simulated port (demo)" : st->ports[i];
        if (strcmp(st->ports[i], st->port) == 0) { keep = 1; }
    }
    if (keep || st->n_ports == 0) { return; }
    /* the ECU connection's port is the likely one (Speeduino: the same USB serial port) */
    for (i = 0; i < st->n_ports; i++) {
        if (app->settings.port[0] != '\0' && strcmp(st->ports[i], app->settings.port) == 0) {
            lt_strlcpy(st->port, st->ports[i], sizeof(st->port));
            return;
        }
    }
    lt_strlcpy(st->port, st->ports[0], sizeof(st->port));
}

static int port_index(const fw_state *st)
{
    int i;
    for (i = 0; i < st->n_ports; i++) {
        if (strcmp(st->ports[i], st->port) == 0) { return i; }
    }
    return -1;
}

static const char *port_label(const fw_state *st)
{
    return strcmp(st->port, SIM_PORT) == 0 ? "the simulated port (demo)" : st->port;
}

/* "0x7E1" / "7e1" / "0x18FF0001x" (x or > 0x7FF = 29-bit) -> id with LT_FW_CAN_EXT, or 0 if bad */
static lt_u32 parse_can_id(const char *t)
{
    lt_u32 v = 0;
    int digits = 0, ext = 0;
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { t += 2; }
    for (; *t != '\0'; t++) {
        int c = (unsigned char)*t, d;
        if (c >= '0' && c <= '9') { d = c - '0'; }
        else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
        else if ((c == 'x' || c == 'X') && t[1] == '\0') { ext = 1; continue; }
        else { return 0; }
        if (++digits > 8) { return 0; }
        v = (v << 4) | (lt_u32)d;
    }
    if (digits == 0 || v > 0x1FFFFFFFUL) { return 0; }
    if (ext || v > 0x7FFu) { v |= LT_FW_CAN_EXT; }
    return v;
}

/* INI controller command that reboots into the bootloader: cmd_dfu / cmd_openblt, else by name */
static const lt_ini_cmd *reboot_cmd(const lt_app *app, int method)
{
    const char *exact = method == LT_FW_DFUSE ? "cmd_dfu" : "cmd_openblt";
    const char *part = method == LT_FW_DFUSE ? "dfu" : "openblt";
    const lt_ini_cmd *c;
    size_t i;
    if (app->ini == NULL || (method != LT_FW_DFUSE && !is_openblt(method))) { return NULL; }
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
    st->sim_gone = 0;
    st->reboot_method = st->method;
    if (lt_fw_method_serial(st->method)) {           /* nothing enumerates: flash right away */
        logf_(st, 0, "OpenBLT now waits on %s: click Start", st->method == LT_FW_OPENBLT_CAN ? "CAN" : "its UART");
        return;
    }
    st->waiting = 1;
    st->wait_until = lt_plat_now_ms() + FW_WAIT_DEV_MS;
    st->next_scan = lt_plat_now_ms() + (st->sim_mode ? 1500.0 : 500.0);
}

/* ------------------------------------------------------------------ the job */
static void open_failed(fw_state *st, const char *what, const char *err)
{
    lt_snprintf(st->result, sizeof(st->result), "Cannot open %s: %s", what, err[0] != '\0' ? err : "unknown error");
    add_log(st, 3, st->result);
    st->finished = 1;
    st->final_phase = LT_FW_PH_FAILED;
}

static void start_job(lt_app *app, fw_state *st)
{
    const lt_fw_preset *p = lt_fw_preset_get(st->preset);
    lt_fw_opts o;
    int serial = lt_fw_method_serial(st->method);
    int di = serial ? -1 : find_dev(st, st->method), sim;
    char err[240], what[160];
    if (!st->img_ok || st->running || (serial ? st->port[0] == '\0' : di < 0)) { return; }
    job_close(st);
    st->usb_err = 0;
    st->result[0] = '\0';
    st->ser_err[0] = '\0';
    st->finished = 0;
    sim = serial ? strcmp(st->port, SIM_PORT) == 0 : st->dev[di].sim;
    lt_strlcpy(what, serial ? port_label(st) : st->dev[di].label, sizeof(what));
    if (sim) {
        lt_fw_sim_opts so;
        memset(&so, 0, sizeof(so));
        so.kind = st->method;
        so.erase_ms_16k = 110;
        so.write_ms = st->method == LT_FW_STK500 ? 4 : 6;
        so.busy_ms = 2500;
        so.teensy4 = p->dev_base == LT_FW_TEENSY4_FLASH;
        so.can_kbps = 500;
        st->sim = lt_fw_sim_new(&so, sim_clock, NULL);
        if (st->sim == NULL) { lt_strlcpy(st->result, "out of memory", sizeof(st->result)); return; }
        lt_fw_sim_io(st->sim, &st->io);
    } else if (serial) {
        long baud = st->method == LT_FW_OPENBLT_UART ? uart_bauds[st->uart_baud] : 115200L;
        if (app->conn == LT_APP_ONLINE && strcmp(app->settings.port, st->port) == 0) {
            lt_app_disconnect(app);                 /* free the port the ECU connection holds */
            logf_(st, 0, "Disconnected from the ECU to free %s", st->port);
        }
        err[0] = '\0';
        st->ser = lt_fw_serial_open(st->port, baud, &st->io, err, sizeof(err));
        if (st->ser == NULL) {
            lt_strlcpy(st->ser_err, err, sizeof(st->ser_err));
            open_failed(st, st->port, err);
            return;
        }
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
            open_failed(st, st->dev[di].label, err);
            return;
        }
    }
    memset(&o, 0, sizeof(o));
    o.verify = st->verify && lt_fw_method_verifies(st->method);
    o.leave = st->leave;
    o.keep_settings = st->keep && p->n_protect > 0;
    o.protect = p->protect;
    o.n_protect = p->n_protect;
    o.app_base = is_openblt(st->method) ? p->app_base : 0;
    o.dev_base = p->dev_base;
    o.dev_size = p->dev_size;
    o.can_tx = parse_can_id(st->can_tx);
    o.can_rx = parse_can_id(st->can_rx);
    o.can_kbps = can_rates[st->can_rate];
    logf_(st, 0, "Flashing %s with %s (%s)", st->demo_img ? "the demo image" : lt_path_base(st->path),
          lt_fw_method_name(st->method), what);
    st->job = lt_fw_job_new(st->method, &st->io, &st->img, &o, job_log, st);
    if (st->job == NULL) {
        job_close(st);
        lt_strlcpy(st->result, "cannot start the job", sizeof(st->result));
        return;
    }
    st->running = 1;
}

static void job_finished(lt_app *app, fw_state *st)
{
    lt_fw_job_progress(st->job, &st->prog);
    st->final_phase = st->prog.phase;
    st->usb_err = st->prog.usb_err;
    lt_strlcpy(st->result, st->prog.msg, sizeof(st->result));
    st->finished = 1;
    if (st->sim != NULL && lt_fw_sim_left(st->sim) && !lt_fw_method_serial(st->method)) { st->sim_gone = 1; }
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
    st->method = -1;
    if (app->headless) {
        st->preset = env_int("LT_SHOT_FW_PRESET", st->preset);
        if (st->preset < 0 || st->preset >= LT_FW_PRESET_COUNT) { st->preset = LT_FW_PRESET_GENERIC; }
        st->method = env_int("LT_SHOT_FW_METHOD", -1);
        st->shot_start = env_int("LT_SHOT_FW_START", 0);
        st->shot_wb = env_int("LT_SHOT_FW_WBO", 0);
        st->shot_eacces = env_int("LT_SHOT_FW_EACCES", 0);
    }
    st->verify = 1;
    st->leave = 1;
    st->keep = lt_fw_preset_get(st->preset)->n_protect > 0;
    if (!has_method(lt_fw_preset_get(st->preset), st->method)) { st->method = first_method(lt_fw_preset_get(st->preset)); }
    st->uart_baud = 3;                                      /* 57600: the OpenBLT default */
    st->can_rate = 2;                                       /* 500 kbit/s */
    lt_snprintf(st->can_tx, sizeof(st->can_tx), "0x%03lX", (unsigned long)LT_FW_CAN_TX_ID);
    lt_snprintf(st->can_rx, sizeof(st->can_rx), "0x%03lX", (unsigned long)LT_FW_CAN_RX_ID);
    st->was_demo = app->conn == LT_APP_DEMO;
    load_image(st);
    scan(app, st);
    list_ports(app, st);
    st->next_scan = lt_plat_now_ms() + 1500.0;
    st->next_ports = lt_plat_now_ms() + 2000.0;
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
    if (!has_method(p, st->method)) { st->method = first_method(p); }
    load_image(st);                                         /* base address and checks follow the board */
    st->loaded_preset = st->preset;
}

static const char *preset_note(int preset)
{
    switch (preset) {
    case LT_FW_PRESET_GENERIC:
        return "Raw images go to 0x08000000. Only the flash sectors the image covers are erased.";
    case LT_FW_PRESET_SPEEDUINO_MEGA:
        return "ATmega2560 with the Arduino STK500v2 bootloader over its USB serial port at 115200 baud. Pages "
               "the image covers are rewritten; the bootloader (0x3E000-0x3FFFF) is never touched. Speeduino "
               "keeps its tune in EEPROM, which is not written.";
    case LT_FW_PRESET_TEENSY35:
    case LT_FW_PRESET_TEENSY36:
    case LT_FW_PRESET_TEENSY41:
        return "PJRC's HalfKay bootloader over USB, 1 KB blocks. The first block makes HalfKay erase the program "
               "flash; Speeduino's tune lives in EEPROM outside it, but save the tune to a file first anyway.";
    case LT_FW_PRESET_OPENBLT:
        return "Any OpenBLT bootloader: USB, a UART (XCP with a length byte, no checksum) or CAN through an SLCAN "
               "adapter. OpenBLT refuses writes to its own area; use the app's .srec or .hex.";
    default:
        return "OpenBLT at 0x08000000, rusEFI at 0x08008000. The ECU settings in flash sectors 10-11 "
               "(0x080C0000) are kept unless you untick Keep ECU settings.";
    }
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
        const char *t;
        if (p->url != NULL) {
            t = "Pick rusefi_miata-ecu.bin (OpenBLT + rusEFI, written at 0x08000000), .dfu or .hex from the "
                "Firmware workflow artifacts (Get firmware).";
        } else if (has_method(p, LT_FW_STK500)) {
            t = "Pick the Speeduino firmware .hex for the Mega 2560 (SpeedyLoader keeps the downloaded ones; or "
                "your own build).";
        } else if (has_method(p, LT_FW_HALFKAY)) {
            t = "Pick the Speeduino firmware .hex built for exactly this Teensy model.";
        } else {
            t = "Pick a firmware image: .bin (written at 0x08000000), Intel .hex, .srec or ST .dfu.";
        }
        lt_ui_label_wrap(ui, t, th->muted);
        return;
    }
    lt_snprintf(b, sizeof(b), "%s%s", st->demo_img ? "Demo image (synthetic): " : "", st->img_sum);
    lt_ui_label_wrap(ui, b, th->text);
    if (p->dev_size != 0) {
        if (st->img_fit[0] != '\0') { lt_ui_label_wrap(ui, st->img_fit, th->error); }
        else {
            lt_snprintf(b, sizeof(b), "Fits the %lu KB of %s", (unsigned long)(p->dev_size / 1024u),
                        has_method(p, LT_FW_STK500) ? "application flash below the bootloader" : "flash");
            lt_ui_label_wrap(ui, b, th->success);
        }
        return;
    }
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
    if (is_openblt(st->method) && p->app_base != 0 && st->img.last < p->app_base) {
        lt_ui_label_wrap(ui, "This image holds only the bootloader: nothing to send over OpenBLT.", th->error);
    }
}

/* Linux without permission: show the rule, copy it, or save it to a file and show how to install it */
static void draw_udev(lt_app *app, fw_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    const char *p = lt_fw_udev_rule, *picked = lt_app_file_result(app, FD_FW_UDEV);
    int w3[3], w1 = -1;
    char b[640];
    if (picked != NULL) {
        if (lt_file_write(picked, lt_fw_udev_rule, strlen(lt_fw_udev_rule)) == 0) {
            lt_strlcpy(st->udev_path, picked, sizeof(st->udev_path));
            lt_app_toast(app, 1, "udev rule saved to %s", picked);
        } else {
            lt_app_toast(app, 3, "Cannot write %s: %s", picked, lt_err_get());
        }
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
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
    w3[0] = 140; w3[1] = 170; w3[2] = -1;
    lt_ui_layout_row(ui, 3, w3, 0);
    if (lt_ui_button(ui, "Copy rule")) {
        lt_ui_clipboard_set(ui, lt_fw_udev_rule);
        lt_app_toast(app, 1, "udev rule copied");
    }
    if (lt_ui_button(ui, "Save udev rule...")) {
        char *home = lt_plat_home_dir();
        lt_app_file_dialog(app, FD_FW_UDEV, LT_UI_FILE_SAVE, home, LT_FW_UDEV_FILE, "udev rules|*.rules|All files|*");
        lt_free(home);
    }
    lt_ui_label_ex(ui, st->udev_path[0] != '\0' ? "saved; install it as root:" : "then install it as root:",
                   th->muted, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(b, sizeof(b), "sudo cp \"%s\" /etc/udev/rules.d/" LT_FW_UDEV_FILE,
                st->udev_path[0] != '\0' ? st->udev_path : "$HOME/" LT_FW_UDEV_FILE);
    lt_ui_label_ex(ui, b, th->text, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "sudo udevadm control --reload-rules && sudo udevadm trigger", th->text, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "then unplug and replug the board (LibreTune never runs sudo itself)", th->muted,
                   LT_UI_ALIGN_LEFT);
}

static void draw_usb_hint(lt_app *app, fw_state *st, int code)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int win = lt_plat_os() == LT_OS_WINDOWS, w1 = -1;
    const char *h = lt_fw_usb_hint(code, win);
    if (h == NULL) { return; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, h, th->warning);
    if (code == LT_USB_EACCES && win && st->method == LT_FW_HALFKAY) {
        lt_ui_label_wrap(ui, "Teensy: replacing the HID driver of the bootloader is not recommended; use PJRC's "
                             "Teensy Loader on Windows instead.", th->warning);
    }
    if (code == LT_USB_EACCES && !win) { draw_udev(app, st); }
}

static void draw_hint(lt_app *app, fw_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    const char *t;
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    switch (st->method) {
    case LT_FW_DFUSE:
        t = st->preset == LT_FW_PRESET_MIATA
            ? "First time (or no working firmware): hold BOOT (SW1 on the daughterboard), tap RESET (SW2), "
              "then release BOOT. The board shows up as STM32 BOOTLOADER on its USB-C port."
            : "No working firmware: hold the board's BOOT0 button while you reset or power it, then release it. "
              "The board shows up as STM32 BOOTLOADER.";
        break;
    case LT_FW_STK500:
        t = "Plug in the Mega's USB port and pick its serial port. Start resets the board into its bootloader "
            "(DTR/RTS pulse); the ECU connection is closed first when it uses the same port.";
        break;
    case LT_FW_HALFKAY:
        t = "Press the program button on the Teensy: it shows up as Teensy HalfKay (16c0:0478). Linux: LibreTune "
            "detaches the HID driver itself. Windows: HalfKay is a HID device and this flasher needs the WinUSB "
            "driver, so Teensy flashing does not work on Windows unless the driver is swapped (Zadig) - use PJRC's "
            "Teensy Loader there.";
        break;
    case LT_FW_OPENBLT_UART:
        t = "A USB-UART adapter on the ECU's bootloader UART (8N1; OpenBLT's default is 57600 baud). Use Reboot "
            "into OpenBLT, or reset the ECU and click Start within the bootloader's start-up window.";
        break;
    case LT_FW_OPENBLT_CAN:
        t = "An SLCAN (Lawicel ASCII) adapter on the ECU's CAN bus, e.g. a CANable with slcan firmware or a "
            "USBtin. OpenBLT defaults: 500 kbit/s, 0x667 to the ECU, 0x7E1 from it (append x for 29-bit ids).";
        break;
    default:
        t = "OpenBLT takes the update over the normal USB port without buttons: connect to the ECU and use "
            "Reboot into OpenBLT, or reset the ECU and start within its short bootloader window.";
        break;
    }
    lt_ui_label_wrap(ui, t, th->muted);
}

static void draw_serial(lt_app *app, fw_state *st, int busy)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w3[3], w5[5], sel = port_index(st), win = lt_plat_os() == LT_OS_WINDOWS;
    double now = lt_plat_now_ms();
    if (!busy && now >= st->next_ports) {
        list_ports(app, st);
        st->next_ports = now + 2000.0;
        sel = port_index(st);
    }
    w3[0] = 110; w3[1] = -1; w3[2] = 90;
    lt_ui_layout_row(ui, 3, w3, 0);
    row_label(ui, "Port");
    lt_ui_disabled_begin(ui, busy ? LT_TRUE : LT_FALSE);
    if (st->n_ports > 0) {
        if (sel < 0) { sel = 0; lt_strlcpy(st->port, st->ports[0], sizeof(st->port)); }
        if (lt_ui_combo(ui, "fwport", &sel, st->port_items, st->n_ports) && !busy && sel >= 0 && sel < st->n_ports) {
            lt_strlcpy(st->port, st->ports[sel], sizeof(st->port));
        }
    } else {
        st->port[0] = '\0';
        lt_ui_label_ex(ui, "No serial ports found", th->muted, LT_UI_ALIGN_LEFT);
    }
    if (lt_ui_button(ui, "Refresh") && !busy) {
        list_ports(app, st);
        st->next_ports = now + 2000.0;
    }
    if (st->method == LT_FW_OPENBLT_UART) {
        w3[0] = 110; w3[1] = 160; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        row_label(ui, "Baud rate");
        (void)lt_ui_combo(ui, "fwbaud", &st->uart_baud, uart_baud_names, 6);
        lt_ui_label_ex(ui, "8N1, no flow control", th->muted, LT_UI_ALIGN_LEFT);
    } else if (st->method == LT_FW_OPENBLT_CAN) {
        w5[0] = 110; w5[1] = 160; w5[2] = 120; w5[3] = 120; w5[4] = -1;
        lt_ui_layout_row(ui, 5, w5, 0);
        row_label(ui, "CAN");
        (void)lt_ui_combo(ui, "fwcanrate", &st->can_rate, can_rate_names, 5);
        (void)lt_ui_text(ui, "fwcantx", st->can_tx, sizeof(st->can_tx), 0);
        lt_ui_tooltip(ui, "CAN id the ECU listens to (host -> ECU), hex; OpenBLT default 0x667");
        (void)lt_ui_text(ui, "fwcanrx", st->can_rx, sizeof(st->can_rx), 0);
        lt_ui_tooltip(ui, "CAN id the ECU answers with (ECU -> host), hex; OpenBLT default 0x7E1");
        if (parse_can_id(st->can_tx) == 0 || parse_can_id(st->can_rx) == 0) {
            lt_ui_label_ex(ui, "bad id (hex)", th->error, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_label_ex(ui, "to ECU / from ECU", th->muted, LT_UI_ALIGN_LEFT);
        }
    }
    lt_ui_disabled_end(ui);
    if (st->ser_err[0] != '\0') {
        const char *h = lt_fw_serial_hint(st->ser_err, win);
        int w1 = -1;
        if (h != NULL) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_wrap(ui, h, th->warning);
        }
    }
}

static void draw_device(lt_app *app, fw_state *st, int busy)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    const lt_fw_preset *p = lt_fw_preset_get(st->preset);
    static const char *const want[LT_FW_METHOD_COUNT] = {
        "STM32 BOOTLOADER (0483:df11)", "OpenBLT device (1d50:60ac)", "", "Teensy HalfKay (16c0:0478)", "", ""
    };
    const char *mnames[LT_FW_METHOD_COUNT];
    int mlist[LT_FW_METHOD_COUNT];
    int w3[3], di, online = is_online(app), nm = 0, m, msel = 0;
    char b[240];
    double now = lt_plat_now_ms();
    for (m = 0; m < LT_FW_METHOD_COUNT; m++) {
        if (!has_method(p, m)) { continue; }
        if (m == st->method) { msel = nm; }
        mlist[nm] = m;
        mnames[nm++] = lt_fw_method_name(m);
    }
    w3[0] = 110; w3[1] = -1; w3[2] = 1;
    lt_ui_layout_row(ui, 3, w3, 0);
    row_label(ui, "Method");
    lt_ui_disabled_begin(ui, busy || nm < 2 ? LT_TRUE : LT_FALSE);
    if (lt_ui_combo(ui, "fwmethod", &msel, mnames, nm) && !busy && msel >= 0 && msel < nm) {
        st->method = mlist[msel];
        st->ser_err[0] = '\0';
    }
    lt_ui_disabled_end(ui);
    lt_ui_label(ui, "");
    if (lt_fw_method_serial(st->method)) {
        draw_serial(app, st, busy);
    } else {
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
            lt_snprintf(b, sizeof(b), "Waiting for the %s to appear... %d s",
                        st->method == LT_FW_DFUSE ? "STM32 BOOTLOADER" : "OpenBLT device",
                        (int)((st->wait_until - now) / 1000.0) + 1);
            lt_ui_label_ex(ui, b, th->accent, LT_UI_ALIGN_LEFT);
        } else if (st->scan_rc == LT_USB_ENOTSUP) {
            lt_ui_label_ex(ui, "USB access is not available in this build", th->warning, LT_UI_ALIGN_LEFT);
        } else if (st->scan_rc < 0) {
            lt_snprintf(b, sizeof(b), "Cannot list USB devices: %s", lt_usb_strerror(st->scan_rc));
            lt_ui_label_ex(ui, b, th->warning, LT_UI_ALIGN_LEFT);
        } else {
            lt_snprintf(b, sizeof(b), "No %s found", want[st->method]);
            lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
        }
        if (lt_ui_button_ex(ui, "Scan", busy ? LT_UI_BTN_DISABLED : 0) && !busy) {
            scan(app, st);
            st->next_scan = now + 1500.0;
        }
        if (st->scan_rc < 0 && st->scan_rc != LT_USB_ENOTSUP) { draw_usb_hint(app, st, st->scan_rc); }
        if (st->scan_rc == LT_USB_ENOTSUP && !st->sim_mode) { draw_usb_hint(app, st, LT_USB_ENOTSUP); }
    }
    /* reboot into the bootloader through the connection */
    di = lt_fw_method_serial(st->method) ? -1 : find_dev(st, st->method);
    if (online && di < 0 && !st->waiting && (st->method == LT_FW_DFUSE || is_openblt(st->method))) {
        const lt_ini_cmd *c = reboot_cmd(app, st->method);
        if (c != NULL || app->conn == LT_APP_DEMO) {
            int w2[2];
            w2[0] = 220; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            if (lt_ui_button_ex(ui, st->method == LT_FW_DFUSE ? "Reboot ECU into DFU" : "Reboot into OpenBLT",
                                busy ? LT_UI_BTN_DISABLED : 0) && !busy) {
                st->reboot_confirm = 1;
            }
            lt_snprintf(b, sizeof(b), lt_fw_method_serial(st->method) ? "sends %s; then click Start"
                                                                       : "sends %s, then waits for the bootloader",
                        c != NULL ? c->name : "a demo reboot");
            lt_ui_label_ex(ui, b, th->muted, LT_UI_ALIGN_LEFT);
        }
    }
    draw_hint(app, st);
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
    const char *preset_names[LT_FW_PRESET_COUNT];
    int open = d->open, w1 = -1, w2[2], wb[4], busy, fh, di, ready, ps, i, serial, can_verify;
    if (!st->init) { init_state(app, st, d->target); }
    for (i = 0; i < LT_FW_PRESET_COUNT; i++) { preset_names[i] = lt_fw_preset_get(i)->name; }
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
            lt_ui_label_wrap(ui, preset_note(st->preset), th->muted);
            draw_image(app, st, busy);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_separator(ui);
            draw_device(app, st, st->running);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_separator(ui);
            can_verify = lt_fw_method_verifies(st->method);
            lt_ui_disabled_begin(ui, st->running || !can_verify ? LT_TRUE : LT_FALSE);
            if (can_verify) { lt_ui_checkbox(ui, "Verify after writing (read the flash back)", &st->verify); }
            else {
                int off = 0;
                lt_ui_checkbox(ui, "Verify after writing (HalfKay cannot read the flash back)", &off);
            }
            lt_ui_disabled_end(ui);
            lt_ui_disabled_begin(ui, st->running ? LT_TRUE : LT_FALSE);
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
        serial = lt_fw_method_serial(st->method);
        di = serial ? -1 : find_dev(st, st->method);
        ready = st->img_ok && (serial ? st->port[0] != '\0' : di >= 0) && !st->running && !st->waiting &&
                st->img_fit[0] == '\0' &&
                !(is_openblt(st->method) && p->app_base != 0 && st->img.last < p->app_base) &&
                !(st->method == LT_FW_OPENBLT_CAN && (parse_can_id(st->can_tx) == 0 || parse_can_id(st->can_rx) == 0));
        wb[0] = -1; wb[1] = 120; wb[2] = 100; wb[3] = 110;
        lt_ui_layout_row(ui, 4, wb, 0);
        if (st->running) { lt_ui_label_ex(ui, "Engine off. Do not unplug the cable or the power.", th->warning, LT_UI_ALIGN_LEFT); }
        else if (st->finished && st->final_phase == LT_FW_PH_DONE) { lt_ui_label_ex(ui, "Firmware updated", th->success, LT_UI_ALIGN_LEFT); }
        else if (!st->img_ok) { lt_ui_label_ex(ui, "Pick an image file", th->muted, LT_UI_ALIGN_LEFT); }
        else if (serial && st->port[0] == '\0') { lt_ui_label_ex(ui, "Pick a serial port", th->muted, LT_UI_ALIGN_LEFT); }
        else if (!serial && di < 0) { lt_ui_label_ex(ui, "No bootloader device", th->muted, LT_UI_ALIGN_LEFT); }
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
        if (lt_fw_method_serial(st->method)) { st->shot_start = 0; start_job(app, st); }
        else if (find_dev(st, st->method) >= 0 && !st->waiting) { st->shot_start = 0; start_job(app, st); }
        else if (is_online(app) && !st->waiting) { do_reboot(app, st); }
    }
    if (st->shot_wb && app->headless && is_online(app)) {
        st->shot_wb = 0;
        wb_start(app, st);
    }
    if (st->confirm) {
        char m[480];
        int r;
        lt_snprintf(m, sizeof(m), "Write %s to the ECU with %s?\n\nThe engine must be off. Do not unplug the cable "
                    "or the ECU power until it finishes.%s",
                    st->demo_img ? "the demo image" : lt_path_base(st->path), lt_fw_method_name(st->method),
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
