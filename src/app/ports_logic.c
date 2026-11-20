/* ports_logic.c - port ranking, labels, remembered devices, non-blocking ECU probe
 * (see ports_logic.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ports_logic.h"
#include "../project/project.h"

/* ------------------------------------------------------------------ */
/* known USB ids                                                       */
typedef struct usb_known {
    lt_u16 vid, pid;
    int kind;
    const char *label;
} usb_known;

static const usb_known g_known[] = {
    { 0x0483, 0x5740, LT_ECUPORT_STM32, "STM32 USB (rusEFI, FOME, epicEFI)" },
    { 0x2341, 0x0042, LT_ECUPORT_ARDUINO, "Arduino Mega 2560 (Speeduino)" },
    { 0x2341, 0x0010, LT_ECUPORT_ARDUINO, "Arduino Mega 2560 (Speeduino)" },
    { 0x2A03, 0x0042, LT_ECUPORT_ARDUINO, "Arduino Mega 2560 (Speeduino)" },
    { 0x2A03, 0x0010, LT_ECUPORT_ARDUINO, "Arduino Mega 2560 (Speeduino)" },
    { 0x16C0, 0x0483, LT_ECUPORT_TEENSY, "Teensy (Speeduino)" },
    { 0x1A86, 0x7523, LT_ECUPORT_ADAPTER, "CH340 USB-serial" },
    { 0x1A86, 0x5523, LT_ECUPORT_ADAPTER, "CH341 USB-serial" },
    { 0x1A86, 0x55D4, LT_ECUPORT_ADAPTER, "CH9102 USB-serial" },
    { 0x0403, 0x6001, LT_ECUPORT_ADAPTER, "FTDI USB-serial" },
    { 0x0403, 0x6010, LT_ECUPORT_ADAPTER, "FTDI USB-serial" },
    { 0x0403, 0x6014, LT_ECUPORT_ADAPTER, "FTDI USB-serial" },
    { 0x0403, 0x6015, LT_ECUPORT_ADAPTER, "FTDI USB-serial" },
    { 0x10C4, 0xEA60, LT_ECUPORT_ADAPTER, "CP210x USB-serial" },
    { 0x067B, 0x2303, LT_ECUPORT_ADAPTER, "Prolific USB-serial" }
};

static const long bauds_cdc[] = { 115200L };
static const long bauds_adapter[] = { 115200L, 57600L, 38400L, 9600L };
static const long bauds_all[] = { 115200L, 921600L, 460800L, 230400L, 57600L, 9600L };

static int kind_score(int kind)
{
    switch (kind) {
    case LT_ECUPORT_STM32:
    case LT_ECUPORT_ARDUINO:
    case LT_ECUPORT_TEENSY:  return 500;
    case LT_ECUPORT_CDC:     return 300;
    case LT_ECUPORT_ADAPTER: return 250;
    case LT_ECUPORT_USB:     return 200;
    case LT_ECUPORT_OTHER:   return 100;
    default:                 return 0;
    }
}

const char *lt_ecuport_family(int kind)
{
    switch (kind) {
    case LT_ECUPORT_STM32:   return "rusEFI / FOME / epicEFI";
    case LT_ECUPORT_ARDUINO:
    case LT_ECUPORT_TEENSY:  return "Speeduino";
    default:                 return "";
    }
}

static void set_bauds(lt_ecuport *p, const long *b, int n)
{
    int i;
    p->n_bauds = n < LT_ECUPORT_MAX_BAUDS ? n : LT_ECUPORT_MAX_BAUDS;
    for (i = 0; i < p->n_bauds; i++) { p->bauds[i] = b[i]; }
}

/* a product string that names the firmware is better than any table entry */
static int product_names_ecu(const char *s)
{
    static const char *const words[] = { "rusefi", "fome", "epicefi", "speeduino", "megasquirt",
                                         "microsquirt", "ecu" };
    size_t i, k, n = strlen(s);
    for (i = 0; i < LT_COUNTOF(words); i++) {
        size_t w = strlen(words[i]);
        for (k = 0; k + w <= n; k++) {
            if (lt_strnicmp(s + k, words[i], w) == 0) { return 1; }
        }
    }
    return 0;
}

