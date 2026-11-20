/* test_usb.c - plat/usb.h: error texts, string descriptor decoding, the Linux sysfs
 * enumeration against a fake tree, and a live smoke test over whatever devices this
 * machine has (passes with none, or without permission to open them). */
#include "lt_test.h"
#include "../src/plat/plat.h"
#include "../src/plat/usb.h"
#include "../src/plat/usb_priv.h"
#include <stdlib.h>
#include <string.h>

LT_TEST(test_strerror)
{
    static const int codes[] = {
        LT_USB_EIO, LT_USB_ETIMEOUT, LT_USB_EPIPE, LT_USB_EACCES, LT_USB_ENODEV,
        LT_USB_EBUSY, LT_USB_ENOTSUP, LT_USB_EINVAL, LT_USB_ENOMEM
    };
    size_t i, j;
    for (i = 0; i < LT_COUNTOF(codes); i++) {
        const char *s = lt_usb_strerror(codes[i]);
        LT_CHECK(s != NULL && s[0] != '\0');
        for (j = 0; j < i; j++) LT_CHECK(strcmp(s, lt_usb_strerror(codes[j])) != 0);
    }
    LT_CHECK_STR(lt_usb_strerror(0), "ok");
    LT_CHECK_STR(lt_usb_strerror(64), "ok");
    LT_CHECK_STR(lt_usb_strerror(-100), "unknown USB error");
}

/* build a string descriptor from UTF-16 code units */
static int make_desc(lt_u8 *d, const unsigned *units, int n)
{
    int i;
    d[0] = (lt_u8)(2 + 2 * n);
    d[1] = LT_USB_DT_STRING;
    for (i = 0; i < n; i++) {
        d[2 + 2 * i] = (lt_u8)(units[i] & 0xFFU);
        d[3 + 2 * i] = (lt_u8)(units[i] >> 8);
    }
    return 2 + 2 * n;
}

LT_TEST(test_string_desc)
{
    static const unsigned ascii[] = { 'D', 'F', 'U' };
    /* "µ€" + U+1F600 (surrogate pair) */
    static const unsigned bmp[] = { 0x00B5, 0x20AC, 0xD83D, 0xDE00 };
    /* lone high surrogate, lone low surrogate */
    static const unsigned bad[] = { 'a', 0xD800, 'b', 0xDC00 };
    static const unsigned nul[] = { 'x', 0, 'y' };
    lt_u8 d[64];
    char out[32];
    int n;

    n = make_desc(d, ascii, 3);
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, sizeof out), 3);
    LT_CHECK_STR(out, "DFU");

    n = make_desc(d, bmp, 4);
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, sizeof out), 2 + 3 + 4);
    LT_CHECK_STR(out, "\xC2\xB5\xE2\x82\xAC\xF0\x9F\x98\x80");
    /* too small: whole characters only */
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, 6), 5);
    LT_CHECK_STR(out, "\xC2\xB5\xE2\x82\xAC");
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, 4), 2);
    LT_CHECK_STR(out, "\xC2\xB5");

    n = make_desc(d, bad, 4);
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, sizeof out), 8);
    LT_CHECK_STR(out, "a\xEF\xBF\xBD" "b\xEF\xBF\xBD");

    n = make_desc(d, nul, 3);
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, sizeof out), 1);
    LT_CHECK_STR(out, "x");

    /* bLength beyond the received bytes, odd lengths, wrong type, empty */
    n = make_desc(d, ascii, 3);
    LT_CHECK_INT(lt_usb_string_from_desc(d, 5, out, sizeof out), 1);
    LT_CHECK_STR(out, "D");
    d[0] = 7;
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, sizeof out), 2);
    d[0] = 8;
    d[1] = LT_USB_DT_CONFIG;
    LT_CHECK_INT(lt_usb_string_from_desc(d, n, out, sizeof out), LT_USB_EIO);
    LT_CHECK_STR(out, "");
    d[0] = 2;
    d[1] = LT_USB_DT_STRING;
    LT_CHECK_INT(lt_usb_string_from_desc(d, 2, out, sizeof out), 0);
    LT_CHECK_INT(lt_usb_string_from_desc(d, 1, out, sizeof out), LT_USB_EIO);
    LT_CHECK_INT(lt_usb_string_from_desc(NULL, 0, out, sizeof out), LT_USB_EIO);
    LT_CHECK_INT(lt_usb_string_from_desc(d, 2, NULL, 0), LT_USB_EINVAL);
}

