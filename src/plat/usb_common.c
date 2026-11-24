/* usb_common.c - universal parts of plat/usb.h: error texts, the device list helpers
 * and the descriptor helpers the per-OS backends share (usb_priv.h). The OS-specific
 * calls live in src/plat/posix/usb_<target>.c and src/plat/win32/usb.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include <string.h>
#include "../lt.h"
#include "usb.h"
#include "usb_priv.h"

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

void lt_usb_free_list(lt_usb_info *list)
{
    lt_free(list);
}

void lt_usb_set_err(char *err, size_t errcap, const char *msg)
{
    if (err != NULL && errcap > 0) lt_strlcpy(err, msg != NULL ? msg : "", errcap);
}

lt_bool lt_usb_matches(const lt_usb_info *it, lt_u16 vid, lt_u16 pid)
{
    if (vid != 0 && it->vid != vid) return 0;
    if (pid != 0 && it->pid != pid) return 0;
    return 1;
}

int lt_usb_list_add(lt_usb_info **arr, int n, int *cap, const lt_usb_info *it)
{
    if (n >= *cap) {
        int ncap;
        size_t bytes;
        lt_usb_info *na;
        ncap = *cap < 8 ? 8 : *cap;
        if (n >= ncap && !lt_mul_int(ncap, 2, &ncap)) return LT_USB_ENOMEM;
        if (!lt_mul_size((size_t)ncap, sizeof **arr, &bytes)) return LT_USB_ENOMEM;
        na = (lt_usb_info *)lt_realloc(*arr, bytes);
        if (na == NULL) return LT_USB_ENOMEM;
        *arr = na;
        *cap = ncap;
    }
    (*arr)[n] = *it;
    return n + 1;
}

/* UTF-8 encoding of one code point (<= 0x10FFFF) into o; returns the byte count */
static size_t utf8_encode(lt_u32 cp, lt_u8 *o)
{
    if (cp < 0x80UL) {
        o[0] = (lt_u8)cp;
        return 1;
    }
    if (cp < 0x800UL) {
        o[0] = (lt_u8)(0xC0U | (unsigned)(cp >> 6));
        o[1] = (lt_u8)(0x80U | (unsigned)(cp & 0x3FU));
        return 2;
    }
    if (cp < 0x10000UL) {
        o[0] = (lt_u8)(0xE0U | (unsigned)(cp >> 12));
        o[1] = (lt_u8)(0x80U | (unsigned)((cp >> 6) & 0x3FU));
        o[2] = (lt_u8)(0x80U | (unsigned)(cp & 0x3FU));
        return 3;
    }
    o[0] = (lt_u8)(0xF0U | (unsigned)(cp >> 18));
    o[1] = (lt_u8)(0x80U | (unsigned)((cp >> 12) & 0x3FU));
    o[2] = (lt_u8)(0x80U | (unsigned)((cp >> 6) & 0x3FU));
    o[3] = (lt_u8)(0x80U | (unsigned)(cp & 0x3FU));
    return 4;
}

int lt_usb_string_from_desc(const lt_u8 *desc, int len, char *buf, size_t cap)
{
    int end, i;
    size_t out = 0;

    if (buf == NULL || cap == 0) return LT_USB_EINVAL;
    buf[0] = '\0';
    if (desc == NULL || len < 2 || desc[1] != LT_USB_DT_STRING) return LT_USB_EIO;
    end = desc[0] < len ? desc[0] : len;     /* trust neither length alone */
    if (end < 2) return LT_USB_EIO;
    i = 2;
    while (i + 1 < end) {
        lt_u32 cp = (lt_u32)desc[i] | ((lt_u32)desc[i + 1] << 8);
        lt_u8 enc[4];
        size_t k;
        i += 2;
        if (cp >= 0xD800UL && cp <= 0xDBFFUL) {
            lt_u32 lo = 0;
            if (i + 1 < end) lo = (lt_u32)desc[i] | ((lt_u32)desc[i + 1] << 8);
            if (lo >= 0xDC00UL && lo <= 0xDFFFUL) {
                cp = (lt_u32)(0x10000UL + ((cp - 0xD800UL) << 10) + (lo - 0xDC00UL));
                i += 2;
            } else {
                cp = 0xFFFDUL;
            }
        } else if (cp >= 0xDC00UL && cp <= 0xDFFFUL) {
            cp = 0xFFFDUL;
        }
        if (cp == 0) break;                  /* some devices NUL-terminate */
        k = utf8_encode(cp, enc);
        if (out + k >= cap) break;           /* keep whole characters only */
        memcpy(buf + out, enc, k);
        out += k;
    }
    buf[out] = '\0';
    return (int)out;
}