static int is_legacy_name(const lt_serial_info *in)
{
    const char *b = lt_path_base(in->name);
    if (in->usb) { return 0; }
    if (lt_str_starts(in->name, "/dev/ttyS") || lt_str_starts(b, "ttyS")) { return 1; }
    if (lt_stricmp(in->driver, "Serial") == 0) { return 1; }           /* Windows on-board UART */
    if (lt_str_starts(b, "tty.") && strstr(b, "Bluetooth") != NULL) { return 1; }
    return 0;
}

void lt_ecuport_describe(const lt_serial_info *in, lt_ecuport *out)
{
    size_t i;
    memset(out, 0, sizeof(*out));
    out->info = *in;
    out->kind = in->usb ? LT_ECUPORT_USB : LT_ECUPORT_OTHER;
    out->settle_ms = 1500;
    set_bauds(out, bauds_all, (int)LT_COUNTOF(bauds_all));
    if (in->vid != 0 || in->pid != 0) {
        lt_snprintf(out->ids, sizeof(out->ids), "%04x:%04x", (unsigned)in->vid, (unsigned)in->pid);
    }
    for (i = 0; i < LT_COUNTOF(g_known); i++) {
        if (g_known[i].vid == in->vid && g_known[i].pid == in->pid) {
            out->kind = g_known[i].kind;
            lt_strlcpy(out->label, g_known[i].label, sizeof(out->label));
            break;
        }
    }
    if (i == LT_COUNTOF(g_known) && in->usb) {
        const char *b = lt_path_base(in->name);
        if (strcmp(in->driver, "cdc_acm") == 0 || strcmp(in->driver, "cdc") == 0 ||
            lt_stricmp(in->driver, "usbser") == 0 || lt_str_starts(b, "ttyACM") ||
            strstr(b, "usbmodem") != NULL) {
            out->kind = LT_ECUPORT_CDC;
        }
    }
    if (is_legacy_name(in)) { out->kind = LT_ECUPORT_LEGACY; }
    switch (out->kind) {
    case LT_ECUPORT_STM32:
    case LT_ECUPORT_TEENSY:
    case LT_ECUPORT_CDC:
        /* USB CDC: the baud rate is not used on the wire, nothing resets on open */
        set_bauds(out, bauds_cdc, 1);
        out->settle_ms = 150;
        break;
    case LT_ECUPORT_ARDUINO:
        set_bauds(out, bauds_cdc, 1);   /* Speeduino always runs at 115200 */
        out->settle_ms = 1600;          /* opening resets the Mega: bootloader, then firmware */
        break;
    case LT_ECUPORT_ADAPTER:
        set_bauds(out, bauds_adapter, (int)LT_COUNTOF(bauds_adapter));
        break;
    default:
        break;
    }
    if (in->product[0] != '\0' && (out->label[0] == '\0' || product_names_ecu(in->product))) {
        lt_strlcpy(out->label, in->product, sizeof(out->label));
    }
    if (out->label[0] == '\0') {
        switch (out->kind) {
        case LT_ECUPORT_CDC:    lt_strlcpy(out->label, "USB serial device", sizeof(out->label)); break;
        case LT_ECUPORT_USB:    lt_strlcpy(out->label, "USB serial device", sizeof(out->label)); break;
        case LT_ECUPORT_LEGACY: lt_strlcpy(out->label, "Built-in serial port", sizeof(out->label)); break;
        default:                lt_strlcpy(out->label, "Serial port", sizeof(out->label)); break;
        }
    }
    out->score = kind_score(out->kind);
}

