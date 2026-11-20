/* test_ports.c - finding the ECU: port labels and ranking, remembered devices, the
 * non-blocking probe (scripted devices, fake clock), the advice texts, and the Linux sysfs
 * lookup on a fake tree.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/ports_logic.h"

static lt_serial_info mk(const char *name, int usb, lt_u16 vid, lt_u16 pid, const char *product,
                         const char *serial)
{
    lt_serial_info s;
    memset(&s, 0, sizeof(s));
    lt_strlcpy(s.name, name, sizeof(s.name));
    s.usb = usb;
    s.vid = vid;
    s.pid = pid;
    lt_strlcpy(s.product, product, sizeof(s.product));
    lt_strlcpy(s.serial, serial, sizeof(s.serial));
    return s;
}

LT_TEST(test_describe)
{
    lt_serial_info s;
    lt_ecuport d;
    s = mk("/dev/ttyACM0", 1, 0x0483, 0x5740, "", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_STM32);
    LT_CHECK_STR(d.ids, "0483:5740");
    LT_CHECK_INT(d.n_bauds, 1);
    LT_CHECK(d.settle_ms < 500);
    LT_CHECK(strstr(d.label, "rusEFI") != NULL);
    LT_CHECK_STR(lt_ecuport_family(d.kind), "rusEFI / FOME / epicEFI");
    /* a product string naming the firmware wins over the table */
    s = mk("/dev/ttyACM0", 1, 0x0483, 0x5740, "FOME ECU", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_STR(d.label, "FOME ECU");
    /* Arduino Mega: Speeduino, one baud rate, waits for the reset */
    s = mk("/dev/ttyACM1", 1, 0x2341, 0x0042, "Arduino Mega 2560", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_ARDUINO);
    LT_CHECK(d.bauds[0] == 115200L && d.n_bauds == 1);
    LT_CHECK(d.settle_ms >= 1500);
    LT_CHECK(strstr(d.label, "Speeduino") != NULL);
    /* CH340 bridge: several baud rates, 115200 first */
    s = mk("/dev/ttyUSB0", 1, 0x1a86, 0x7523, "USB2.0-Serial", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_ADAPTER);
    LT_CHECK(d.n_bauds >= 3 && d.bauds[0] == 115200L);
    LT_CHECK_STR(d.label, "CH340 USB-serial");
    /* other ids */
    s = mk("/dev/ttyUSB1", 1, 0x0403, 0x6015, "", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_ADAPTER);
    s = mk("COM4", 1, 0x10c4, 0xea60, "", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_ADAPTER);
    s = mk("/dev/ttyACM3", 1, 0x16c0, 0x0483, "", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_TEENSY);
    /* unknown CDC device: named by its product */
    s = mk("/dev/ttyACM4", 1, 0x1234, 0x5678, "Widget", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_CDC);
    LT_CHECK_STR(d.label, "Widget");
    /* legacy UARTs */
    s = mk("/dev/ttyS0", 0, 0, 0, "", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_LEGACY);
    s = mk("COM1", 0, 0, 0, "", "");
    lt_strlcpy(s.driver, "Serial", sizeof(s.driver));
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_LEGACY);
    /* a COM port Windows told us nothing about is not hidden */
    s = mk("COM7", 0, 0, 0, "", "");
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_OTHER);
    /* macOS names */
    s = mk("/dev/cu.usbmodem14101", 1, 0, 0, "", "");
    lt_strlcpy(s.driver, "cdc", sizeof(s.driver));
    lt_ecuport_describe(&s, &d);
    LT_CHECK_INT(d.kind, LT_ECUPORT_CDC);
}

