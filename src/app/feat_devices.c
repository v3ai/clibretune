/* feat_devices.c - the ECUs this computer has talked to (devices.conf in the configuration
 * folder: last port, baud rate, signature and project per USB serial number), a cached list
 * of the serial ports for the start screen and reconnect (refreshed at most once a second,
 * no port is opened), and the one-click "Connect to my ECU" flow.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "ports_logic.h"

typedef struct dev_state {
    lt_ecuport_mem mem;
    int mem_loaded;
    lt_ecuport *ports;
    int n_ports, n_hidden;
    double t_list;
    unsigned long gen;               /* changes whenever the list of ports changes */
} dev_state;

static void dev_cleanup(lt_app *app, void *p)
{
    dev_state *s = (dev_state *)p;
    LT_UNUSED(app);
    lt_ecuport_mem_free(&s->mem);
    lt_free(s->ports);
    s->ports = NULL;
}

static dev_state *dev(lt_app *app)
{
    dev_state *s = (dev_state *)lt_ext_state(app, "devices", sizeof(dev_state), dev_cleanup);
    if (!s->mem_loaded) {
        s->mem_loaded = 1;
        lt_ecuport_mem_init(&s->mem);
        if (app->config_dir != NULL) { (void)lt_ecuport_mem_load(&s->mem, app->config_dir); }
    }
    return s;
}

const lt_ecuport_mem *lt_feat_devices_mem(lt_app *app)
{
    return &dev(app)->mem;
}

static int same_ports(const lt_ecuport *a, int na, const lt_ecuport *b, int nb)
{
    int i;
    if (na != nb) { return 0; }
    for (i = 0; i < na; i++) {
        if (strcmp(a[i].info.name, b[i].info.name) != 0 || a[i].info.vid != b[i].info.vid ||
            a[i].info.pid != b[i].info.pid || strcmp(a[i].info.serial, b[i].info.serial) != 0) {
            return 0;
        }
    }
    return 1;
}

int lt_feat_devices_ports(lt_app *app, const lt_ecuport **out, int *n_hidden)
{
    dev_state *s = dev(app);
    double now = lt_plat_now_ms();
    if (s->t_list == 0.0 || now - s->t_list >= 1000.0 || now < s->t_list) {
        lt_ecuport *p = NULL;
        int nh = 0, n = lt_ecuport_list(0, &s->mem, &p, &nh);
        if (!same_ports(p, n, s->ports, s->n_ports)) {
            s->gen++;
            if (app->ui != NULL) { lt_ui_request_redraw(app->ui); }
        }
        lt_free(s->ports);
        s->ports = p;
        s->n_ports = n;
        s->n_hidden = nh;
        s->t_list = now;
    }
    if (out != NULL) { *out = s->ports; }
    if (n_hidden != NULL) { *n_hidden = s->n_hidden; }
    return s->n_ports;
}

unsigned long lt_feat_devices_generation(lt_app *app)
{
    return dev(app)->gen;
}

/* the identity of a port right now (show_all: legacy ports too); 0 when not listed */
int lt_feat_devices_info(lt_app *app, const char *port, lt_serial_info *out)
{
    lt_serial_info *l = NULL;
    int n = lt_serial_list_info(&l), i, found = 0;
    LT_UNUSED(app);
    for (i = 0; i < n; i++) {
        if (strcmp(l[i].name, port) == 0) { *out = l[i]; found = 1; break; }
    }
    lt_free(l);
    return found;
}

/* remember the device behind a good connection */
static void on_connect(lt_app *app)
{
    dev_state *s = dev(app);
    lt_serial_info in;
    if (app->conn != LT_APP_ONLINE || app->settings.port[0] == '\0' ||
        strncmp(app->settings.port, "tcp:", 4) == 0) {
        return;
    }
    if (!lt_feat_devices_info(app, app->settings.port, &in)) {
        memset(&in, 0, sizeof(in));
        lt_strlcpy(in.name, app->settings.port, sizeof(in.name));
    }
    lt_ecuport_mem_note(&s->mem, &in, (long)app->settings.baud, app->signature,
                        app->has_project ? app->project.path : "");
    if (app->config_dir != NULL && !app->headless) { (void)lt_ecuport_mem_save(&s->mem, app->config_dir); }
    s->t_list = 0.0;                 /* "used before" marks change */
}

static int project_exists(const char *dir)
{
    char *p;
    int ok;
    if (dir == NULL || dir[0] == '\0') { return 0; }
    p = lt_path_join(dir, "project.json");
    ok = p != NULL && lt_file_exists(p);
    lt_free(p);
    return ok;
}

int lt_feat_devices_detected(lt_app *app, lt_ecuport *port, char *project, size_t pcap)
{
    const lt_ecuport *p = NULL;
    int n = lt_feat_devices_ports(app, &p, NULL), best = lt_ecuport_best_ecu(p, n);
    if (project != NULL && pcap > 0) { project[0] = '\0'; }
    if (best < 0) { return 0; }
    if (port != NULL) { *port = p[best]; }
    if (project != NULL && p[best].remembered) {
        const lt_ecuport_seen *seen = lt_ecuport_mem_find(&dev(app)->mem, &p[best].info);
        if (seen != NULL && project_exists(seen->project)) { lt_strlcpy(project, seen->project, pcap); }
    }
    return 1;
}

void lt_feat_devices_connect(lt_app *app)
{
    lt_ecuport port;
    char project[512];
    if (app->ini == NULL && lt_feat_devices_detected(app, &port, project, sizeof(project)) &&
        project[0] != '\0') {
        /* this ECU was used with a project before: open it and connect */
        if (lt_app_open_project(app, project) == 0) {
            long baud = port.bauds[0] > 0 ? port.bauds[0] : 115200L;
            if (lt_app_connect_serial(app, port.info.name, baud) != 0) { (void)lt_app_connect_dialog(app); }
        }
        return;
    }
    if (app->ini != NULL) { (void)lt_app_connect_dialog(app); return; }
    (void)lt_app_dialog_push(app, "connect_wizard", "", "Connect ECU");
}

void lt_feat_devices_register(lt_app_ext *reg)
{
    lt_ext_add_on_connect(reg, on_connect);
}
