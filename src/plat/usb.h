/* usb.h - raw USB device access (control and bulk transfers), for the firmware
 * flasher: ST DfuSe bootloaders, OpenBLT over USB bulk, and similar.
 *
 * No libusb: each OS talks to its own kernel interface, in per-platform files chosen by
 * the build (tools/target.sh), never by #ifdef:
 *   Linux   usbfs ioctls on /dev/bus/usb/BBB/DDD, enumeration from /sys/bus/usb/devices
 *   Windows WinUSB + SetupAPI (the device must be bound to the WinUSB driver)
 *   macOS   IOKit IOUSBDeviceInterface / IOUSBInterfaceInterface (C API)
 *   BSD     not supported yet (every call returns LT_USB_ENOTSUP)
 * Single-threaded and blocking with timeouts, like the rest of plat.h: callers keep each
 * call short (one control or bulk transfer) and step their work from the UI loop.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PLAT_USB_H
#define LT_PLAT_USB_H

#include "../lt.h"

LT_BEGIN_DECLS

/* error codes (all negative; transfers return a byte count >= 0 on success) */
#define LT_USB_EIO       (-1)  /* generic I/O error */
#define LT_USB_ETIMEOUT  (-2)  /* transfer timed out */
#define LT_USB_EPIPE     (-3)  /* endpoint stalled (request rejected by the device) */
#define LT_USB_EACCES    (-4)  /* no permission (Linux: udev rule; Windows: driver not WinUSB) */
#define LT_USB_ENODEV    (-5)  /* device gone (unplugged, re-enumerated) */
#define LT_USB_EBUSY     (-6)  /* interface claimed by another program or driver */
#define LT_USB_ENOTSUP   (-7)  /* not supported on this OS / build */
#define LT_USB_EINVAL    (-8)  /* bad argument */
#define LT_USB_ENOMEM    (-9)

typedef struct lt_usb_info {
    lt_u16 vid, pid;
    lt_u16 bcd_device;
    int bus, addr;              /* -1 where the OS has no such numbers */
    char path[256];             /* opaque, OS-specific; pass to lt_usb_open */
    char manufacturer[96];      /* string descriptors where the OS caches them, else "" */
    char product[96];
    char serial[64];
} lt_usb_info;

typedef struct lt_usb_dev lt_usb_dev;

/* 1 if this build can access USB devices at all, 0 otherwise */
int lt_usb_supported(void);

/* Enumerate attached devices; vid/pid 0 match any. Returns the count (>= 0) and a
 * malloc'd array in *out (free with lt_usb_free_list; *out is NULL when the count is 0),
 * or a negative LT_USB_E* code. */
int  lt_usb_list(lt_u16 vid, lt_u16 pid, lt_usb_info **out);
void lt_usb_free_list(lt_usb_info *list);

/* Open a device by lt_usb_info.path. On failure returns NULL and, when err is not NULL,
 * a one-line reason a user can act on (e.g. the udev rule or WinUSB driver hint). */
lt_usb_dev *lt_usb_open(const char *path, char *err, size_t errcap);
void lt_usb_close(lt_usb_dev *d);

/* Claim an interface for exclusive use (Linux: detaches a kernel driver first), and
 * select an alternate setting on it. 0 or LT_USB_E*. */
int lt_usb_claim(lt_usb_dev *d, int iface);
int lt_usb_set_alt(lt_usb_dev *d, int iface, int alt);

/* Control transfer on endpoint 0. bmRequestType bit 7 selects the direction
 * (0x80 = device-to-host). Returns the number of bytes transferred or LT_USB_E*. */
int lt_usb_control(lt_usb_dev *d, lt_u8 request_type, lt_u8 request, lt_u16 value,
                   lt_u16 index, void *data, lt_u16 length, int timeout_ms);

/* Bulk transfer; endpoint bit 7 set = IN (device-to-host). Returns bytes or LT_USB_E*.
 * The endpoint's interface must be claimed. A stalled endpoint is cleared (CLEAR_FEATURE
 * ENDPOINT_HALT) before LT_USB_EPIPE is returned, so the next transfer can proceed.
 * timeout_ms <= 0 waits without a time limit (also for lt_usb_control). */
int lt_usb_bulk(lt_usb_dev *d, lt_u8 endpoint, void *data, int length, int timeout_ms);

/* Descriptors. Configuration descriptor of the active configuration (whole, with its
 * interfaces/endpoints/functional descriptors) into buf: returns its length or LT_USB_E*.
 * String descriptor `index` (language 0x0409) converted to UTF-8: returns length or
 * LT_USB_E*. */
int lt_usb_config_descriptor(lt_usb_dev *d, lt_u8 *buf, int cap);
int lt_usb_string(lt_usb_dev *d, int index, char *buf, size_t cap);

/* short English text for an LT_USB_E* code */
const char *lt_usb_strerror(int code);

LT_END_DECLS

#endif /* LT_PLAT_USB_H */