LT_TEST(test_rank)
{
    lt_serial_info in[7];
    lt_ecuport *p = NULL;
    int n, hidden = -1;
    in[0] = mk("/dev/ttyS0", 0, 0, 0, "", "");
    in[1] = mk("/dev/ttyS1", 0, 0, 0, "", "");
    in[2] = mk("/dev/ttyUSB0", 1, 0x1a86, 0x7523, "", "");
    in[3] = mk("/dev/ttyACM10", 1, 0x1234, 0x0001, "", "");
    in[4] = mk("/dev/ttyACM2", 1, 0x1234, 0x0002, "", "");
    in[5] = mk("/dev/ttyACM0", 1, 0x0483, 0x5740, "", "ABC123");
    in[6] = mk("/dev/ttyS2", 0, 0, 0, "", "");
    n = lt_ecuport_rank(in, 7, 0, NULL, &p, &hidden);
    LT_CHECK_INT(n, 4);
    LT_CHECK_INT(hidden, 3);
    if (n == 4) {
        LT_CHECK_STR(p[0].info.name, "/dev/ttyACM0");     /* known ECU */
        LT_CHECK_STR(p[1].info.name, "/dev/ttyACM2");     /* CDC, numeric order */
        LT_CHECK_STR(p[2].info.name, "/dev/ttyACM10");
        LT_CHECK_STR(p[3].info.name, "/dev/ttyUSB0");     /* bridge */
        LT_CHECK_INT(lt_ecuport_best_ecu(p, n), 0);
    }
    lt_free(p);
    n = lt_ecuport_rank(in, 7, 1, NULL, &p, &hidden);
    LT_CHECK_INT(n, 7);
    LT_CHECK_INT(hidden, 0);
    if (n == 7) {
        LT_CHECK_STR(p[4].info.name, "/dev/ttyS0");
        LT_CHECK_STR(p[6].info.name, "/dev/ttyS2");
    }
    lt_free(p);
    /* nothing but legacy ports */
    n = lt_ecuport_rank(in, 2, 0, NULL, &p, &hidden);
    LT_CHECK_INT(n, 0);
    LT_CHECK(p == NULL);
    LT_CHECK_INT(hidden, 2);
    /* macOS: the tty.* twin of a cu.* device is dropped */
    in[0] = mk("/dev/cu.usbmodem1", 1, 0, 0, "", "");
    in[1] = mk("/dev/tty.usbmodem1", 1, 0, 0, "", "");
    n = lt_ecuport_rank(in, 2, 0, NULL, &p, &hidden);
    LT_CHECK_INT(n, 1);
    LT_CHECK(n == 1 && strcmp(p[0].info.name, "/dev/cu.usbmodem1") == 0);
    lt_free(p);
}

LT_TEST(test_memory)
{
    lt_ecuport_mem m, m2;
    lt_serial_info a, b, c, in[3];
    lt_ecuport *p = NULL;
    const lt_ecuport_seen *s;
    char key[112];
    int n, i;
    a = mk("/dev/ttyUSB0", 1, 0x0403, 0x6001, "", "FT1234");
    b = mk("/dev/ttyACM0", 1, 0x2341, 0x0042, "", "");
    c = mk("/dev/ttyUSB3", 1, 0x0403, 0x6001, "", "FT1234");   /* same device, new name */
    lt_ecuport_key(&a, key, sizeof(key));
    LT_CHECK_STR(key, "usb:0403:6001:FT1234");
    lt_ecuport_key(&b, key, sizeof(key));
    LT_CHECK_STR(key, "port:/dev/ttyACM0");
    lt_ecuport_mem_init(&m);
    LT_CHECK(lt_ecuport_mem_find(&m, &a) == NULL);
    lt_ecuport_mem_note(&m, &b, 115200L, "speeduino 202402", "/p/one");
    lt_ecuport_mem_note(&m, &a, 57600L, "MS2Extra comms342|x", "/p/two");
    LT_CHECK_INT(m.n, 2);
    s = lt_ecuport_mem_find(&m, &c);
    LT_CHECK(s != NULL && s->baud == 57600L);
    LT_CHECK(s != NULL && strcmp(s->project, "/p/two") == 0);
    /* a connection without a project keeps the project link */
    lt_ecuport_mem_note(&m, &c, 57600L, "MS2Extra comms342", "");
    LT_CHECK_INT(m.n, 2);
    s = lt_ecuport_mem_find(&m, &a);
    LT_CHECK(s != NULL && strcmp(s->project, "/p/two") == 0 && strcmp(s->port, "/dev/ttyUSB3") == 0);
    /* persisted */
    lt_plat_mkdirs("build/test_ports_cfg");
    LT_CHECK(lt_ecuport_mem_save(&m, "build/test_ports_cfg") == 0);
    lt_ecuport_mem_init(&m2);
    LT_CHECK(lt_ecuport_mem_load(&m2, "build/test_ports_cfg") == 0);
    LT_CHECK_INT(m2.n, 2);
    s = lt_ecuport_mem_find(&m2, &b);
    LT_CHECK(s != NULL && s->baud == 115200L && strcmp(s->signature, "speeduino 202402") == 0);
    s = lt_ecuport_mem_find(&m2, &a);
    LT_CHECK(s != NULL && strcmp(s->project, "/p/two") == 0);
    /* a remembered device goes first, at its baud rate */
    in[0] = mk("/dev/ttyACM5", 1, 0x0483, 0x5740, "", "");
    in[1] = c;
    in[2] = mk("/dev/ttyUSB9", 1, 0x1a86, 0x7523, "", "");
    n = lt_ecuport_rank(in, 3, 0, &m2, &p, NULL);
    LT_CHECK_INT(n, 3);
    if (n == 3) {
        LT_CHECK_STR(p[0].info.name, "/dev/ttyUSB3");
        LT_CHECK(p[0].remembered);
        LT_CHECK(p[0].bauds[0] == 57600L);
        for (i = 1; i < p[0].n_bauds; i++) { LT_CHECK(p[0].bauds[i] != 57600L); }
        LT_CHECK_INT(lt_ecuport_best_ecu(p, n), 0);
    }
    lt_free(p);
    /* at most 32 devices are kept, newest first */
    for (i = 0; i < 40; i++) {
        char nm[32];
        lt_snprintf(nm, sizeof(nm), "/dev/ttyX%d", i);
        a = mk(nm, 0, 0, 0, "", "");
        lt_ecuport_mem_note(&m2, &a, 9600L, "", "");
    }
    LT_CHECK_INT(m2.n, 32);
    LT_CHECK_STR(m2.e[0].port, "/dev/ttyX39");
    lt_ecuport_mem_free(&m);
    lt_ecuport_mem_free(&m2);
    /* a missing file is an empty memory */
    LT_CHECK(lt_ecuport_mem_load(&m, "build/test_ports_none") == 0);
    LT_CHECK_INT(m.n, 0);
    lt_ecuport_mem_free(&m);
}