LT_TEST(test_list_helpers)
{
    lt_usb_info *arr = NULL, it;
    int n = 0, cap = 0, i;
    memset(&it, 0, sizeof it);
    for (i = 0; i < 40; i++) {
        it.vid = (lt_u16)(0x1000 + i);
        it.pid = (lt_u16)i;
        n = lt_usb_list_add(&arr, n, &cap, &it);
        LT_CHECK_INT(n, i + 1);
    }
    LT_CHECK(cap >= 40);
    LT_CHECK(arr != NULL && arr[39].vid == 0x1027 && arr[39].pid == 39);
    LT_CHECK(lt_usb_matches(&arr[3], 0, 0));
    LT_CHECK(lt_usb_matches(&arr[3], 0x1003, 0));
    LT_CHECK(lt_usb_matches(&arr[3], 0x1003, 3));
    LT_CHECK(!lt_usb_matches(&arr[3], 0x1003, 4));
    LT_CHECK(!lt_usb_matches(&arr[3], 0x1004, 0));
    lt_usb_free_list(arr);
    lt_usb_free_list(NULL);
}

LT_TEST(test_hints)
{
    char buf[512];
    lt_usb_udev_hint(buf, sizeof buf, "/dev/bus/usb/001/005", 1, 0x0483, 0xDF11);
    LT_CHECK(strstr(buf, "/dev/bus/usb/001/005") != NULL);
    LT_CHECK(strstr(buf, "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0483\", "
                         "ATTR{idProduct}==\"df11\", MODE=\"0666\"") != NULL);
    LT_CHECK(strstr(buf, "TAG+=\"uaccess\"") != NULL);
    lt_usb_udev_hint(buf, sizeof buf, NULL, 0, 0, 0);
    LT_CHECK(strstr(buf, "idVendor") == NULL);
    LT_CHECK(strstr(buf, "SUBSYSTEM==\"usb\", MODE") != NULL);
    lt_usb_set_err(buf, sizeof buf, "x");
    LT_CHECK_STR(buf, "x");
    lt_usb_set_err(NULL, 0, "x");
}

LT_TEST(test_supported)
{
    lt_os os = lt_plat_os();
    int expect = os == LT_OS_LINUX || os == LT_OS_WINDOWS || os == LT_OS_MACOS;
    LT_CHECK_INT(lt_usb_supported(), expect);
}

LT_TEST(test_bad_args)
{
    char err[256];
    lt_u8 b[16];
    lt_usb_info *l = NULL;
    err[0] = '\0';
    LT_CHECK(lt_usb_open(NULL, err, sizeof err) == NULL);
    LT_CHECK(err[0] != '\0');
    err[0] = '\0';
    LT_CHECK(lt_usb_open("", err, sizeof err) == NULL);
    LT_CHECK(err[0] != '\0');
    LT_CHECK(lt_usb_open("", NULL, 0) == NULL);
    LT_CHECK(lt_usb_list(0, 0, NULL) < 0);
    LT_CHECK(lt_usb_control(NULL, 0x80, 6, 0x100, 0, b, sizeof b, 100) < 0);
    LT_CHECK(lt_usb_bulk(NULL, 0x81, b, (int)sizeof b, 100) < 0);
    LT_CHECK(lt_usb_claim(NULL, 0) < 0);
    LT_CHECK(lt_usb_set_alt(NULL, 0, 0) < 0);
    LT_CHECK(lt_usb_config_descriptor(NULL, b, (int)sizeof b) < 0);
    LT_CHECK(lt_usb_string(NULL, 1, err, sizeof err) < 0);
    lt_usb_close(NULL);
    if (lt_usb_supported()) {
        /* a device that is not there */
        const char *path = "/dev/bus/usb/999/999";
        if (lt_plat_os() == LT_OS_WINDOWS) {
            path = "\\\\?\\usb#vid_0483&pid_df11#lt_no_such_device#"
                   "{a5dcbf10-6530-11d2-901f-00c04fb951ed}";
        } else if (lt_plat_os() == LT_OS_MACOS) {
            path = "7ffffffffffffff0";
        }
        err[0] = '\0';
        LT_CHECK(lt_usb_open(path, err, sizeof err) == NULL);
        LT_CHECK(err[0] != '\0');
        printf("  open(%s): %s\n", path, err);
        LT_CHECK(lt_usb_list(0, 0, &l) >= 0);
        lt_usb_free_list(l);
    } else {
        LT_CHECK_INT(lt_usb_list(0, 0, &l), LT_USB_ENOTSUP);
        LT_CHECK(l == NULL);
    }
}