void lt_ecuport_key(const lt_serial_info *in, char *key, size_t cap)
{
    if (in->serial[0] != '\0' && (in->vid != 0 || in->pid != 0)) {
        lt_snprintf(key, cap, "usb:%04x:%04x:%s", (unsigned)in->vid, (unsigned)in->pid, in->serial);
    } else {
        lt_snprintf(key, cap, "port:%s", in->name);
    }
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
#define MEM_MAX 32

void lt_ecuport_mem_init(lt_ecuport_mem *m)
{
    m->e = NULL;
    m->n = 0;
}

void lt_ecuport_mem_free(lt_ecuport_mem *m)
{
    lt_free(m->e);
    lt_ecuport_mem_init(m);
}

static char *mem_path(const char *dir)
{
    return dir != NULL ? lt_path_join(dir, "devices.conf") : NULL;
}

/* one field of "a|b|c" (fields never contain '|': it is replaced when saving) */
static void field(const char *s, int idx, char *out, size_t cap)
{
    int k = 0;
    size_t n = 0;
    while (*s != '\0' && k < idx) { if (*s == '|') { k++; } s++; }
    if (k < idx) { out[0] = '\0'; return; }
    while (s[n] != '\0' && s[n] != '|') { n++; }
    if (n >= cap) { n = cap - 1; }
    memcpy(out, s, n);
    out[n] = '\0';
}

int lt_ecuport_mem_load(lt_ecuport_mem *m, const char *config_dir)
{
    lt_props p;
    char *path = mem_path(config_dir);
    char key[24], b[24];
    int i;
    lt_ecuport_mem_free(m);
    if (path == NULL) { return -1; }
    lt_props_init(&p);
    if (lt_file_exists(path) && lt_props_load(&p, path) == 0) {
        m->e = (lt_ecuport_seen *)lt_calloc(MEM_MAX, sizeof(lt_ecuport_seen));
        for (i = 0; i < MEM_MAX; i++) {
            const char *k, *v;
            lt_ecuport_seen *s;
            lt_snprintf(key, sizeof(key), "dev%d.key", i);
            k = lt_props_get(&p, key);
            lt_snprintf(key, sizeof(key), "dev%d.val", i);
            v = lt_props_get(&p, key);
            if (k == NULL || v == NULL || k[0] == '\0') { continue; }
            s = &m->e[m->n++];
            lt_strlcpy(s->key, k, sizeof(s->key));
            field(v, 0, b, sizeof(b));
            s->baud = atol(b);
            field(v, 1, s->port, sizeof(s->port));
            field(v, 2, s->signature, sizeof(s->signature));
            field(v, 3, s->project, sizeof(s->project));
        }
    }
    lt_props_free(&p);
    lt_free(path);
    return 0;
}

static void no_bars(char *s)
{
    for (; *s != '\0'; s++) { if (*s == '|' || *s == '\n' || *s == '\r') { *s = ' '; } }
}

int lt_ecuport_mem_save(const lt_ecuport_mem *m, const char *config_dir)
{
    lt_props p;
    char *path = mem_path(config_dir);
    char key[24], *val;
    int i, rc;
    size_t cap = 900;
    if (path == NULL) { return -1; }
    val = (char *)lt_malloc(cap);
    lt_props_init(&p);
    for (i = 0; i < m->n; i++) {
        const lt_ecuport_seen *s = &m->e[i];
        lt_snprintf(key, sizeof(key), "dev%d.key", i);
        lt_props_set(&p, key, s->key);
        lt_snprintf(val, cap, "%ld|%s|%s|%s", s->baud, s->port, s->signature, s->project);
        lt_snprintf(key, sizeof(key), "dev%d.val", i);
        lt_props_set(&p, key, val);
    }
    rc = lt_props_save(&p, path);
    lt_props_free(&p);
    lt_free(val);
    lt_free(path);
    return rc;
}

const lt_ecuport_seen *lt_ecuport_mem_find(const lt_ecuport_mem *m, const lt_serial_info *in)
{
    char key[112];
    int i;
    if (m == NULL) { return NULL; }
    lt_ecuport_key(in, key, sizeof(key));
    for (i = 0; i < m->n; i++) {
        if (strcmp(m->e[i].key, key) == 0) { return &m->e[i]; }
    }
    return NULL;
}

void lt_ecuport_mem_note(lt_ecuport_mem *m, const lt_serial_info *in, long baud,
                         const char *signature, const char *project)
{
    lt_ecuport_seen s;
    int i, at = -1;
    memset(&s, 0, sizeof(s));
    lt_ecuport_key(in, s.key, sizeof(s.key));
    s.baud = baud;
    lt_strlcpy(s.port, in->name, sizeof(s.port));
    lt_strlcpy(s.signature, signature != NULL ? signature : "", sizeof(s.signature));
    lt_strlcpy(s.project, project != NULL ? project : "", sizeof(s.project));
    no_bars(s.port); no_bars(s.signature); no_bars(s.project);
    if (m->e == NULL) { m->e = (lt_ecuport_seen *)lt_calloc(MEM_MAX, sizeof(lt_ecuport_seen)); }
    for (i = 0; i < m->n; i++) {
        if (strcmp(m->e[i].key, s.key) == 0) { at = i; break; }
    }
    if (at >= 0 && s.project[0] == '\0') {   /* a connection without a project keeps the link */
        lt_strlcpy(s.project, m->e[at].project, sizeof(s.project));
    }
    if (at < 0) { at = m->n < MEM_MAX ? m->n++ : MEM_MAX - 1; }
    for (i = at; i > 0; i--) { m->e[i] = m->e[i - 1]; }
    m->e[0] = s;
}

/* ------------------------------------------------------------------ */
/* ranking                                                             */
/* trailing number of a port name ("ttyACM10" -> 10), -1 if none */
static long name_num(const char *s)
{
    size_t n = strlen(s), k = n;
    while (k > 0 && s[k - 1] >= '0' && s[k - 1] <= '9') { k--; }
    if (k == n || n - k > 6) { return -1; }
    return atol(s + k);
}

static int cmp_ports(const void *pa, const void *pb)
{
    const lt_ecuport *a = (const lt_ecuport *)pa, *b = (const lt_ecuport *)pb;
    size_t la, lb;
    long na, nb;
    int c;
    if (a->score != b->score) { return a->score > b->score ? -1 : 1; }
    /* same family of names in numeric order: ttyACM2 before ttyACM10, COM3 before COM12 */
    la = strlen(a->info.name);
    lb = strlen(b->info.name);
    while (la > 0 && a->info.name[la - 1] >= '0' && a->info.name[la - 1] <= '9') { la--; }
    while (lb > 0 && b->info.name[lb - 1] >= '0' && b->info.name[lb - 1] <= '9') { lb--; }
    if (la == lb && strncmp(a->info.name, b->info.name, la) == 0) {
        na = name_num(a->info.name);
        nb = name_num(b->info.name);
        if (na != nb) { return na < nb ? -1 : 1; }
    }
    c = strcmp(a->info.name, b->info.name);
    return (c > 0) - (c < 0);
}

/* macOS lists each device twice (cu.* and tty.*): keep the callout one */
static int mac_duplicate(const lt_serial_info *in, int n, const lt_serial_info *all)
{
    const char *b = lt_path_base(in->name);
    int i;
    if (!lt_str_starts(b, "tty.")) { return 0; }
    for (i = 0; i < n; i++) {
        const char *o = lt_path_base(all[i].name);
        if (lt_str_starts(o, "cu.") && strcmp(o + 3, b + 4) == 0) { return 1; }
    }
    return 0;
}

int lt_ecuport_rank(const lt_serial_info *in, int n, int show_all, const lt_ecuport_mem *mem,
                    lt_ecuport **out, int *n_hidden)
{
    lt_ecuport *a;
    int i, k = 0, hidden = 0;
    *out = NULL;
    if (n_hidden != NULL) { *n_hidden = 0; }
    if (n <= 0) { return 0; }
    a = (lt_ecuport *)lt_calloc((size_t)n, sizeof(lt_ecuport));
    for (i = 0; i < n; i++) {
        const lt_ecuport_seen *seen;
        lt_ecuport_describe(&in[i], &a[k]);
        if (!show_all && (a[k].kind == LT_ECUPORT_LEGACY || mac_duplicate(&in[i], n, in))) {
            hidden++;
            continue;
        }
        seen = lt_ecuport_mem_find(mem, &in[i]);
        if (seen != NULL) {
            int j, at = -1;
            a[k].remembered = 1;
            a[k].score += 1000;
            for (j = 0; j < a[k].n_bauds; j++) { if (a[k].bauds[j] == seen->baud) { at = j; } }
            if (seen->baud > 0) {
                if (at < 0) {   /* not in the list: put it in front, drop the last */
                    at = a[k].n_bauds < LT_ECUPORT_MAX_BAUDS ? a[k].n_bauds++ : a[k].n_bauds - 1;
                }
                for (j = at; j > 0; j--) { a[k].bauds[j] = a[k].bauds[j - 1]; }
                a[k].bauds[0] = seen->baud;
            }
        }
        k++;
    }
    if (n_hidden != NULL) { *n_hidden = hidden; }
    if (k == 0) { lt_free(a); return 0; }
    qsort(a, (size_t)k, sizeof(lt_ecuport), cmp_ports);
    *out = a;
    return k;
}

int lt_ecuport_list(int show_all, const lt_ecuport_mem *mem, lt_ecuport **out, int *n_hidden)
{
    lt_serial_info *info = NULL;
    int n = lt_serial_list_info(&info), k;
    if (n < 0) { n = 0; }
    k = lt_ecuport_rank(info, n, show_all, mem, out, n_hidden);
    lt_free(info);
    return k;
}

int lt_ecuport_best_ecu(const lt_ecuport *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (p[i].remembered || p[i].kind == LT_ECUPORT_STM32 || p[i].kind == LT_ECUPORT_ARDUINO ||
            p[i].kind == LT_ECUPORT_TEENSY || product_names_ecu(p[i].info.product)) {
            return i;
        }
    }
    return -1;
}