/* ---- scripted devices for the probe ---- */
typedef struct fake_dev {
    const char *name;
    int open_err;              /* LT_SERIAL_E* or 0 */
    long baud;                 /* the only baud rate it answers at (0 = any) */
    const char *reply_q, *reply_s;   /* NULL = silent */
    int garbage;               /* answers noise at any other baud rate */
    int opens, writes, blocking_reads;
    /* the open session */
    long cur_baud;
    char pending[64];
    size_t np;
} fake_dev;

typedef struct fake_bus { fake_dev *d; int n; double *clock; } fake_bus;

static int f_read(void *ud, void *buf, int n, int timeout_ms)
{
    fake_dev *d = (fake_dev *)ud;
    int k = (int)d->np < n ? (int)d->np : n;
    if (timeout_ms != 0) { d->blocking_reads++; }
    memcpy(buf, d->pending, (size_t)k);
    memmove(d->pending, d->pending + k, d->np - (size_t)k);
    d->np -= (size_t)k;
    return k;
}

static int f_write(void *ud, const void *buf, int n)
{
    fake_dev *d = (fake_dev *)ud;
    char q = ((const char *)buf)[0];
    const char *r = NULL;
    d->writes++;
    if (d->baud == 0 || d->cur_baud == d->baud) { r = q == 'Q' ? d->reply_q : d->reply_s; }
    else if (d->garbage) { r = "\x8f\x13\xfe\x02\x81\x90\x11\x07\xe0"; }
    if (r != NULL) {
        size_t l = strlen(r);
        if (l > sizeof(d->pending) - d->np) { l = sizeof(d->pending) - d->np; }
        memcpy(d->pending + d->np, r, l);
        d->np += l;
    }
    return n;
}

static void f_close(void *ud) { ((fake_dev *)ud)->np = 0; }

static int f_open(void *ud, const char *port, long baud, lt_proto_io *io)
{
    fake_bus *b = (fake_bus *)ud;
    int i;
    for (i = 0; i < b->n; i++) {
        fake_dev *d = &b->d[i];
        if (strcmp(d->name, port) == 0) {
            d->opens++;
            if (d->open_err != 0) { return d->open_err; }
            d->cur_baud = baud;
            d->np = 0;
            memset(io, 0, sizeof(*io));
            io->ud = d;
            io->read = f_read;
            io->write = f_write;
            io->close = f_close;
            return 0;
        }
    }
    return LT_SERIAL_EGONE;
}