/* ---- Linux: enumeration from a fake sysfs tree --------------------------------- */

static void put_attr(const char *root, const char *dev, const char *name, const char *val)
{
    char *d = lt_path_join(root, dev);
    char *f = lt_path_join(d, name);
    lt_plat_mkdirs(d);
    LT_CHECK(lt_file_write(f, val, strlen(val)) == 0);
    lt_free(f);
    lt_free(d);
}

LT_TEST(test_linux_sysfs)
{
    static lt_usb_info dummy;
    char *home, *root;
    lt_usb_info *l = NULL;
    int n;

    if (lt_plat_os() != LT_OS_LINUX) return;
    home = lt_plat_getenv("HOME");
    root = lt_path_join(home != NULL ? home : "build", "fake_sysfs_usb");
    lt_free(home);

    /* an STM32 DFU bootloader */
    put_attr(root, "1-1.4", "idVendor", "0483\n");
    put_attr(root, "1-1.4", "idProduct", "df11\n");
    put_attr(root, "1-1.4", "bcdDevice", "2200\n");
    put_attr(root, "1-1.4", "busnum", "1\n");
    put_attr(root, "1-1.4", "devnum", "17\n");
    put_attr(root, "1-1.4", "manufacturer", "STMicroelectronics\n");
    put_attr(root, "1-1.4", "product", "STM32  BOOTLOADER\n");
    put_attr(root, "1-1.4", "serial", "305E36A53235\n");
    /* its interface: skipped */
    put_attr(root, "1-1.4:1.0", "bInterfaceClass", "fe\n");
    /* a root hub without strings or bcdDevice */
    put_attr(root, "usb1", "idVendor", "1d6b\n");
    put_attr(root, "usb1", "idProduct", "0002\n");
    put_attr(root, "usb1", "busnum", "1\n");
    put_attr(root, "usb1", "devnum", "1\n");
    /* broken entries: no idProduct; non-hex vendor; bus number out of range */
    put_attr(root, "2-1", "idVendor", "1234\n");
    put_attr(root, "2-1", "busnum", "2\n");
    put_attr(root, "2-1", "devnum", "3\n");
    put_attr(root, "2-2", "idVendor", "zz12\n");
    put_attr(root, "2-2", "idProduct", "0001\n");
    put_attr(root, "2-2", "busnum", "2\n");
    put_attr(root, "2-2", "devnum", "4\n");
    put_attr(root, "2-3", "idVendor", "1234\n");
    put_attr(root, "2-3", "idProduct", "0001\n");
    put_attr(root, "2-3", "busnum", "1000\n");
    put_attr(root, "2-3", "devnum", "4\n");

    LT_CHECK(lt_plat_setenv("LT_USB_SYSFS_ROOT", root) == 0);

    n = lt_usb_list(0, 0, &l);
    LT_CHECK_INT(n, 2);
    if (n == 2) {
        /* sorted by sysfs name: "1-1.4" before "usb1" */
        LT_CHECK_INT(l[0].vid, 0x0483);
        LT_CHECK_INT(l[0].pid, 0xDF11);
        LT_CHECK_INT(l[0].bcd_device, 0x2200);
        LT_CHECK_INT(l[0].bus, 1);
        LT_CHECK_INT(l[0].addr, 17);
        LT_CHECK_STR(l[0].path, "/dev/bus/usb/001/017");
        LT_CHECK_STR(l[0].manufacturer, "STMicroelectronics");
        LT_CHECK_STR(l[0].product, "STM32  BOOTLOADER");
        LT_CHECK_STR(l[0].serial, "305E36A53235");
        LT_CHECK_INT(l[1].vid, 0x1D6B);
        LT_CHECK_INT(l[1].pid, 0x0002);
        LT_CHECK_INT(l[1].bcd_device, 0);
        LT_CHECK_STR(l[1].path, "/dev/bus/usb/001/001");
        LT_CHECK_STR(l[1].product, "");
    }
    lt_usb_free_list(l);

    l = NULL;
    n = lt_usb_list(0x0483, 0xDF11, &l);
    LT_CHECK_INT(n, 1);
    LT_CHECK(l != NULL && l[0].addr == 17);
    lt_usb_free_list(l);

    l = NULL;
    n = lt_usb_list(0x0483, 0, &l);
    LT_CHECK_INT(n, 1);
    lt_usb_free_list(l);

    l = &dummy;   /* must be reset to NULL */
    n = lt_usb_list(0x0483, 0x1234, &l);
    LT_CHECK_INT(n, 0);
    LT_CHECK(l == NULL);

    /* a missing sysfs is "no devices", not an error */
    {
        char *gone = lt_path_join(root, "nonexistent");
        LT_CHECK(lt_plat_setenv("LT_USB_SYSFS_ROOT", gone) == 0);
        n = lt_usb_list(0, 0, &l);
        LT_CHECK_INT(n, 0);
        LT_CHECK(l == NULL);
        lt_free(gone);
    }

    LT_CHECK(lt_plat_setenv("LT_USB_SYSFS_ROOT", "") == 0);
    lt_free(root);
}

