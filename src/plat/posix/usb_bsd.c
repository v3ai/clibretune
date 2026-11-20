/* usb_bsd.c - plat/usb.h on the BSDs: not implemented yet, every call reports
 * LT_USB_ENOTSUP. Selected by the build system for TARGET=bsd.
 *
 * PLATFORM FILE: no OS headers needed yet (kept with the POSIX backends so the
 * build picks exactly one usb_<target>.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include "../usb_priv.h"

int lt_usb_supported(void) { return 0; }

int lt_usb_list(lt_u16 vid, lt_u16 pid, lt_usb_info **out)
{
    LT_UNUSED(vid); LT_UNUSED(pid);
    if (out != NULL) { *out = NULL; }
    return LT_USB_ENOTSUP;
}

lt_usb_dev *lt_usb_open(const char *path, char *err, size_t errcap)
{
    LT_UNUSED(path);
    lt_usb_set_err(err, errcap, lt_usb_strerror(LT_USB_ENOTSUP));
    return NULL;
}

void lt_usb_close(lt_usb_dev *d) { LT_UNUSED(d); }
int lt_usb_claim(lt_usb_dev *d, int iface) { LT_UNUSED(d); LT_UNUSED(iface); return LT_USB_ENOTSUP; }
int lt_usb_set_alt(lt_usb_dev *d, int iface, int alt)
{
    LT_UNUSED(d); LT_UNUSED(iface); LT_UNUSED(alt);
    return LT_USB_ENOTSUP;
}

int lt_usb_control(lt_usb_dev *d, lt_u8 request_type, lt_u8 request, lt_u16 value,
                   lt_u16 index, void *data, lt_u16 length, int timeout_ms)
{
    LT_UNUSED(d); LT_UNUSED(request_type); LT_UNUSED(request); LT_UNUSED(value);
    LT_UNUSED(index); LT_UNUSED(data); LT_UNUSED(length); LT_UNUSED(timeout_ms);
    return LT_USB_ENOTSUP;
}

int lt_usb_bulk(lt_usb_dev *d, lt_u8 endpoint, void *data, int length, int timeout_ms)
{
    LT_UNUSED(d); LT_UNUSED(endpoint); LT_UNUSED(data); LT_UNUSED(length); LT_UNUSED(timeout_ms);
    return LT_USB_ENOTSUP;
}

int lt_usb_config_descriptor(lt_usb_dev *d, lt_u8 *buf, int cap)
{
    LT_UNUSED(d); LT_UNUSED(buf); LT_UNUSED(cap);
    return LT_USB_ENOTSUP;
}

int lt_usb_string(lt_usb_dev *d, int index, char *buf, size_t cap)
{
    LT_UNUSED(d); LT_UNUSED(index);
    if (buf != NULL && cap > 0) { buf[0] = '\0'; }
    return LT_USB_ENOTSUP;
}