/* step until done; returns the result; *elapsed gets the fake time used */
static int run_scan(lt_ecuport_scan *s, double *elapsed)
{
    double t = 1000.0;
    int r = LT_ECUSCAN_RUNNING, guard = 0;
    while (r == LT_ECUSCAN_RUNNING && guard++ < 100000) {
        r = lt_ecuport_scan_step(s, t);
        t += 10.0;
    }
    if (elapsed != NULL) { *elapsed = t - 1000.0; }
    return r;
}

LT_TEST(test_scan)
{
    fake_dev devs[4];
    fake_bus bus;
    lt_serial_info in[4];
    lt_ecuport *p = NULL;
    lt_ecuport_scan *s;
    const lt_ecuport_hit *h;
    double took = 0.0;
    char title[96], msg[400], st[200];
    int n, r, i;

    memset(devs, 0, sizeof(devs));
    devs[0].name = "/dev/ttyACM0"; devs[0].open_err = LT_SERIAL_EPERM;
    devs[1].name = "/dev/ttyACM1";                                      /* silent CDC */
    devs[2].name = "/dev/ttyUSB0"; devs[2].baud = 57600L; devs[2].garbage = 1;
    devs[2].reply_q = "MS2Extra comms342h2";
    bus.d = devs; bus.n = 3; bus.clock = NULL;
    in[0] = mk("/dev/ttyACM0", 1, 0x0483, 0x5740, "", "");
    in[1] = mk("/dev/ttyACM1", 1, 0x1234, 0x0001, "", "");
    in[2] = mk("/dev/ttyUSB0", 1, 0x0403, 0x6001, "", "");
    n = lt_ecuport_rank(in, 3, 0, NULL, &p, NULL);
    LT_CHECK_INT(n, 3);
    s = lt_ecuport_scan_new(p, n);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    lt_ecuport_scan_status(s, st, sizeof(st));
    LT_CHECK(strstr(st, "/dev/ttyACM0") != NULL);
    r = run_scan(s, &took);
    LT_CHECK_INT(r, LT_ECUSCAN_FOUND);
    h = lt_ecuport_scan_hit(s);
    LT_CHECK(h != NULL);
    if (h != NULL) {
        LT_CHECK_STR(h->port, "/dev/ttyUSB0");
        LT_CHECK_STR(h->signature, "MS2Extra comms342h2");
        LT_CHECK(h->baud == 57600L);
        LT_CHECK_INT(h->index, 2);
    }
    LT_CHECK_INT(devs[0].opens, 1);          /* permission: not retried at other baud rates */
    LT_CHECK_INT(devs[1].opens, 1);          /* CDC: one baud rate */
    LT_CHECK_INT(devs[2].opens, 2);          /* 115200 (noise), then 57600 */
    for (i = 0; i < 3; i++) { LT_CHECK_INT(devs[i].blocking_reads, 0); }
    LT_CHECK(took < 6000.0);
    LT_CHECK(lt_ecuport_scan_progress(s) >= 1.0);
    lt_ecuport_scan_free(s);

    /* nothing found: the permission problem is what the user is told about, with the fix */
    devs[2].reply_q = NULL; devs[2].garbage = 0;
    s = lt_ecuport_scan_new(p, n);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    r = run_scan(s, NULL);
    LT_CHECK_INT(r, LT_ECUSCAN_DONE);
    LT_CHECK(lt_ecuport_scan_hit(s) == NULL);
    lt_ecuport_scan_report(s, 0, title, sizeof(title), msg, sizeof(msg));
    LT_CHECK(strstr(title, "permission") != NULL);
    LT_CHECK(strstr(msg, "/dev/ttyACM0") != NULL);
    if (lt_plat_os() == LT_OS_LINUX) { LT_CHECK(strstr(msg, "usermod -aG") != NULL); }
    lt_ecuport_scan_free(s);

    /* busy port */
    devs[0].open_err = LT_SERIAL_EBUSY;
    s = lt_ecuport_scan_new(p, n);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    (void)run_scan(s, NULL);
    lt_ecuport_scan_report(s, 0, title, sizeof(title), msg, sizeof(msg));
    LT_CHECK(strstr(title, "in use") != NULL);
    LT_CHECK(strstr(msg, "TunerStudio") != NULL);
    lt_ecuport_scan_free(s);

    /* the known ECU is silent: check power and firmware */
    devs[0].open_err = 0;
    s = lt_ecuport_scan_new(p, n);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    (void)run_scan(s, NULL);
    lt_ecuport_scan_report(s, 0, title, sizeof(title), msg, sizeof(msg));
    LT_CHECK(strstr(title, "did not answer") != NULL);
    LT_CHECK(strstr(msg, "powered") != NULL);
    lt_ecuport_scan_free(s);

    /* noise at every speed: wrong baud rate advice */
    devs[2].garbage = 1; devs[2].baud = 1L;
    s = lt_ecuport_scan_new(p + 1, 2);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    (void)run_scan(s, NULL);
    lt_ecuport_scan_report(s, 0, title, sizeof(title), msg, sizeof(msg));
    LT_CHECK(strstr(msg, "baud rate") != NULL);
    lt_ecuport_scan_free(s);

    /* bootloader answer */
    devs[1].reply_q = "\xE0\x01>";
    s = lt_ecuport_scan_new(p + 1, 1);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    (void)run_scan(s, NULL);
    lt_ecuport_scan_report(s, 0, title, sizeof(title), msg, sizeof(msg));
    LT_CHECK(strstr(title, "bootloader") != NULL);
    lt_ecuport_scan_free(s);

    /* no ports at all */
    s = lt_ecuport_scan_new(NULL, 0);
    LT_CHECK_INT(lt_ecuport_scan_step(s, 0.0), LT_ECUSCAN_DONE);
    lt_ecuport_scan_report(s, 4, title, sizeof(title), msg, sizeof(msg));
    LT_CHECK(strstr(title, "No USB") != NULL);
    LT_CHECK(strstr(msg, "Show all ports") != NULL);
    lt_ecuport_scan_free(s);

    /* cancel closes the port and ends the scan */
    devs[1].reply_q = NULL;
    s = lt_ecuport_scan_new(p + 1, 1);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    LT_CHECK_INT(lt_ecuport_scan_step(s, 0.0), LT_ECUSCAN_RUNNING);
    lt_ecuport_scan_cancel(s);
    LT_CHECK_INT(lt_ecuport_scan_step(s, 10.0), LT_ECUSCAN_DONE);
    lt_ecuport_scan_free(s);
    lt_free(p);

    /* a fast CDC ECU is found in well under a second of probing */
    memset(devs, 0, sizeof(devs));
    devs[0].name = "/dev/ttyACM0"; devs[0].reply_q = "rusEFI master.2025.01.01.f407-discovery.123";
    bus.n = 1;
    in[0] = mk("/dev/ttyACM0", 1, 0x0483, 0x5740, "", "");
    n = lt_ecuport_rank(in, 1, 0, NULL, &p, NULL);
    s = lt_ecuport_scan_new(p, n);
    lt_ecuport_scan_set_open(s, f_open, &bus);
    r = run_scan(s, &took);
    LT_CHECK_INT(r, LT_ECUSCAN_FOUND);
    LT_CHECK(took < 500.0);
    lt_ecuport_scan_free(s);
    lt_free(p);
}

