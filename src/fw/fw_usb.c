/* fw_usb.c - the flashing transport on top of plat/usb.h, bootloader discovery and the
 * user-facing hints for USB permission / driver errors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "fw.h"

const char lt_fw_udev_rule[] =
    "# /etc/udev/rules.d/50-libretune-flash.rules\n"
    "# STM32 ROM bootloader (DFU) and OpenBLT\n"
    "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"0483\", ATTRS{idProduct}==\"df11\", MODE=\"0660\", TAG+=\"uaccess\"\n"
    "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"1d50\", ATTRS{idProduct}==\"60ac\", MODE=\"0660\", TAG+=\"uaccess\"\n";

struct lt_fw_usb { lt_usb_dev *dev; };

static int u_control(void *ctx, lt_u8 rtype, lt_u8 req, lt_u16 value, lt_u16 index, lt_u8 *data,
                     lt_u16 len, int timeout_ms)
{
    return lt_usb_control(((lt_fw_usb *)ctx)->dev, rtype, req, value, index, data, len, timeout_ms);
}

static int u_bulk(void *ctx, lt_u8 ep, lt_u8 *data, int len, int timeout_ms)
{
    return lt_usb_bulk(((lt_fw_usb *)ctx)->dev, ep, data, len, timeout_ms);
}

static int u_config(void *ctx, lt_u8 *buf, int cap)
{
    return lt_usb_config_descriptor(((lt_fw_usb *)ctx)->dev, buf, cap);
}

static int u_string(void *ctx, int index, char *buf, size_t cap)
{
    return lt_usb_string(((lt_fw_usb *)ctx)->dev, index, buf, cap);
}

static int u_claim(void *ctx, int iface, int alt)
{
    lt_usb_dev *d = ((lt_fw_usb *)ctx)->dev;
    int r = lt_usb_claim(d, iface);
    if (r < 0 || alt < 0) { return r; }
    return lt_usb_set_alt(d, iface, alt);
}

lt_fw_usb *lt_fw_usb_open(const char *path, lt_fw_io *io, char *err, size_t errcap)
{
    lt_fw_usb *u;
    lt_usb_dev *d = lt_usb_open(path, err, errcap);
    if (d == NULL) { return NULL; }
    u = (lt_fw_usb *)lt_calloc(1, sizeof(*u));
    if (u == NULL) {
        lt_usb_close(d);
        if (err != NULL && errcap > 0) { lt_strlcpy(err, "out of memory", errcap); }
        return NULL;
    }
    u->dev = d;
    memset(io, 0, sizeof(*io));
    io->ctx = u;
    io->control = u_control;
    io->bulk = u_bulk;
    io->get_config = u_config;
    io->get_string = u_string;
    io->claim = u_claim;
    return u;
}

void lt_fw_usb_close(lt_fw_usb *u)
{
    if (u == NULL) { return; }
    lt_usb_close(u->dev);
    lt_free(u);
}

int lt_fw_scan(lt_fw_found *out, int cap)
{
    lt_usb_info *list = NULL;
    int n = lt_usb_list(0, 0, &list), i, k = 0;
    if (n < 0) { return n; }
    for (i = 0; i < n && k < cap; i++) {
        const lt_usb_info *u = &list[i];
        lt_fw_found *f = &out[k];
        const char *name;
        if (u->vid == LT_FW_DFU_VID && u->pid == LT_FW_DFU_PID) {
            f->method = LT_FW_DFUSE;
            name = "STM32 BOOTLOADER";
        } else if (u->vid == LT_FW_OPENBLT_VID && u->pid == LT_FW_OPENBLT_PID) {
            f->method = LT_FW_OPENBLT;
            name = "OpenBLT";
        } else {
            continue;
        }
        f->sim = 0;
        f->vid = u->vid;
        f->pid = u->pid;
        lt_strlcpy(f->path, u->path, sizeof(f->path));
        lt_snprintf(f->label, sizeof(f->label), "%s %04x:%04x%s%s", u->product[0] != '\0' ? u->product : name,
                    (unsigned)u->vid, (unsigned)u->pid, u->serial[0] != '\0' ? " #" : "", u->serial);
        k++;
    }
    lt_usb_free_list(list);
    return k;
}

const char *lt_fw_usb_hint(int code, int os_windows)
{
    switch (code) {
    case LT_USB_EACCES:
        return os_windows
            ? "Windows cannot open the device: it must use the WinUSB driver. Run Zadig "
              "(zadig.akeo.ie), pick \"STM32 BOOTLOADER\" (or OpenBLT), choose WinUSB and click "
              "Install/Replace Driver, then scan again. STM32CubeProgrammer's driver also works."
            : "No permission to open the USB device. Install the udev rule below (as root), "
              "then unplug and replug the board:";
    case LT_USB_EBUSY:
        return "Another program holds the device (close STM32CubeProgrammer, dfu-util, the rusEFI "
               "console or a virtual machine using it).";
    case LT_USB_ENOTSUP:
        return "This build cannot access USB devices. Use dfu-util or STM32CubeProgrammer instead.";
    case LT_USB_ENODEV:
        return "The device disappeared: check the cable, and that the board is still in its bootloader.";
    default:
        return NULL;
    }
}
