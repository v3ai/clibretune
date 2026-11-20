/* usb_macos.c - plat/usb.h on macOS: IOKit user-space USB (IOUSBLib) through its C
 * COM-style interfaces. Selected by the build system for TARGET=macos.
 *
 * Devices are matched as IOUSBHostDevice (macOS 10.11+) and IOUSBDevice (older
 * systems), deduplicated by registry entry ID; lt_usb_info.path is that ID in hex.
 * Control transfers go through IOUSBDeviceInterface245::DeviceRequestTO (which works
 * even when another client has the device open), bulk transfers through the claimed
 * interface's IOUSBInterfaceInterface245, mapping endpoint addresses to pipe refs with
 * GetPipeProperties.
 *
 * Written without access to a Mac: compiled by the macOS CI job only.
 *
 * PLATFORM FILE: uses Apple headers (CoreFoundation, IOKit/IOKitLib.h,
 * IOKit/IOCFPlugIn.h, IOKit/usb/IOUSBLib.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include "../usb_priv.h"

#define LT_MAC_MAX_IFACES 32

typedef IOUSBDeviceInterface245 **lt_mac_dev;
typedef IOUSBInterfaceInterface245 **lt_mac_intf;

struct lt_usb_dev {
    lt_mac_dev dev;
    int opened;                          /* USBDeviceOpen succeeded */
    lt_mac_intf intf[LT_MAC_MAX_IFACES]; /* claimed (opened) interfaces by number */
};

int lt_usb_supported(void)
{
    return 1;
}

static int map_kr(IOReturn kr)
{
    switch (kr) {
    case kIOReturnSuccess:         return 0;
    case kIOUSBTransactionTimeout:
    case kIOReturnTimeout:         return LT_USB_ETIMEOUT;
    case kIOUSBPipeStalled:        return LT_USB_EPIPE;
    case kIOReturnNotPrivileged:   return LT_USB_EACCES;
    case kIOReturnNoDevice:
    case kIOReturnNotResponding:
    case kIOReturnNotAttached:     return LT_USB_ENODEV;
    case kIOReturnExclusiveAccess: return LT_USB_EBUSY;
    case kIOReturnNoMemory:        return LT_USB_ENOMEM;
    case kIOReturnBadArgument:     return LT_USB_EINVAL;
    case kIOReturnUnsupported:     return LT_USB_ENOTSUP;
    default:                       return LT_USB_EIO;
    }
}

/* ---- registry entry IDs as text ------------------------------------------------ */

static void id_to_hex(UInt64 id, char *buf, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[17];
    int i;
    for (i = 15; i >= 0; i--) {
        tmp[i] = digits[(unsigned)(id & 0xFU)];
        id >>= 4;
    }
    tmp[16] = '\0';
    lt_strlcpy(buf, tmp, cap);
}

static int hex_to_id(const char *s, UInt64 *out)
{
    UInt64 v = 0;
    int n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s != '\0'; s++, n++) {
        int h;
        if (*s >= '0' && *s <= '9') h = *s - '0';
        else if (*s >= 'a' && *s <= 'f') h = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') h = *s - 'A' + 10;
        else return -1;
        if (n >= 16) return -1;
        v = (v << 4) | (UInt64)h;
    }
    if (n == 0) return -1;
    *out = v;
    return 0;
}

/* ---- enumeration ------------------------------------------------------------------ */

static long reg_long(io_service_t s, CFStringRef key, long dflt)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    long out = dflt;
    if (v != NULL) {
        long x = 0;
        if (CFGetTypeID(v) == CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &x)) {
            out = x;
        }
        CFRelease(v);
    }
    return out;
}

static void reg_str(io_service_t s, CFStringRef key, char *buf, size_t cap)
{
    CFTypeRef v;
    if (buf[0] != '\0') return;           /* already found under another key */
    v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    if (v != NULL) {
        if (CFGetTypeID(v) == CFStringGetTypeID() &&
            !CFStringGetCString((CFStringRef)v, buf, (CFIndex)cap, kCFStringEncodingUTF8)) {
            buf[0] = '\0';
        }
        CFRelease(v);
    }
}