LT_TEST(test_advice)
{
    char msg[400];
    lt_ecuport_open_advice(LT_SERIAL_EBUSY, "COM3", msg, sizeof(msg));
    LT_CHECK(strstr(msg, "COM3 is in use") != NULL);
    lt_ecuport_open_advice(LT_SERIAL_EGONE, "/dev/ttyACM0", msg, sizeof(msg));
    LT_CHECK(strstr(msg, "unplugged") != NULL);
    lt_ecuport_open_advice(LT_SERIAL_EPERM, "/dev/ttyACM0", msg, sizeof(msg));
    LT_CHECK(strstr(msg, "not allowed") != NULL);
}

/* ---- Linux: USB identity from a fake sysfs tree ---- */
static void put(const char *root, const char *rel, const char *val)
{
    char *p = lt_path_join(root, rel);
    char *d = lt_strdup(p);
    char *sl = strrchr(d, '/');
    if (sl != NULL) { *sl = '\0'; }
    lt_plat_mkdirs(d);
    if (val != NULL) { LT_CHECK(lt_file_write(p, val, strlen(val)) == 0); }
    else { lt_plat_mkdirs(p); }
    lt_free(d);
    lt_free(p);
}

LT_TEST(test_linux_sysfs)
{
    const char *root = "build/test_ports_sysfs";
    lt_serial_info *l = NULL;
    int n, i, acm = -1, usb = -1;
    if (lt_plat_os() != LT_OS_LINUX) { return; }
    /* an STM32 board on cdc_acm (interface dir has tty/ttyACM7) */
    put(root, "3-1/idVendor", "0483\n");
    put(root, "3-1/idProduct", "5740\n");
    put(root, "3-1/manufacturer", "rusEFI\n");
    put(root, "3-1/product", "rusEFI ECU\n");
    put(root, "3-1/serial", "3A0047000851\n");
    put(root, "3-1:1.0/bInterfaceClass", "02\n");
    put(root, "3-1:1.1/tty/ttyACM7", NULL);
    /* an FTDI cable on ftdi_sio (interface dir has ttyUSB5/) */
    put(root, "3-2/idVendor", "0403\n");
    put(root, "3-2/idProduct", "6001\n");
    put(root, "3-2/serial", "A50285BI\n");
    put(root, "3-2:1.0/ttyUSB5/tty/ttyUSB5", NULL);
    LT_CHECK(lt_plat_setenv("LT_USB_SYSFS_ROOT", root) == 0);
    n = lt_serial_list_info(&l);
    for (i = 0; i < n; i++) {
        if (strcmp(l[i].name, "/dev/ttyACM7") == 0) { acm = i; }
        if (strcmp(l[i].name, "/dev/ttyUSB5") == 0) { usb = i; }
    }
    LT_CHECK(acm >= 0);
    LT_CHECK(usb >= 0);
    if (acm >= 0) {
        LT_CHECK(l[acm].usb);
        LT_CHECK_INT(l[acm].vid, 0x0483);
        LT_CHECK_INT(l[acm].pid, 0x5740);
        LT_CHECK_STR(l[acm].product, "rusEFI ECU");
        LT_CHECK_STR(l[acm].serial, "3A0047000851");
        LT_CHECK_STR(l[acm].driver, "cdc_acm");
    }
    if (usb >= 0) {
        LT_CHECK_INT(l[usb].vid, 0x0403);
        LT_CHECK_STR(l[usb].serial, "A50285BI");
        LT_CHECK_STR(l[usb].product, "");
    }
    lt_free(l);
    LT_CHECK(lt_plat_setenv("LT_USB_SYSFS_ROOT", "") == 0);
}

