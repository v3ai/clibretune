/* usb_linux.c - plat/usb.h on Linux: usbfs (/dev/bus/usb/BBB/DDD) ioctls, enumeration
 * from sysfs (/sys/bus/usb/devices). Selected by the build system for TARGET=linux.
 *
 * Enumeration reads the root from $LT_USB_SYSFS_ROOT when set, so tests can point it at
 * a fake sysfs tree.
 *
 * PLATFORM FILE: uses POSIX/Linux headers (fcntl.h, unistd.h, sys/ioctl.h,
 * linux/usbdevice_fs.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#include "../usb_priv.h"

#define LT_USB_SYSFS_DEFAULT "/sys/bus/usb/devices"
#define LT_USB_MAX_IFACES 32

struct lt_usb_dev {
    int fd;
    lt_u32 claimed;     /* bit i: interface i claimed by us */
    lt_u32 reattach;    /* bit i: a kernel driver was detached from interface i */
    char path[256];
};

int lt_usb_supported(void)
{
    return 1;
}

static int map_errno(int e)
{
    switch (e) {
    case ETIMEDOUT: return LT_USB_ETIMEOUT;
    case EPIPE:     return LT_USB_EPIPE;
    case EACCES:
    case EPERM:     return LT_USB_EACCES;
    case ENODEV:
    case ENOENT:
    case ENXIO:
    case ESHUTDOWN: return LT_USB_ENODEV;
    case EBUSY:     return LT_USB_EBUSY;
    case EINVAL:    return LT_USB_EINVAL;
    case ENOMEM:    return LT_USB_ENOMEM;
    default:        return LT_USB_EIO;
    }
}

/* ioctl restarted after signals; returns the ioctl result, errno set on -1 */
static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* ---- enumeration ------------------------------------------------------- */

/* read a sysfs attribute (first line, trailing whitespace stripped); 0 ok */
static int read_attr(const char *dir, const char *name, char *buf, size_t cap)
{
    char *p = lt_path_join(dir, name);
    FILE *f;
    size_t n;
    buf[0] = '\0';
    if (p == NULL) return -1;
    f = fopen(p, "r");
    lt_free(p);
    if (f == NULL) return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' ||
                     buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }
    return 0;
}

/* numeric attribute in the given base, within [0, max]; 0 ok */
static int read_num(const char *dir, const char *name, int base, unsigned long max,
                    unsigned long *out)
{
    char buf[32];
    char *end = NULL;
    unsigned long v;
    if (read_attr(dir, name, buf, sizeof buf) != 0 || buf[0] == '\0') return -1;
    v = strtoul(buf, &end, base);
    if (end == buf || *end != '\0' || v > max) return -1;
    *out = v;
    return 0;
}

static int read_device(const char *root, const char *name, lt_usb_info *it)
{
    char *dir;
    unsigned long vid = 0, pid = 0, bcd = 0, bus = 0, dev = 0;
    int ok;

    memset(it, 0, sizeof *it);
    dir = lt_path_join(root, name);
    if (dir == NULL) return -1;
    ok = read_num(dir, "idVendor", 16, 0xFFFFUL, &vid) == 0 &&
         read_num(dir, "idProduct", 16, 0xFFFFUL, &pid) == 0 &&
         read_num(dir, "busnum", 10, 999UL, &bus) == 0 &&
         read_num(dir, "devnum", 10, 999UL, &dev) == 0;
    if (ok) {
        if (read_num(dir, "bcdDevice", 16, 0xFFFFUL, &bcd) != 0) bcd = 0;
        it->vid = (lt_u16)vid;
        it->pid = (lt_u16)pid;
        it->bcd_device = (lt_u16)bcd;
        it->bus = (int)bus;
        it->addr = (int)dev;
        lt_snprintf(it->path, sizeof it->path, "/dev/bus/usb/%03d/%03d", it->bus, it->addr);
        read_attr(dir, "manufacturer", it->manufacturer, sizeof it->manufacturer);
        read_attr(dir, "product", it->product, sizeof it->product);
        read_attr(dir, "serial", it->serial, sizeof it->serial);
    }
    lt_free(dir);
    return ok ? 0 : -1;
}

int lt_usb_list(lt_u16 vid, lt_u16 pid, lt_usb_info **out)
{
    const char *root = getenv("LT_USB_SYSFS_ROOT");
    char **names = NULL;
    lt_usb_info *arr = NULL;
    int n, i, count = 0, cap = 0;

    if (out == NULL) return LT_USB_EINVAL;
    *out = NULL;
    if (root == NULL || root[0] == '\0') root = LT_USB_SYSFS_DEFAULT;
    n = lt_plat_list_dir(root, &names);
    if (n < 0) return 0;              /* no sysfs (container, chroot): no devices */
    for (i = 0; i < n; i++) {
        lt_usb_info it;
        /* devices are "usbN" (root hubs) and "B-P.P..."; "B-P:C.I" are interfaces */
        if (names[i][0] == '.' || strchr(names[i], ':') != NULL) continue;
        if (read_device(root, names[i], &it) != 0) continue;
        if (!lt_usb_matches(&it, vid, pid)) continue;
        count = lt_usb_list_add(&arr, count, &cap, &it);
        if (count < 0) {
            lt_free(arr);
            lt_plat_free_list(names, n);
            return count;
        }
    }
    lt_plat_free_list(names, n);
    if (count == 0) {
        lt_free(arr);
        arr = NULL;
    }
    *out = arr;
    return count;
}

