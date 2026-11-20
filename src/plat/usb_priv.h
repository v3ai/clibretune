/* usb_priv.h - helpers shared by the per-OS USB backends (plat/usb.h). Universal C,
 * implemented in usb_common.c. Not part of the public interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PLAT_USB_PRIV_H
#define LT_PLAT_USB_PRIV_H

#include "usb.h"

LT_BEGIN_DECLS

/* standard request / descriptor numbers used by the backends */
#define LT_USB_REQ_GET_DESCRIPTOR    0x06
#define LT_USB_REQ_GET_CONFIGURATION 0x08
#define LT_USB_DT_DEVICE             0x01
#define LT_USB_DT_CONFIG             0x02
#define LT_USB_DT_STRING             0x03
#define LT_USB_LANG_EN_US            0x0409
/* timeout for the descriptor requests the backends issue on their own */
#define LT_USB_DESC_TIMEOUT_MS       1000

/* 1 if the device matches a vid/pid filter (0 = any) */
lt_bool lt_usb_matches(const lt_usb_info *it, lt_u16 vid, lt_u16 pid);

/* Append a copy of *it to a growing array. Returns the new count, or LT_USB_ENOMEM
 * (the array is left as it was). */
int lt_usb_list_add(lt_usb_info **arr, int n, int *cap, const lt_usb_info *it);

/* Convert a USB string descriptor (bLength, bDescriptorType 3, UTF-16LE code units) of
 * `len` bytes to NUL-terminated UTF-8. Unpaired surrogates become U+FFFD; output is cut
 * at a character boundary when cap is too small. Returns the UTF-8 length in bytes or
 * LT_USB_EIO when the descriptor is malformed. */
int lt_usb_string_from_desc(const lt_u8 *desc, int len, char *buf, size_t cap);

/* String descriptor via GET_DESCRIPTOR control requests (language 0x0409, falling back
 * to the device's first language). Same contract as lt_usb_string. */
int lt_usb_string_by_control(lt_usb_dev *d, int index, char *buf, size_t cap);

/* Configuration descriptor (index 0) via GET_DESCRIPTOR control requests; copies at
 * most cap bytes and returns the number copied, or LT_USB_E*. */
int lt_usb_config_by_control(lt_usb_dev *d, lt_u8 *buf, int cap);

/* One-line Linux permission hint with a udev rule for vid:pid (generic rule when
 * have_ids is 0). */
void lt_usb_udev_hint(char *buf, size_t cap, const char *path, int have_ids,
                      lt_u16 vid, lt_u16 pid);

/* copy msg into err (when err is not NULL) */
void lt_usb_set_err(char *err, size_t errcap, const char *msg);

LT_END_DECLS

#endif /* LT_PLAT_USB_PRIV_H */