static int read_service(io_service_t s, lt_usb_info *it)
{
    UInt64 id = 0;
    long vid, pid, loc;
    memset(it, 0, sizeof *it);
    if (IORegistryEntryGetRegistryEntryID(s, &id) != KERN_SUCCESS) return -1;
    vid = reg_long(s, CFSTR("idVendor"), -1);
    pid = reg_long(s, CFSTR("idProduct"), -1);
    if (vid < 0 || vid > 0xFFFFL || pid < 0 || pid > 0xFFFFL) return -1;
    it->vid = (lt_u16)vid;
    it->pid = (lt_u16)pid;
    it->bcd_device = (lt_u16)((unsigned long)reg_long(s, CFSTR("bcdDevice"), 0) & 0xFFFFUL);
    loc = reg_long(s, CFSTR("locationID"), -1);
    it->bus = loc < 0 ? -1 : (int)(((unsigned long)loc >> 24) & 0xFFUL);
    it->addr = (int)reg_long(s, CFSTR("USB Address"), -1);
    id_to_hex(id, it->path, sizeof it->path);
    reg_str(s, CFSTR("USB Vendor Name"), it->manufacturer, sizeof it->manufacturer);
    reg_str(s, CFSTR("kUSBVendorString"), it->manufacturer, sizeof it->manufacturer);
    reg_str(s, CFSTR("USB Product Name"), it->product, sizeof it->product);
    reg_str(s, CFSTR("kUSBProductString"), it->product, sizeof it->product);
    reg_str(s, CFSTR("USB Serial Number"), it->serial, sizeof it->serial);
    reg_str(s, CFSTR("kUSBSerialNumberString"), it->serial, sizeof it->serial);
    return 0;
}

/* add matching services of an IOKit class; returns the new count or LT_USB_E* */
static int scan_class(const char *cls, lt_u16 vid, lt_u16 pid, lt_usb_info **arr, int n,
                      int *cap)
{
    CFMutableDictionaryRef m = IOServiceMatching(cls);
    io_iterator_t iter = 0;
    io_service_t s;
    if (m == NULL) return n;
    /* consumes m */
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, m, &iter) != KERN_SUCCESS) return n;
    while ((s = IOIteratorNext(iter)) != 0) {
        lt_usb_info it;
        int ok = read_service(s, &it) == 0, i, dup = 0;
        IOObjectRelease(s);
        if (!ok || !lt_usb_matches(&it, vid, pid)) continue;
        for (i = 0; i < n; i++) {
            if (strcmp((*arr)[i].path, it.path) == 0) dup = 1;
        }
        if (dup) continue;
        n = lt_usb_list_add(arr, n, cap, &it);
        if (n < 0) break;
    }
    IOObjectRelease(iter);
    return n;
}

int lt_usb_list(lt_u16 vid, lt_u16 pid, lt_usb_info **out)
{
    lt_usb_info *arr = NULL;
    int n = 0, cap = 0;
    if (out == NULL) return LT_USB_EINVAL;
    *out = NULL;
    n = scan_class("IOUSBHostDevice", vid, pid, &arr, n, &cap);
    if (n >= 0) n = scan_class("IOUSBDevice", vid, pid, &arr, n, &cap);
    if (n <= 0) {
        lt_free(arr);
        return n;
    }
    *out = arr;
    return n;
}

/* ---- open / close ------------------------------------------------------------------- */