int lt_ecuport_find_same(const lt_ecuport *p, int n, const lt_serial_info *dev)
{
    int i;
    for (i = 0; i < n; i++) {
        const lt_serial_info *s = &p[i].info;
        if (dev->serial[0] != '\0' && (dev->vid != 0 || dev->pid != 0)) {
            if (s->vid == dev->vid && s->pid == dev->pid && strcmp(s->serial, dev->serial) == 0) { return i; }
        } else if (strcmp(s->name, dev->name) == 0 && s->vid == dev->vid && s->pid == dev->pid) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
enum { ST_OPEN = 0, ST_SETTLE, ST_SEND, ST_READ, ST_DONE };
enum { RES_NONE = 0, RES_SILENT, RES_GARBAGE, RES_ECHO, RES_BOOT, RES_GPS };

typedef struct port_out {
    int open_err;              /* LT_SERIAL_E* of the last failed open, 0 none */
    int opened, answered, garbage, boot, gone;
} port_out;

struct lt_ecuport_scan {
    lt_ecuport *ports;
    port_out *res;
    int n, pi, bi, qi, st, result, cancelled;
    lt_ecuport_open_fn open_fn;
    void *open_ud;
    lt_proto_io io;
    int io_open;
    double t_open, t_sent, t_last;
    lt_u8 buf[64];
    size_t got;
    lt_ecuport_hit hit;
};

static int default_open(void *ud, const char *port, long baud, lt_proto_io *io)
{
    lt_stream *s;
    LT_UNUSED(ud);
    s = lt_serial_open(port, baud);
    if (s == NULL) {
        int e = lt_serial_last_error();
        return e != LT_SERIAL_EOK ? e : LT_SERIAL_EOTHER;
    }
    *io = lt_proto_io_from_stream(s, LT_TRUE);
    return 0;
}

lt_ecuport_scan *lt_ecuport_scan_new(const lt_ecuport *ports, int n)
{
    lt_ecuport_scan *s = (lt_ecuport_scan *)lt_calloc(1, sizeof(*s));
    if (n < 0) { n = 0; }
    s->n = n;
    if (n > 0) {
        s->ports = (lt_ecuport *)lt_calloc((size_t)n, sizeof(lt_ecuport));
        memcpy(s->ports, ports, (size_t)n * sizeof(lt_ecuport));
        s->res = (port_out *)lt_calloc((size_t)n, sizeof(port_out));
    }
    s->open_fn = default_open;
    s->st = n > 0 ? ST_OPEN : ST_DONE;
    s->result = n > 0 ? LT_ECUSCAN_RUNNING : LT_ECUSCAN_DONE;
    return s;
}

static void io_close(lt_ecuport_scan *s)
{
    if (s->io_open && s->io.close != NULL) { s->io.close(s->io.ud); }
    s->io_open = 0;
    memset(&s->io, 0, sizeof(s->io));
}

void lt_ecuport_scan_free(lt_ecuport_scan *s)
{
    if (s == NULL) { return; }
    io_close(s);
    lt_free(s->ports);
    lt_free(s->res);
    lt_free(s);
}

void lt_ecuport_scan_set_open(lt_ecuport_scan *s, lt_ecuport_open_fn fn, void *ud)
{
    s->open_fn = fn != NULL ? fn : default_open;
    s->open_ud = ud;
}

void lt_ecuport_scan_cancel(lt_ecuport_scan *s)
{
    io_close(s);
    s->cancelled = 1;
    s->st = ST_DONE;
    if (s->result == LT_ECUSCAN_RUNNING) { s->result = LT_ECUSCAN_DONE; }
}

static void next_port(lt_ecuport_scan *s)
{
    io_close(s);
    s->pi++;
    s->bi = 0;
    s->qi = 0;
    s->st = s->pi < s->n ? ST_OPEN : ST_DONE;
    if (s->st == ST_DONE) { s->result = LT_ECUSCAN_DONE; }
}

static void next_baud(lt_ecuport_scan *s)
{
    io_close(s);
    s->bi++;
    s->qi = 0;
    if (s->bi >= s->ports[s->pi].n_bauds) { next_port(s); return; }
    s->st = ST_OPEN;
}

static void drain(lt_ecuport_scan *s)
{
    lt_u8 tmp[128];
    int g = 0;
    if (s->io.clear_input != NULL) { s->io.clear_input(s->io.ud); return; }
    while (g++ < 64 && s->io.read(s->io.ud, tmp, (int)sizeof(tmp), 0) > 0) { }
}

/* a reply is complete: classify it and move on */
static void finish_query(lt_ecuport_scan *s, double now)
{
    static const char qs[2] = { 'Q', 'S' };
    port_out *o = &s->res[s->pi];
    char sig[96];
    lt_disc_kind k;
    LT_UNUSED(now);
    if (s->got == 0) {
        s->qi++;
        if (s->qi >= 2) { next_baud(s); } else { s->st = ST_SEND; }
        return;
    }
    o->answered = 1;
    k = lt_disc_classify(s->buf, s->got, qs[s->qi], sig, sizeof(sig));
    switch (k) {
    case LT_DISC_SIGNATURE:
        lt_strlcpy(s->hit.port, s->ports[s->pi].info.name, sizeof(s->hit.port));
        lt_strlcpy(s->hit.signature, sig, sizeof(s->hit.signature));
        s->hit.baud = s->ports[s->pi].bauds[s->bi];
        s->hit.index = s->pi;
        io_close(s);
        s->st = ST_DONE;
        s->result = LT_ECUSCAN_FOUND;
        return;
    case LT_DISC_BOOTLOADER:
        o->boot = 1;
        next_port(s);
        return;
    case LT_DISC_GPS:
        next_port(s);
        return;
    case LT_DISC_ECHO:
        s->qi++;
        if (s->qi >= 2) { next_baud(s); } else { s->st = ST_SEND; }
        return;
    default:
        o->garbage = 1;        /* noise at this speed: try the next baud rate */
        next_baud(s);
        return;
    }
}

int lt_ecuport_scan_step(lt_ecuport_scan *s, double now)
{
    static const char qs[2] = { 'Q', 'S' };
    int guard;
    for (guard = 0; guard < 16 && s->st != ST_DONE; guard++) {
        lt_ecuport *p = &s->ports[s->pi];
        port_out *o = &s->res[s->pi];
        if (s->st == ST_OPEN) {
            int rc;
            memset(&s->io, 0, sizeof(s->io));
            rc = s->open_fn(s->open_ud, p->info.name, p->bauds[s->bi], &s->io);
            if (rc != 0) {
                o->open_err = rc;
                if (rc == LT_SERIAL_EGONE) { o->gone = 1; }
                next_port(s);               /* the reason does not depend on the baud rate */
                continue;
            }
            s->io_open = 1;
            o->opened = 1;
            s->t_open = now;
            s->st = ST_SETTLE;
            continue;
        }
        if (s->st == ST_SETTLE) {
            if (now - s->t_open < (double)(s->bi == 0 ? p->settle_ms : LT_MIN(p->settle_ms, 1500))) {
                return s->result;
            }
            drain(s);
            s->st = ST_SEND;
            continue;
        }
        if (s->st == ST_SEND) {
            lt_u8 q = (lt_u8)qs[s->qi];
            drain(s);
            s->got = 0;
            if (s->io.write(s->io.ud, &q, 1) != 1) {
                o->gone = 1;
                next_port(s);
                continue;
            }
            if (s->io.flush != NULL) { s->io.flush(s->io.ud); }
            s->t_sent = now;
            s->t_last = now;
            s->st = ST_READ;
            continue;
        }
        if (s->st == ST_READ) {
            int r = 0;
            /* a USB CDC ECU answers within milliseconds; bridges at low baud rates take longer */
            double window = (p->kind == LT_ECUPORT_STM32 || p->kind == LT_ECUPORT_TEENSY) ? 400.0 : 700.0;
            if (s->got < sizeof(s->buf)) {
                r = s->io.read(s->io.ud, s->buf + s->got, (int)(sizeof(s->buf) - s->got), 0);
            }
            if (r < 0) {
                o->gone = 1;
                next_port(s);
                continue;
            }
            if (r > 0) {
                s->got += (size_t)r;
                s->t_last = now;
                if (s->got >= 32 || s->got >= sizeof(s->buf)) { finish_query(s, now); continue; }
                return s->result;
            }
            if ((s->got > 0 && now - s->t_last >= 40.0) || now - s->t_sent >= window) {
                finish_query(s, now);
                continue;
            }
            return s->result;
        }
    }
    return s->result;
}

double lt_ecuport_scan_progress(const lt_ecuport_scan *s)
{
    double per;
    const lt_ecuport *p;
    if (s->n == 0 || s->st == ST_DONE) { return 1.0; }
    p = &s->ports[s->pi];
    per = p->n_bauds > 0 ? (double)s->bi / (double)p->n_bauds : 0.0;
    return ((double)s->pi + per) / (double)s->n;
}

void lt_ecuport_scan_status(const lt_ecuport_scan *s, char *buf, size_t cap)
{
    const lt_ecuport *p;
    if (s->result == LT_ECUSCAN_FOUND) {
        lt_snprintf(buf, cap, "Found %s on %s", s->hit.signature, s->hit.port);
        return;
    }
    if (s->st == ST_DONE || s->n == 0) {
        lt_snprintf(buf, cap, "%s", s->cancelled ? "Search stopped" : "Search finished");
        return;
    }
    p = &s->ports[s->pi];
    if (p->n_bauds > 1) {
        lt_snprintf(buf, cap, "Asking %s on %s at %ld baud", p->label, p->info.name, p->bauds[s->bi]);
    } else {
        lt_snprintf(buf, cap, "Asking %s on %s", p->label, p->info.name);
    }
}

const lt_ecuport_hit *lt_ecuport_scan_hit(const lt_ecuport_scan *s)
{
    return s->result == LT_ECUSCAN_FOUND ? &s->hit : NULL;
}

void lt_ecuport_open_advice(int code, const char *port, char *msg, size_t cap)
{
    char cmd[96];
    switch (code) {
    case LT_SERIAL_EPERM:
        if (lt_serial_perm_fix(cmd, sizeof(cmd))) {
            lt_snprintf(msg, cap, "Your user is not allowed to open %s. Run this command once, then log "
                        "out and back in (or restart):\n    %s", port, cmd);
        } else {
            lt_snprintf(msg, cap, "Your user is not allowed to open %s.", port);
        }
        break;
    case LT_SERIAL_EBUSY:
        lt_snprintf(msg, cap, "%s is in use by another program. Close TunerStudio, the Arduino IDE "
                    "serial monitor or any other program that talks to the ECU, then try again.", port);
        break;
    case LT_SERIAL_EGONE:
        lt_snprintf(msg, cap, "%s is gone: the device was unplugged or restarted. Check the USB cable "
                    "and try again.", port);
        break;
    default:
        lt_snprintf(msg, cap, "%s could not be opened.", port);
        break;
    }
}

void lt_ecuport_scan_report(const lt_ecuport_scan *s, int n_hidden, char *title, size_t tcap,
                            char *msg, size_t mcap)
{
    int i, perm = -1, busy = -1, boot = -1, garbage = -1, gone = -1, silent_ecu = -1, opened = 0;
    for (i = 0; i < s->n; i++) {
        const port_out *o = &s->res[i];
        if (o->open_err == LT_SERIAL_EPERM && perm < 0) { perm = i; }
        if (o->open_err == LT_SERIAL_EBUSY && busy < 0) { busy = i; }
        if (o->boot && boot < 0) { boot = i; }
        if (o->garbage && !o->boot && garbage < 0) { garbage = i; }
        if (o->gone && gone < 0) { gone = i; }
        if (o->opened) { opened++; }
        if (o->opened && !o->answered && silent_ecu < 0 && lt_ecuport_family(s->ports[i].kind)[0] != '\0') {
            silent_ecu = i;
        }
    }
    if (s->cancelled) {
        lt_snprintf(title, tcap, "Search stopped");
        lt_snprintf(msg, mcap, "Start it again when the ECU is plugged in and powered.");
    } else if (s->n == 0) {
        lt_snprintf(title, tcap, "No USB serial device found");
        lt_snprintf(msg, mcap, "Plug the ECU into a USB port and switch the ignition on. Some USB cables "
                    "only charge: if nothing shows up, try another cable.%s",
                    n_hidden > 0 ? " Built-in serial ports are hidden; tick Show all ports to try them." : "");
    } else if (perm >= 0) {
        lt_snprintf(title, tcap, "No permission to open the port");
        lt_ecuport_open_advice(LT_SERIAL_EPERM, s->ports[perm].info.name, msg, mcap);
    } else if (busy >= 0) {
        lt_snprintf(title, tcap, "The port is in use");
        lt_ecuport_open_advice(LT_SERIAL_EBUSY, s->ports[busy].info.name, msg, mcap);
    } else if (boot >= 0) {
        lt_snprintf(title, tcap, "The ECU is in bootloader mode");
        lt_snprintf(msg, mcap, "The ECU on %s is waiting for a firmware update. Finish it in Tools > "
                    "Firmware Update, or power-cycle the ECU to start its normal firmware.",
                    s->ports[boot].info.name);
    } else if (gone >= 0 && opened == 0) {
        lt_snprintf(title, tcap, "The device went away");
        lt_ecuport_open_advice(LT_SERIAL_EGONE, s->ports[gone].info.name, msg, mcap);
    } else if (garbage >= 0) {
        lt_snprintf(title, tcap, "No ECU answer at the tried speeds");
        lt_snprintf(msg, mcap, "%s on %s sent data, but not an ECU reply at any baud rate tried. If it is "
                    "an ECU, choose the port and baud rate by hand (Manual connection).",
                    s->ports[garbage].label, s->ports[garbage].info.name);
    } else if (silent_ecu >= 0) {
        lt_snprintf(title, tcap, "The ECU did not answer");
        lt_snprintf(msg, mcap, "%s on %s did not answer. Check that the ECU is powered (ignition on) and "
                    "running its normal firmware; after flashing, power-cycle it once.",
                    s->ports[silent_ecu].label, s->ports[silent_ecu].info.name);
    } else {
        lt_snprintf(title, tcap, "No ECU answered");
        lt_snprintf(msg, mcap, "%d port%s tried, none answered like an ECU. Check the ECU's power and the "
                    "cable, and that the firmware is running.%s", s->n, s->n == 1 ? "" : "s",
                    n_hidden > 0 ? " Tick Show all ports to also try the built-in serial ports." : "");
    }
}
