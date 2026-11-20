/* os_linux.c - Linux-specific platform pieces (selected by the build system).
 *
 * PLATFORM FILE: uses POSIX/Linux headers (time.h clock_gettime, unistd.h
 * readlink). Built only when TARGET=linux.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <grp.h>

lt_os lt_plat_os(void)
{
    return LT_OS_LINUX;
}

double lt_plat_os_now_ms(void)
{
    struct timespec ts;
    double t = 0.0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        t = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    }
    return lt_posix_clamp_time(t);
}

static char *config_dir_os(void)
{
    return lt_posix_xdg_config_dir();
}

char *lt_plat_config_dir(void)
{
    char *o = lt_plat_config_override();
    return o != NULL ? o : config_dir_os();
}

char *lt_plat_exe_path(void)
{
    char buf[1024];
    ssize_t r = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (r <= 0) return NULL;
    buf[r] = '\0';
    return lt_strdup(buf);
}

int lt_plat_open_url(const char *url)
{
    return lt_posix_spawn_open("xdg-open", url);
}

int lt_serial_list(char ***names)
{
    static const char *const prefixes[] = { "ttyUSB", "ttyACM" };
    static const char *const fixed[] = { "ttyS0", "ttyS1", "ttyS2", "ttyS3" };
    return lt_posix_serial_scan(prefixes, LT_COUNTOF(prefixes), fixed, LT_COUNTOF(fixed), names);
}

/* ---- serial port identity from sysfs ------------------------------------- */
/* USB serial interfaces appear in /sys/bus/usb/devices as "B-P[.P]:C.I" entries whose
 * directory holds "tty/ttyACM<n>" (cdc_acm) or "ttyUSB<n>" (usb-serial drivers: ftdi_sio,
 * ch341, cp210x, pl2303); the USB device itself is "B-P[.P]" with idVendor, idProduct,
 * manufacturer, product and serial. Only plain directory listings and attribute reads are
 * needed, so a fake tree ($LT_USB_SYSFS_ROOT) works in tests. */
static void sysfs_attr(const char *root, const char *dev, const char *name, char *buf,
                       size_t cap)
{
    char *d = lt_path_join(root, dev);
    char *p = d != NULL ? lt_path_join(d, name) : NULL;
    FILE *f = p != NULL ? fopen(p, "r") : NULL;
    size_t n;
    buf[0] = '\0';
    if (f != NULL) {
        if (fgets(buf, (int)cap, f) == NULL) buf[0] = '\0';
        fclose(f);
    }
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    lt_free(p);
    lt_free(d);
}

static lt_u16 sysfs_hex(const char *root, const char *dev, const char *name)
{
    char buf[16];
    char *end = NULL;
    unsigned long v;
    sysfs_attr(root, dev, name, buf, sizeof buf);
    if (buf[0] == '\0') return 0;
    v = strtoul(buf, &end, 16);
    return (end != NULL && *end == '\0' && v <= 0xFFFFUL) ? (lt_u16)v : 0;
}

static int info_find(const lt_serial_info *a, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(a[i].name, name) == 0) return i;
    }
    return -1;
}

static int info_add(lt_serial_info **a, int n, int *cap, const char *name)
{
    if (n >= *cap) {
        int nc = *cap > 0 ? *cap * 2 : 8;
        *a = (lt_serial_info *)lt_realloc(*a, (size_t)nc * sizeof(lt_serial_info));
        *cap = nc;
    }
    memset(&(*a)[n], 0, sizeof(lt_serial_info));
    lt_strlcpy((*a)[n].name, name, sizeof((*a)[n].name));
    return n + 1;
}

/* the tty names under one interface directory (at most 4) */
static int iface_ttys(const char *root, const char *iface, char found[4][32])
{
    char *dir = lt_path_join(root, iface);
    char **sub = NULL;
    int ns, i, nf = 0;
    ns = dir != NULL ? lt_plat_list_dir(dir, &sub) : -1;
    for (i = 0; i < ns && nf < 4; i++) {
        if (strcmp(sub[i], "tty") == 0) {
            char *td = lt_path_join(dir, "tty");
            char **tt = NULL;
            int nt = td != NULL ? lt_plat_list_dir(td, &tt) : -1, k;
            for (k = 0; k < nt && nf < 4; k++) {
                if (lt_str_starts(tt[k], "tty")) lt_strlcpy(found[nf++], tt[k], 32);
            }
            if (nt > 0) lt_plat_free_list(tt, nt);
            lt_free(td);
        } else if (lt_str_starts(sub[i], "ttyUSB") || lt_str_starts(sub[i], "ttyACM")) {
            lt_strlcpy(found[nf++], sub[i], 32);
        }
    }
    if (ns > 0) lt_plat_free_list(sub, ns);
    lt_free(dir);
    return nf;
}