/* user-client plugin + QueryInterface; NULL on failure (*kr_out set) */
static void *query_plugin(io_service_t s, CFUUIDRef type, CFUUIDRef iid, IOReturn *kr_out)
{
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    void *pv = NULL;
    IOReturn kr = kIOReturnError;
    HRESULT hr;
    int tries;
    /* the plugin can be briefly unavailable right after a device (re)appears */
    for (tries = 0; tries < 5; tries++) {
        kr = IOCreatePlugInInterfaceForService(s, type, kIOCFPlugInInterfaceID, &plug, &score);
        if (kr == kIOReturnSuccess && plug != NULL) break;
        plug = NULL;
        lt_plat_sleep_ms(20);
    }
    *kr_out = kr;
    if (plug == NULL) return NULL;
    hr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(iid), &pv);
    IODestroyPlugInInterface(plug);
    if (hr != 0 || pv == NULL) {
        *kr_out = kIOReturnUnsupported;
        return NULL;
    }
    return pv;
}

lt_usb_dev *lt_usb_open(const char *path, char *err, size_t errcap)
{
    UInt64 id = 0;
    CFMutableDictionaryRef m;
    io_service_t s;
    IOReturn kr = kIOReturnError;
    lt_mac_dev dev;
    lt_usb_dev *d;
    char msg[256];

    if (path == NULL || hex_to_id(path, &id) != 0) {
        lt_usb_set_err(err, errcap, lt_usb_strerror(LT_USB_EINVAL));
        return NULL;
    }
    m = IORegistryEntryIDMatching(id);
    s = m != NULL ? IOServiceGetMatchingService(MACH_PORT_NULL, m) : 0;   /* consumes m */
    if (s == 0) {
        lt_snprintf(msg, sizeof msg, "USB device %s not found (unplugged or re-enumerated?)", path);
        lt_usb_set_err(err, errcap, msg);
        return NULL;
    }
    dev = (lt_mac_dev)query_plugin(s, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID245, &kr);
    IOObjectRelease(s);
    if (dev == NULL) {
        lt_snprintf(msg, sizeof msg, "cannot access USB device %s: %s (IOKit error 0x%08lx)", path,
                    lt_usb_strerror(map_kr(kr)), (unsigned long)(UInt32)kr);
        lt_usb_set_err(err, errcap, msg);
        return NULL;
    }
    d = (lt_usb_dev *)lt_calloc(1, sizeof *d);
    if (d == NULL) {
        (void)(*dev)->Release(dev);
        lt_usb_set_err(err, errcap, lt_usb_strerror(LT_USB_ENOMEM));
        return NULL;
    }
    d->dev = dev;
    kr = (*dev)->USBDeviceOpen(dev);
    if (kr == kIOReturnSuccess) {
        d->opened = 1;
    } else if (kr != kIOReturnExclusiveAccess) {
        /* exclusive access only means another client has it open: control transfers
         * and interfaces not claimed elsewhere still work */
        lt_snprintf(msg, sizeof msg, "cannot open USB device %s: %s (IOKit error 0x%08lx)", path,
                    lt_usb_strerror(map_kr(kr)), (unsigned long)(UInt32)kr);
        lt_usb_set_err(err, errcap, msg);
        (void)(*dev)->Release(dev);
        lt_free(d);
        return NULL;
    }
    return d;
}

void lt_usb_close(lt_usb_dev *d)
{
    int i;
    if (d == NULL) return;
    for (i = 0; i < LT_MAC_MAX_IFACES; i++) {
        lt_mac_intf in = d->intf[i];
        if (in != NULL) {
            (void)(*in)->USBInterfaceClose(in);
            (void)(*in)->Release(in);
        }
    }
    if (d->opened) (void)(*d->dev)->USBDeviceClose(d->dev);
    (void)(*d->dev)->Release(d->dev);
    lt_free(d);
}

/* ---- interfaces --------------------------------------------------------------------- */

