/* usb_common.c - universal parts of plat/usb.h. Until a per-OS backend exists for a
 * target, the stubs at the bottom make every call report LT_USB_ENOTSUP.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include "../lt.h"
#include "usb.h"

const char *lt_usb_strerror(int code)
{
    switch (code) {
    case LT_USB_EIO:      return "USB I/O error";
    case LT_USB_ETIMEOUT: return "USB transfer timed out";
    case LT_USB_EPIPE:    return "request rejected by the device (endpoint stall)";
    case LT_USB_EACCES:   return "no permission to access the USB device";
    case LT_USB_ENODEV:   return "USB device not found or disconnected";
    case LT_USB_EBUSY:    return "USB interface is in use by another program or driver";
    case LT_USB_ENOTSUP:  return "USB access is not supported on this system";
    case LT_USB_EINVAL:   return "invalid USB request";
    case LT_USB_ENOMEM:   return "out of memory";
    default:              return code >= 0 ? "ok" : "unknown USB error";
    }
}

/* ---- stubs: replaced by per-OS backends (src/plat/<plat>/usb_*.c) ---- */

int lt_usb_supported(void) { return 0; }

int lt_usb_list(lt_u16 vid, lt_u16 pid, lt_usb_info **out)
{
    LT_UNUSED(vid); LT_UNUSED(pid);
    if (out != NULL) { *out = NULL; }
    return LT_USB_ENOTSUP;
}

void lt_usb_free_list(lt_usb_info *list) { lt_free(list); }

lt_usb_dev *lt_usb_open(const char *path, char *err, size_t errcap)
{
    LT_UNUSED(path);
    if (err != NULL && errcap > 0) { lt_strlcpy(err, lt_usb_strerror(LT_USB_ENOTSUP), errcap); }
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