int lt_usb_string_by_control(lt_usb_dev *d, int index, char *buf, size_t cap)
{
    lt_u8 desc[255];
    int r;
    lt_u16 lang = LT_USB_LANG_EN_US;

    if (buf != NULL && cap > 0) buf[0] = '\0';
    if (d == NULL || buf == NULL || cap == 0 || index <= 0 || index > 255) return LT_USB_EINVAL;
    r = lt_usb_control(d, 0x80, LT_USB_REQ_GET_DESCRIPTOR,
                       (lt_u16)((LT_USB_DT_STRING << 8) | index), lang,
                       desc, (lt_u16)sizeof desc, LT_USB_DESC_TIMEOUT_MS);
    if (r == LT_USB_EPIPE) {
        /* no US English: use the first language the device lists (string 0) */
        r = lt_usb_control(d, 0x80, LT_USB_REQ_GET_DESCRIPTOR, (lt_u16)(LT_USB_DT_STRING << 8),
                           0, desc, (lt_u16)sizeof desc, LT_USB_DESC_TIMEOUT_MS);
        if (r < 0) return r;
        if (r < 4 || desc[1] != LT_USB_DT_STRING) return LT_USB_EIO;
        lang = (lt_u16)(desc[2] | (desc[3] << 8));
        r = lt_usb_control(d, 0x80, LT_USB_REQ_GET_DESCRIPTOR,
                           (lt_u16)((LT_USB_DT_STRING << 8) | index), lang,
                           desc, (lt_u16)sizeof desc, LT_USB_DESC_TIMEOUT_MS);
    }
    if (r < 0) return r;
    return lt_usb_string_from_desc(desc, r, buf, cap);
}

int lt_usb_config_by_control(lt_usb_dev *d, lt_u8 *buf, int cap)
{
    lt_u8 head[9];
    int r, total, want;

    if (d == NULL || buf == NULL || cap <= 0) return LT_USB_EINVAL;
    r = lt_usb_control(d, 0x80, LT_USB_REQ_GET_DESCRIPTOR, (lt_u16)(LT_USB_DT_CONFIG << 8), 0,
                       head, (lt_u16)sizeof head, LT_USB_DESC_TIMEOUT_MS);
    if (r < 0) return r;
    if (r < 4 || head[1] != LT_USB_DT_CONFIG) return LT_USB_EIO;
    total = head[2] | (head[3] << 8);
    if (total < 9) return LT_USB_EIO;
    want = total < cap ? total : cap;
    r = lt_usb_control(d, 0x80, LT_USB_REQ_GET_DESCRIPTOR, (lt_u16)(LT_USB_DT_CONFIG << 8), 0,
                       buf, (lt_u16)want, LT_USB_DESC_TIMEOUT_MS);
    return r;
}

void lt_usb_udev_hint(char *buf, size_t cap, const char *path, int have_ids,
                      lt_u16 vid, lt_u16 pid)
{
    char ids[96];
    if (have_ids) {
        lt_snprintf(ids, sizeof ids, ", ATTR{idVendor}==\"%04x\", ATTR{idProduct}==\"%04x\"",
                    (unsigned)vid, (unsigned)pid);
    } else {
        ids[0] = '\0';
    }
    lt_snprintf(buf, cap,
                "no permission to open %s: add a udev rule such as "
                "SUBSYSTEM==\"usb\"%s, MODE=\"0666\", TAG+=\"uaccess\" "
                "in /etc/udev/rules.d/50-clibretune.rules, then run "
                "'sudo udevadm control --reload' and replug the device",
                path != NULL ? path : "the USB device", ids);
}