/* ---- open / close ------------------------------------------------------- */

lt_usb_dev *lt_usb_open(const char *path, char *err, size_t errcap)
{
    lt_usb_dev *d;
    int fd, e;
    char msg[512];

    if (path == NULL || path[0] == '\0') {
        lt_usb_set_err(err, errcap, lt_usb_strerror(LT_USB_EINVAL));
        return NULL;
    }
    do {
        fd = open(path, O_RDWR);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        e = errno;
        switch (map_errno(e)) {
        case LT_USB_EACCES: {
            /* name the device in the udev rule when sysfs knows it */
            lt_usb_info *list = NULL;
            int i, n = lt_usb_list(0, 0, &list), found = -1;
            for (i = 0; i < n; i++) {
                if (strcmp(list[i].path, path) == 0) found = i;
            }
            if (found >= 0) {
                lt_usb_udev_hint(msg, sizeof msg, path, 1, list[found].vid, list[found].pid);
            } else {
                lt_usb_udev_hint(msg, sizeof msg, path, 0, 0, 0);
            }
            lt_usb_free_list(list);
            break;
        }
        case LT_USB_ENODEV:
            lt_snprintf(msg, sizeof msg, "USB device %s not found (unplugged or re-enumerated?)",
                        path);
            break;
        default:
            lt_snprintf(msg, sizeof msg, "cannot open %s: %s", path, strerror(e));
            break;
        }
        lt_usb_set_err(err, errcap, msg);
        return NULL;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    d = (lt_usb_dev *)lt_calloc(1, sizeof *d);
    if (d == NULL) {
        close(fd);
        lt_usb_set_err(err, errcap, lt_usb_strerror(LT_USB_ENOMEM));
        return NULL;
    }
    d->fd = fd;
    lt_strlcpy(d->path, path, sizeof d->path);
    return d;
}

/* USBDEVFS_IOCTL to the interface's driver (detach / reattach) */
static int driver_ioctl(lt_usb_dev *d, int iface, unsigned long code)
{
    struct usbdevfs_ioctl cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.ifno = iface;
    cmd.ioctl_code = (int)code;
    cmd.data = NULL;
    return xioctl(d->fd, USBDEVFS_IOCTL, &cmd);
}

void lt_usb_close(lt_usb_dev *d)
{
    int i;
    if (d == NULL) return;
    for (i = 0; i < LT_USB_MAX_IFACES; i++) {
        lt_u32 bit = (lt_u32)1 << i;
        if (d->claimed & bit) {
            unsigned int ifn = (unsigned int)i;
            (void)xioctl(d->fd, USBDEVFS_RELEASEINTERFACE, &ifn);
        }
        if (d->reattach & bit) (void)driver_ioctl(d, i, USBDEVFS_CONNECT);
    }
    close(d->fd);
    lt_free(d);
}

/* ---- interfaces ------------------------------------------------------------ */

int lt_usb_claim(lt_usb_dev *d, int iface)
{
    struct usbdevfs_getdriver gd;
    struct usbdevfs_disconnect_claim dc;
    lt_u32 bit;
    int had_driver = 0, r;

    if (d == NULL || iface < 0 || iface >= LT_USB_MAX_IFACES) return LT_USB_EINVAL;
    bit = (lt_u32)1 << iface;
    if (d->claimed & bit) return 0;

    memset(&gd, 0, sizeof gd);
    gd.interface = (unsigned int)iface;
    if (xioctl(d->fd, USBDEVFS_GETDRIVER, &gd) == 0) {
        gd.driver[sizeof gd.driver - 1] = '\0';
        if (strcmp(gd.driver, "usbfs") != 0) had_driver = 1;
    }

    /* detach any kernel driver and claim in one step (Linux 3.6+) */
    memset(&dc, 0, sizeof dc);
    dc.interface = (unsigned int)iface;
    dc.flags = USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER;
    lt_strlcpy(dc.driver, "usbfs", sizeof dc.driver);
    r = xioctl(d->fd, USBDEVFS_DISCONNECT_CLAIM, &dc);
    if (r < 0 && errno == ENOTTY) {
        /* older kernel: detach, then claim */
        unsigned int ifn = (unsigned int)iface;
        if (had_driver && driver_ioctl(d, iface, USBDEVFS_DISCONNECT) < 0 && errno != ENODATA) {
            return map_errno(errno);
        }
        r = xioctl(d->fd, USBDEVFS_CLAIMINTERFACE, &ifn);
    }
    if (r < 0) return map_errno(errno);
    d->claimed |= bit;
    if (had_driver) d->reattach |= bit;
    return 0;
}

int lt_usb_set_alt(lt_usb_dev *d, int iface, int alt)
{
    struct usbdevfs_setinterface si;
    int r;
    if (d == NULL || iface < 0 || iface >= LT_USB_MAX_IFACES || alt < 0 || alt > 255) {
        return LT_USB_EINVAL;
    }
    r = lt_usb_claim(d, iface);
    if (r < 0) return r;
    si.interface = (unsigned int)iface;
    si.altsetting = (unsigned int)alt;
    if (xioctl(d->fd, USBDEVFS_SETINTERFACE, &si) < 0) return map_errno(errno);
    return 0;
}

/* ---- transfers ------------------------------------------------------------ */

int lt_usb_control(lt_usb_dev *d, lt_u8 request_type, lt_u8 request, lt_u16 value,
                   lt_u16 index, void *data, lt_u16 length, int timeout_ms)
{
    struct usbdevfs_ctrltransfer c;
    int r;
    if (d == NULL || (length > 0 && data == NULL)) return LT_USB_EINVAL;
    memset(&c, 0, sizeof c);
    c.bRequestType = request_type;
    c.bRequest = request;
    c.wValue = (unsigned short)value;
    c.wIndex = (unsigned short)index;
    c.wLength = (unsigned short)length;
    c.timeout = timeout_ms > 0 ? (unsigned int)timeout_ms : 0U;
    c.data = data;
    r = xioctl(d->fd, USBDEVFS_CONTROL, &c);
    if (r < 0) return map_errno(errno);
    return r;
}

int lt_usb_bulk(lt_usb_dev *d, lt_u8 endpoint, void *data, int length, int timeout_ms)
{
    struct usbdevfs_bulktransfer b;
    int r, code;
    if (d == NULL || length < 0 || (length > 0 && data == NULL)) return LT_USB_EINVAL;
    memset(&b, 0, sizeof b);
    b.ep = endpoint;
    b.len = (unsigned int)length;
    b.timeout = timeout_ms > 0 ? (unsigned int)timeout_ms : 0U;
    b.data = data;
    r = xioctl(d->fd, USBDEVFS_BULK, &b);
    if (r >= 0) return r;
    code = map_errno(errno);
    if (code == LT_USB_EPIPE) {
        /* clear the halt so the next transfer on this endpoint can proceed */
        unsigned int ep = endpoint;
        (void)xioctl(d->fd, USBDEVFS_CLEAR_HALT, &ep);
    }
    return code;
}

/* ---- descriptors ------------------------------------------------------------ */

/* usbfs read(): the device descriptor followed by every configuration descriptor, as
 * cached by the kernel (no bus traffic). Picks the active configuration. */
static int config_from_fd(lt_usb_dev *d, lt_u8 *buf, int cap)
{
    lt_buf all;
    lt_u8 tmp[1024];
    ssize_t r;
    size_t off;
    int nconf, i, want, active = -1, result = LT_USB_EIO;

    lt_buf_init(&all);
    if (lseek(d->fd, 0, SEEK_SET) != 0) return LT_USB_EIO;
    for (;;) {
        r = read(d->fd, tmp, sizeof tmp);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        lt_buf_append(&all, tmp, (size_t)r);
        if (all.len > 1048576UL) break;
    }
    if (all.len < 18 || ((const lt_u8 *)all.data)[1] != LT_USB_DT_DEVICE) {
        lt_buf_free(&all);
        return LT_USB_EIO;
    }
    nconf = ((const lt_u8 *)all.data)[17];
    if (nconf > 1) {
        lt_u8 v = 0;
        if (lt_usb_control(d, 0x80, LT_USB_REQ_GET_CONFIGURATION, 0, 0, &v, 1,
                           LT_USB_DESC_TIMEOUT_MS) == 1) {
            active = v;
        }
    }
    off = 18;
    for (i = 0; i < nconf && off + 9 <= all.len; i++) {
        const lt_u8 *c = (const lt_u8 *)all.data + off;
        size_t total = (size_t)c[2] | ((size_t)c[3] << 8);
        if (c[1] != LT_USB_DT_CONFIG || total < 9) break;
        if (total > all.len - off) total = all.len - off;
        if (active < 0 || c[5] == active) {
            want = total < (size_t)cap ? (int)total : cap;
            memcpy(buf, c, (size_t)want);
            result = want;
            break;
        }
        off += total;
    }
    lt_buf_free(&all);
    return result;
}

int lt_usb_config_descriptor(lt_usb_dev *d, lt_u8 *buf, int cap)
{
    int r;
    if (d == NULL || buf == NULL || cap <= 0) return LT_USB_EINVAL;
    r = config_from_fd(d, buf, cap);
    if (r < 0) r = lt_usb_config_by_control(d, buf, cap);
    return r;
}

int lt_usb_string(lt_usb_dev *d, int index, char *buf, size_t cap)
{
    return lt_usb_string_by_control(d, index, buf, cap);
}