static lt_mac_intf find_interface(lt_usb_dev *d, int iface)
{
    IOUSBFindInterfaceRequest req;
    io_iterator_t iter = 0;
    io_service_t s;
    lt_mac_intf found = NULL;

    req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    if ((*d->dev)->CreateInterfaceIterator(d->dev, &req, &iter) != kIOReturnSuccess) return NULL;
    while (found == NULL && (s = IOIteratorNext(iter)) != 0) {
        IOReturn kr;
        lt_mac_intf in = (lt_mac_intf)query_plugin(s, kIOUSBInterfaceUserClientTypeID,
                                                   kIOUSBInterfaceInterfaceID245, &kr);
        IOObjectRelease(s);
        if (in != NULL) {
            UInt8 num = 0;
            if ((*in)->GetInterfaceNumber(in, &num) == kIOReturnSuccess && (int)num == iface) {
                found = in;
            } else {
                (void)(*in)->Release(in);
            }
        }
    }
    IOObjectRelease(iter);
    return found;
}

/* an unconfigured device has no interfaces: select its first configuration */
static void ensure_configured(lt_usb_dev *d)
{
    UInt8 cur = 0;
    IOUSBConfigurationDescriptorPtr cd = NULL;
    if (!d->opened) return;
    if ((*d->dev)->GetConfiguration(d->dev, &cur) != kIOReturnSuccess || cur != 0) return;
    if ((*d->dev)->GetConfigurationDescriptorPtr(d->dev, 0, &cd) != kIOReturnSuccess || cd == NULL) {
        return;
    }
    (void)(*d->dev)->SetConfiguration(d->dev, ((const lt_u8 *)cd)[5]);
}

int lt_usb_claim(lt_usb_dev *d, int iface)
{
    lt_mac_intf in;
    IOReturn kr;
    if (d == NULL || iface < 0 || iface >= LT_MAC_MAX_IFACES) return LT_USB_EINVAL;
    if (d->intf[iface] != NULL) return 0;
    in = find_interface(d, iface);
    if (in == NULL) {
        ensure_configured(d);
        in = find_interface(d, iface);
    }
    if (in == NULL) return LT_USB_EINVAL;
    kr = (*in)->USBInterfaceOpen(in);
    if (kr != kIOReturnSuccess) {
        (void)(*in)->Release(in);
        return map_kr(kr);
    }
    d->intf[iface] = in;
    return 0;
}

int lt_usb_set_alt(lt_usb_dev *d, int iface, int alt)
{
    int r;
    IOReturn kr;
    lt_mac_intf in;
    if (d == NULL || iface < 0 || iface >= LT_MAC_MAX_IFACES || alt < 0 || alt > 255) {
        return LT_USB_EINVAL;
    }
    r = lt_usb_claim(d, iface);
    if (r < 0) return r;
    in = d->intf[iface];
    kr = (*in)->SetAlternateInterface(in, (UInt8)alt);
    return kr == kIOReturnSuccess ? 0 : map_kr(kr);
}

/* ---- transfers ---------------------------------------------------------------------- */

int lt_usb_control(lt_usb_dev *d, lt_u8 request_type, lt_u8 request, lt_u16 value,
                   lt_u16 index, void *data, lt_u16 length, int timeout_ms)
{
    IOUSBDevRequestTO req;
    IOReturn kr;
    UInt32 to = timeout_ms > 0 ? (UInt32)timeout_ms : 0U;
    if (d == NULL || (length > 0 && data == NULL)) return LT_USB_EINVAL;
    memset(&req, 0, sizeof req);
    req.bmRequestType = request_type;
    req.bRequest = request;
    req.wValue = (UInt16)value;
    req.wIndex = (UInt16)index;
    req.wLength = (UInt16)length;
    req.pData = data;
    req.wLenDone = 0;
    req.noDataTimeout = to;
    req.completionTimeout = to;
    kr = (*d->dev)->DeviceRequestTO(d->dev, &req);
    if (kr != kIOReturnSuccess) return map_kr(kr);
    return (int)req.wLenDone;
}