/* the same device after a replug, maybe under another name */
LT_TEST(test_find_same)
{
    lt_serial_info in[3], dev;
    lt_ecuport *p = NULL;
    int n;
    in[0] = mk("/dev/ttyUSB0", 1, 0x0403, 0x6001, "", "FT1");
    in[1] = mk("/dev/ttyACM1", 1, 0x0483, 0x5740, "rusEFI ECU", "3A00");
    in[2] = mk("/dev/ttyACM2", 1, 0x0483, 0x5740, "", "");
    n = lt_ecuport_rank(in, 3, 1, NULL, &p, NULL);
    dev = mk("/dev/ttyACM0", 1, 0x0483, 0x5740, "rusEFI ECU", "3A00");     /* was ttyACM0 */
    LT_CHECK_STR(p[lt_ecuport_find_same(p, n, &dev)].info.name, "/dev/ttyACM1");
    dev = mk("/dev/ttyACM0", 1, 0x0483, 0x5740, "rusEFI ECU", "OTHER");    /* another board */
    LT_CHECK_INT(lt_ecuport_find_same(p, n, &dev), -1);
    dev = mk("/dev/ttyACM2", 1, 0x0483, 0x5740, "", "");                   /* no serial: by name */
    LT_CHECK_STR(p[lt_ecuport_find_same(p, n, &dev)].info.name, "/dev/ttyACM2");
    dev = mk("/dev/ttyACM9", 1, 0x0483, 0x5740, "", "");
    LT_CHECK_INT(lt_ecuport_find_same(p, n, &dev), -1);
    lt_free(p);
}

int main(void)
{
    LT_RUN(test_describe);
    LT_RUN(test_rank);
    LT_RUN(test_memory);
    LT_RUN(test_scan);
    LT_RUN(test_advice);
    LT_RUN(test_linux_sysfs);
    LT_RUN(test_find_same);
    LT_TEST_MAIN_END
}