int lt_serial_list_info(lt_serial_info **out)
{
    const char *root = getenv("LT_USB_SYSFS_ROOT");
    char **names = NULL, **ents = NULL;
    lt_serial_info *a = NULL;
    int n = 0, cap = 0, nn, ne, i;

    *out = NULL;
    if (root == NULL || root[0] == '\0') root = "/sys/bus/usb/devices";
    nn = lt_serial_list(&names);
    for (i = 0; i < nn; i++) n = info_add(&a, n, &cap, names[i]);
    if (nn > 0) lt_plat_free_list(names, nn);
    ne = lt_plat_list_dir(root, &ents);
    for (i = 0; i < ne; i++) {
        char found[4][32], dev[64], path[64];
        const char *colon = strchr(ents[i], ':');
        int nf, k;
        size_t dl;
        if (colon == NULL || ents[i][0] == '.') continue;
        dl = (size_t)(colon - ents[i]);
        if (dl == 0 || dl >= sizeof dev) continue;
        memcpy(dev, ents[i], dl);
        dev[dl] = '\0';
        nf = iface_ttys(root, ents[i], found);
        for (k = 0; k < nf; k++) {
            int at;
            lt_serial_info *it;
            lt_snprintf(path, sizeof path, "/dev/%s", found[k]);
            at = info_find(a, n, path);
            if (at < 0) {
                n = info_add(&a, n, &cap, path);
                at = n - 1;
            }
            it = &a[at];
            it->usb = 1;
            it->vid = sysfs_hex(root, dev, "idVendor");
            it->pid = sysfs_hex(root, dev, "idProduct");
            sysfs_attr(root, dev, "manufacturer", it->manufacturer, sizeof it->manufacturer);
            sysfs_attr(root, dev, "product", it->product, sizeof it->product);
            sysfs_attr(root, dev, "serial", it->serial, sizeof it->serial);
            lt_strlcpy(it->driver, lt_str_starts(found[k], "ttyACM") ? "cdc_acm" : "usb-serial",
                       sizeof it->driver);
        }
    }
    if (ne > 0) lt_plat_free_list(ents, ne);
    /* the names the kernel gives USB serial devices, also when sysfs is not readable */
    for (i = 0; i < n; i++) {
        if (lt_str_starts(a[i].name, "/dev/ttyACM") || lt_str_starts(a[i].name, "/dev/ttyUSB")) {
            a[i].usb = 1;
        }
    }
    if (n == 0) {
        lt_free(a);
        a = NULL;
    }
    *out = a;
    return n;
}


/* the process is root or a member of the named group (primary or supplementary) */
static int in_group(const char *name)
{
    struct group gr, *g = NULL;
    char buf[4096];
    gid_t list[256];
    int n, i;
    if (geteuid() == 0) return 1;
    if (getgrnam_r(name, &gr, buf, sizeof buf, &g) != 0 || g == NULL) return 0;
    if (getegid() == g->gr_gid) return 1;
    n = getgroups((int)(sizeof list / sizeof list[0]), list);
    for (i = 0; i < n; i++) {
        if (list[i] == g->gr_gid) return 1;
    }
    return 0;
}

int lt_serial_access_ok(void)
{
    return in_group("dialout") || in_group("uucp");
}

int lt_serial_perm_fix(char *cmd, size_t cap)
{
    /* Debian, Ubuntu, Fedora, openSUSE: dialout; Arch and its derivatives: uucp */
    FILE *f = fopen("/etc/group", "r");
    char line[256];
    int dialout = 0, uucp = 0;
    if (f != NULL) {
        while (fgets(line, (int)sizeof line, f) != NULL) {
            if (strncmp(line, "dialout:", 8) == 0) dialout = 1;
            if (strncmp(line, "uucp:", 5) == 0) uucp = 1;
        }
        fclose(f);
    }
    lt_snprintf(cmd, cap, "sudo usermod -aG %s $USER", (!dialout && uucp) ? "uucp" : "dialout");
    return 1;
}