/* claimed interface and pipe ref for an endpoint address in the current alt settings */
static int find_pipe(lt_usb_dev *d, lt_u8 endpoint, lt_mac_intf *out, UInt8 *pipe)
{
    int i;
    for (i = 0; i < LT_MAC_MAX_IFACES; i++) {
        lt_mac_intf in = d->intf[i];
        UInt8 ne = 0, p;
        if (in == NULL || (*in)->GetNumEndpoints(in, &ne) != kIOReturnSuccess) continue;
        for (p = 1; p <= ne && p != 0; p++) {
            UInt8 dir = 0, num = 0, tt = 0, interval = 0;
            UInt16 mps = 0;
            if ((*in)->GetPipeProperties(in, p, &dir, &num, &tt, &mps, &interval) !=
                kIOReturnSuccess) {
                continue;
            }
            if ((unsigned)(num & 0x0FU) == (unsigned)(endpoint & 0x0FU) &&
                (dir == kUSBIn) == ((endpoint & 0x80U) != 0)) {
                *out = in;
                *pipe = p;
                return 0;
            }
        }
    }
    return -1;
}

int lt_usb_bulk(lt_usb_dev *d, lt_u8 endpoint, void *data, int length, int timeout_ms)
{
    lt_mac_intf in = NULL;
    UInt8 pipe = 0;
    IOReturn kr;
    UInt32 to = timeout_ms > 0 ? (UInt32)timeout_ms : 0U;
    UInt32 size = (UInt32)(length > 0 ? length : 0);
    int r;
    if (d == NULL || length < 0 || (length > 0 && data == NULL)) return LT_USB_EINVAL;
    if (find_pipe(d, endpoint, &in, &pipe) != 0) return LT_USB_EINVAL;   /* not claimed */
    if (endpoint & 0x80U) {
        kr = (*in)->ReadPipeTO(in, pipe, data, &size, to, to);
    } else {
        kr = (*in)->WritePipeTO(in, pipe, data, size, to, to);
    }
    if (kr == kIOReturnSuccess) return (int)size;
    r = map_kr(kr);
    /* a stall or an aborted (timed out) transfer leaves the pipe halted and the data
     * toggles unknown: reset both ends so the next transfer can proceed */
    if (r == LT_USB_EPIPE || r == LT_USB_ETIMEOUT) (void)(*in)->ClearPipeStallBothEnds(in, pipe);
    return r;
}

/* ---- descriptors -------------------------------------------------------------------- */

int lt_usb_config_descriptor(lt_usb_dev *d, lt_u8 *buf, int cap)
{
    UInt8 nconf = 0, cur = 0, i;
    if (d == NULL || buf == NULL || cap <= 0) return LT_USB_EINVAL;
    if ((*d->dev)->GetNumberOfConfigurations(d->dev, &nconf) != kIOReturnSuccess || nconf == 0) {
        return lt_usb_config_by_control(d, buf, cap);
    }
    if ((*d->dev)->GetConfiguration(d->dev, &cur) != kIOReturnSuccess) cur = 0;
    for (i = 0; i < nconf; i++) {
        IOUSBConfigurationDescriptorPtr cd = NULL;
        const lt_u8 *p;
        int total, want;
        if ((*d->dev)->GetConfigurationDescriptorPtr(d->dev, i, &cd) != kIOReturnSuccess ||
            cd == NULL) {
            continue;
        }
        p = (const lt_u8 *)cd;
        /* the active one, or the first when unconfigured / unknown */
        if (cur != 0 && p[5] != cur) continue;
        total = p[2] | (p[3] << 8);
        if (total < 9) return LT_USB_EIO;
        want = total < cap ? total : cap;
        memcpy(buf, p, (size_t)want);
        return want;
    }
    return lt_usb_config_by_control(d, buf, cap);
}

int lt_usb_string(lt_usb_dev *d, int index, char *buf, size_t cap)
{
    return lt_usb_string_by_control(d, index, buf, cap);
}