/* ---- live: whatever is attached (never fails for lack of devices/permission) ---- */

LT_TEST(test_live)
{
    lt_usb_info *l = NULL;
    int n, i, opened = 0;

    if (!lt_usb_supported()) return;
    n = lt_usb_list(0, 0, &l);
    LT_CHECK(n >= 0);
    printf("  %d USB device(s)\n", n);
    for (i = 0; i < n; i++) {
        char err[512];
        lt_usb_dev *d;
        lt_u8 dd[18], cfg[512];
        int r;

        LT_CHECK(l[i].path[0] != '\0');
        printf("  %04x:%04x bcd %04x bus %d addr %d \"%s\" \"%s\" \"%s\"\n",
               (unsigned)l[i].vid, (unsigned)l[i].pid, (unsigned)l[i].bcd_device, l[i].bus,
               l[i].addr, l[i].manufacturer, l[i].product, l[i].serial);
        err[0] = '\0';
        d = lt_usb_open(l[i].path, err, sizeof err);
        if (d == NULL) {
            LT_CHECK(err[0] != '\0');
            if (i == 0) printf("    open: %s\n", err);
            continue;
        }
        opened++;
        /* GET_DESCRIPTOR(DEVICE): a standard request every device answers */
        r = lt_usb_control(d, 0x80, LT_USB_REQ_GET_DESCRIPTOR, (lt_u16)(LT_USB_DT_DEVICE << 8), 0,
                           dd, (lt_u16)sizeof dd, 1000);
        printf("    device descriptor: %d\n", r);
        if (r == (int)sizeof dd) {
            LT_CHECK_INT(dd[1], LT_USB_DT_DEVICE);
            LT_CHECK_INT(dd[8] | (dd[9] << 8), l[i].vid);
            LT_CHECK_INT(dd[10] | (dd[11] << 8), l[i].pid);
            if (dd[15] != 0) {
                char s[128];
                r = lt_usb_string(d, dd[15], s, sizeof s);
                printf("    product string: %d \"%s\"\n", r, r >= 0 ? s : "");
            }
        }
        r = lt_usb_config_descriptor(d, cfg, (int)sizeof cfg);
        printf("    config descriptor: %d\n", r);
        if (r >= 9) {
            LT_CHECK_INT(cfg[1], LT_USB_DT_CONFIG);
            LT_CHECK(r <= (cfg[2] | (cfg[3] << 8)));
        }
        lt_usb_close(d);
    }
    printf("  opened %d of %d\n", opened, n);
    lt_usb_free_list(l);
}

int main(void)
{
    LT_RUN(test_strerror);
    LT_RUN(test_string_desc);
    LT_RUN(test_list_helpers);
    LT_RUN(test_hints);
    LT_RUN(test_supported);
    LT_RUN(test_bad_args);
    LT_RUN(test_live);
    LT_RUN(test_linux_sysfs);
    LT_TEST_MAIN_END
}
