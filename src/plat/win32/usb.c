/* usb.c - plat/usb.h on Windows: SetupAPI enumeration + WinUSB transfers.
 *
 * Enumeration lists every device with the USB device interface
 * (GUID_DEVINTERFACE_USB_DEVICE, registered by the hub driver for all devices), then
 * the device-interface GUIDs of WinUSB-bound functions (DeviceInterfaceGUIDs in the
 * device's registry key, set by Zadig/libwdi INFs or MS OS descriptors) so the
 * functions of composite devices are found too. lt_usb_info.path is the device
 * interface path for CreateFile. Devices not bound to WinUSB are listed after the
 * WinUSB ones but cannot be opened: lt_usb_open then explains how to install the
 * driver.
 *
 * PLATFORM FILE: uses Win32 headers (windows.h, setupapi.h, winusb.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "win_priv.h"

#include <stdlib.h>
#include <string.h>
#include <setupapi.h>
#include <winusb.h>

#include "../usb_priv.h"

#define LT_WIN_MAX_IFS    16
#define LT_WIN_MAX_GUIDS  16
#define LT_WIN_TO_UNSET   0xFFFFFFFFUL
#define LT_WIN_ZADIG_HINT "install the WinUSB driver for it with Zadig (https://zadig.akeo.ie)"

typedef struct win_if {
    WINUSB_INTERFACE_HANDLE h;
    int num;                         /* bInterfaceNumber */
} win_if;

struct lt_usb_dev {
    HANDLE file;
    win_if ifs[LT_WIN_MAX_IFS];      /* [0] from WinUsb_Initialize, then associated ones */
    int nifs;
    int assoc_done;                  /* every associated interface has been fetched */
    ULONG timeout[LT_WIN_MAX_IFS][32];  /* PIPE_TRANSFER_TIMEOUT last set, per pipe */
};

/* {A5DCBF10-6530-11D2-901F-00C04FB951ED} */
static const GUID lt_guid_usb_device = {
    0xA5DCBF10UL, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED }
};
/* DEVPKEY_Device_BusReportedDeviceDesc: the device's own product string */
static const DEVPROPKEY lt_key_bus_desc = {
    { 0x540B947EUL, 0x8B40, 0x45BC, { 0xA8, 0xA2, 0x6A, 0x0B, 0x89, 0x4C, 0xBD, 0xA2 } }, 4
};

int lt_usb_supported(void)
{
    return 1;
}

static int map_err(DWORD e)
{
    switch (e) {
    case ERROR_SEM_TIMEOUT:          return LT_USB_ETIMEOUT;
    case ERROR_GEN_FAILURE:          return LT_USB_EPIPE;   /* WinUSB reports stalls so */
    case ERROR_ACCESS_DENIED:        return LT_USB_EACCES;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_COMMAND:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_NO_SUCH_DEVICE:
    case ERROR_DEVICE_NOT_CONNECTED: return LT_USB_ENODEV;
    case ERROR_BUSY:
    case ERROR_SHARING_VIOLATION:    return LT_USB_EBUSY;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:       return LT_USB_EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_NO_SYSTEM_RESOURCES:  return LT_USB_ENOMEM;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:     return LT_USB_ENOTSUP;
    default:                         return LT_USB_EIO;
    }
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* find key (case-insensitive, e.g. "VID_") followed by 4 hex digits; 0 ok */
static int find_hex4(const char *s, const char *key, lt_u16 *out)
{
    size_t kl = strlen(key);
    for (; *s != '\0'; s++) {
        if (lt_strnicmp(s, key, kl) == 0) {
            unsigned v = 0;
            int i;
            for (i = 0; i < 4; i++) {
                int h = hex_digit((unsigned char)s[kl + (size_t)i]);
                if (h < 0) break;
                v = (v << 4) | (unsigned)h;
            }
            if (i == 4) {
                *out = (lt_u16)v;
                return 0;
            }
        }
    }
    return -1;
}

/* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (braces optional); 0 ok */
static int parse_guid(const char *s, GUID *g)
{
    lt_u8 b[16];
    int n = 0, i;
    if (*s == '{') s++;
    while (*s != '\0' && *s != '}' && n < 32) {
        int h;
        if (*s == '-') { s++; continue; }
        h = hex_digit((unsigned char)*s);
        if (h < 0) return -1;
        if ((n & 1) == 0) b[n / 2] = (lt_u8)(h << 4);
        else b[n / 2] = (lt_u8)(b[n / 2] | h);
        n++;
        s++;
    }
    if (n != 32) return -1;
    g->Data1 = ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) |
               ((unsigned long)b[2] << 8) | (unsigned long)b[3];
    g->Data2 = (unsigned short)((b[4] << 8) | b[5]);
    g->Data3 = (unsigned short)((b[6] << 8) | b[7]);
    for (i = 0; i < 8; i++) g->Data4[i] = b[8 + i];
    return 0;
}

/* ---- enumeration ------------------------------------------------------------ */

typedef struct enum_ctx {
    lt_u16 vid, pid;
    lt_usb_info *arr;    /* devices bound to WinUSB (openable) */
    int n, cap;
    lt_usb_info *rest;   /* other drivers: listed after the WinUSB ones */
    int nrest, restcap;
    char **ids;          /* device instance ids already listed */
    int nids, idcap;
    int err;
} enum_ctx;

static void reg_prop(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD prop, char *buf, size_t cap)
{
    buf[0] = '\0';
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, prop, NULL, (PBYTE)buf, (DWORD)(cap - 1),
                                           NULL)) {
        buf[0] = '\0';
    }
    buf[cap - 1] = '\0';
}

static void bus_product(HDEVINFO set, SP_DEVINFO_DATA *dev, char *buf, size_t cap)
{
    WCHAR w[128];
    DEVPROPTYPE type = 0;
    buf[0] = '\0';
    memset(w, 0, sizeof w);
    if (SetupDiGetDevicePropertyW(set, dev, &lt_key_bus_desc, &type, (PBYTE)w,
                                  (DWORD)(sizeof w - sizeof w[0]), NULL, 0) &&
        type == DEVPROP_TYPE_STRING) {
        if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL) <= 0) buf[0] = '\0';
    }
    buf[cap - 1] = '\0';
}

static int seen_id(const enum_ctx *c, const char *id)
{
    int i;
    for (i = 0; i < c->nids; i++) {
        if (lt_stricmp(c->ids[i], id) == 0) return 1;
    }
    return 0;
}

static void add_interfaces(enum_ctx *c, const GUID *guid)
{
    HDEVINFO set;
    DWORD i;

    set = SetupDiGetClassDevsA(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    for (i = 0; c->err == 0; i++) {
        SP_DEVICE_INTERFACE_DATA ifd;
        SP_DEVINFO_DATA dev;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det;
        DWORD need = 0;
        char inst[256], hw[512], svc[64];
        lt_usb_info it;
        const char *last;
        int r;

        memset(&ifd, 0, sizeof ifd);
        ifd.cbSize = sizeof ifd;
        if (!SetupDiEnumDeviceInterfaces(set, NULL, guid, i, &ifd)) break;
        (void)SetupDiGetDeviceInterfaceDetailA(set, &ifd, NULL, 0, &need, NULL);
        if (need < sizeof *det || need > 65536UL) continue;
        det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)lt_malloc(need);
        if (det == NULL) {
            c->err = LT_USB_ENOMEM;
            break;
        }
        det->cbSize = sizeof *det;
        memset(&dev, 0, sizeof dev);
        dev.cbSize = sizeof dev;
        memset(&it, 0, sizeof it);
        it.bus = -1;
        it.addr = -1;
        r = SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, NULL, &dev) &&
            strlen(det->DevicePath) < sizeof it.path;
        if (r) lt_strlcpy(it.path, det->DevicePath, sizeof it.path);
        lt_free(det);
        if (!r) continue;
        if (!SetupDiGetDeviceInstanceIdA(set, &dev, inst, (DWORD)sizeof inst, NULL)) inst[0] = '\0';
        inst[sizeof inst - 1] = '\0';
        if (find_hex4(inst, "VID_", &it.vid) != 0 && find_hex4(it.path, "VID_", &it.vid) != 0) {
            continue;
        }
        if (find_hex4(inst, "PID_", &it.pid) != 0 && find_hex4(it.path, "PID_", &it.pid) != 0) {
            continue;
        }
        if (!lt_usb_matches(&it, c->vid, c->pid)) continue;
        if (inst[0] != '\0' && seen_id(c, inst)) continue;
        reg_prop(set, &dev, SPDRP_HARDWAREID, hw, sizeof hw);   /* USB\VID_x&PID_y&REV_z */
        if (find_hex4(hw, "REV_", &it.bcd_device) != 0) it.bcd_device = 0;
        bus_product(set, &dev, it.product, sizeof it.product);
        if (it.product[0] == '\0') reg_prop(set, &dev, SPDRP_DEVICEDESC, it.product, sizeof it.product);
        /* the last instance-id part is the serial number unless Windows generated it */
        last = strrchr(inst, '\\');
        if (last != NULL && strchr(last + 1, '&') == NULL) {
            lt_strlcpy(it.serial, last + 1, sizeof it.serial);
        }
        reg_prop(set, &dev, SPDRP_SERVICE, svc, sizeof svc);
        if (lt_stricmp(svc, "WinUSB") == 0) {
            r = lt_usb_list_add(&c->arr, c->n, &c->cap, &it);
            if (r >= 0) c->n = r;
        } else {
            r = lt_usb_list_add(&c->rest, c->nrest, &c->restcap, &it);
            if (r >= 0) c->nrest = r;
        }
        if (r < 0) {
            c->err = r;
            break;
        }
        if (inst[0] != '\0') c->nids = lt_plat_names_add(&c->ids, c->nids, &c->idcap, inst);
    }
    SetupDiDestroyDeviceInfoList(set);
}

/* DeviceInterfaceGUIDs of every present WinUSB-bound USB device or function */
static int winusb_guids(GUID *out, int max)
{
    HDEVINFO set;
    DWORD i;
    int n = 0;

    set = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return 0;
    for (i = 0; n < max; i++) {
        SP_DEVINFO_DATA dev;
        char svc[64], val[1024];
        HKEY key;
        DWORD type = 0, len;
        const char *p;

        memset(&dev, 0, sizeof dev);
        dev.cbSize = sizeof dev;
        if (!SetupDiEnumDeviceInfo(set, i, &dev)) break;
        reg_prop(set, &dev, SPDRP_SERVICE, svc, sizeof svc);
        if (lt_stricmp(svc, "WinUSB") != 0) continue;
        key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == (HKEY)INVALID_HANDLE_VALUE) continue;
        memset(val, 0, sizeof val);
        len = (DWORD)(sizeof val - 2);
        if (RegQueryValueExA(key, "DeviceInterfaceGUIDs", NULL, &type, (LPBYTE)val, &len)
            != ERROR_SUCCESS) {
            memset(val, 0, sizeof val);
            len = (DWORD)(sizeof val - 2);
            if (RegQueryValueExA(key, "DeviceInterfaceGUID", NULL, &type, (LPBYTE)val, &len)
                != ERROR_SUCCESS) {
                val[0] = '\0';
            }
        }
        RegCloseKey(key);
        val[sizeof val - 2] = '\0';
        val[sizeof val - 1] = '\0';
        /* REG_MULTI_SZ: strings back to back, ended by an empty one */
        for (p = val; *p != '\0' && n < max; p += strlen(p) + 1) {
            GUID g;
            int j, dup = 0;
            if (parse_guid(p, &g) != 0) continue;
            for (j = 0; j < n; j++) {
                if (memcmp(&out[j], &g, sizeof g) == 0) dup = 1;
            }
            if (!dup) out[n++] = g;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

int lt_usb_list(lt_u16 vid, lt_u16 pid, lt_usb_info **out)
{
    enum_ctx c;
    GUID guids[LT_WIN_MAX_GUIDS];
    int ng, i;

    if (out == NULL) return LT_USB_EINVAL;
    *out = NULL;
    memset(&c, 0, sizeof c);
    c.vid = vid;
    c.pid = pid;
    add_interfaces(&c, &lt_guid_usb_device);
    ng = winusb_guids(guids, LT_WIN_MAX_GUIDS);
    for (i = 0; i < ng && c.err == 0; i++) add_interfaces(&c, &guids[i]);
    lt_plat_free_list(c.ids, c.nids);
    /* devices that can be opened first, so "first match" picks a usable one */
    for (i = 0; i < c.nrest && c.err == 0; i++) {
        int r = lt_usb_list_add(&c.arr, c.n, &c.cap, &c.rest[i]);
        if (r < 0) c.err = r;
        else c.n = r;
    }
    lt_free(c.rest);
    if (c.err != 0) {
        lt_free(c.arr);
        return c.err;
    }
    if (c.n == 0) {
        lt_free(c.arr);
        c.arr = NULL;
    }
    *out = c.arr;
    return c.n;
}

/* ---- open / close ------------------------------------------------------------ */

lt_usb_dev *lt_usb_open(const char *path, char *err, size_t errcap)
{
    HANDLE f;
    WINUSB_INTERFACE_HANDLE wh = NULL;
    USB_INTERFACE_DESCRIPTOR id;
    lt_usb_dev *d;
    DWORD e;
    lt_u16 vid = 0, pid = 0;
    char name[32], msg[512];

    if (path == NULL || path[0] == '\0') {
        lt_usb_set_err(err, errcap, lt_usb_strerror(LT_USB_EINVAL));
        return NULL;
    }
    if (find_hex4(path, "VID_", &vid) == 0 && find_hex4(path, "PID_", &pid) == 0) {
        lt_snprintf(name, sizeof name, "%04X:%04X", (unsigned)vid, (unsigned)pid);
    } else {
        lt_strlcpy(name, "(unknown)", sizeof name);
    }
    f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        if (map_err(e) == LT_USB_ENODEV) {
            lt_snprintf(msg, sizeof msg, "USB device %s not found (unplugged or re-enumerated?)",
                        name);
        } else if (e == ERROR_ACCESS_DENIED) {
            lt_snprintf(msg, sizeof msg, "cannot open USB device %s: it is in use by another "
                        "program, or it does not use WinUSB: %s", name, LT_WIN_ZADIG_HINT);
        } else {
            lt_snprintf(msg, sizeof msg, "cannot open USB device %s (Windows error %lu)", name,
                        (unsigned long)e);
        }
        lt_usb_set_err(err, errcap, msg);
        return NULL;
    }
    if (!WinUsb_Initialize(f, &wh)) {
        e = GetLastError();
        CloseHandle(f);
        lt_snprintf(msg, sizeof msg, "USB device %s is not using the WinUSB driver (Windows "
                    "error %lu): %s", name, (unsigned long)e, LT_WIN_ZADIG_HINT);
        lt_usb_set_err(err, errcap, msg);
        return NULL;
    }
    d = (lt_usb_dev *)lt_calloc(1, sizeof *d);
    if (d == NULL) {
        WinUsb_Free(wh);
        CloseHandle(f);
        lt_usb_set_err(err, errcap, lt_usb_strerror(LT_USB_ENOMEM));
        return NULL;
    }
    memset(&id, 0, sizeof id);
    if (!WinUsb_QueryInterfaceSettings(wh, 0, &id)) id.bInterfaceNumber = 0;
    d->file = f;
    d->ifs[0].h = wh;
    d->ifs[0].num = id.bInterfaceNumber;
    d->nifs = 1;
    memset(d->timeout, 0xFF, sizeof d->timeout);   /* all LT_WIN_TO_UNSET */
    return d;
}

void lt_usb_close(lt_usb_dev *d)
{
    int i;
    if (d == NULL) return;
    for (i = d->nifs - 1; i >= 0; i--) WinUsb_Free(d->ifs[i].h);
    CloseHandle(d->file);
    lt_free(d);
}

/* index into d->ifs of interface number iface, fetching associated interfaces as
 * needed; -1 when the WinUSB function has no such interface */
static int iface_index(lt_usb_dev *d, int iface)
{
    int i;
    for (i = 0; i < d->nifs; i++) {
        if (d->ifs[i].num == iface) return i;
    }
    while (!d->assoc_done && d->nifs < LT_WIN_MAX_IFS) {
        WINUSB_INTERFACE_HANDLE ah = NULL;
        USB_INTERFACE_DESCRIPTOR id;
        if (!WinUsb_GetAssociatedInterface(d->ifs[0].h, (UCHAR)(d->nifs - 1), &ah)) {
            d->assoc_done = 1;
            break;
        }
        memset(&id, 0, sizeof id);
        if (!WinUsb_QueryInterfaceSettings(ah, 0, &id)) {
            WinUsb_Free(ah);
            d->assoc_done = 1;
            break;
        }
        d->ifs[d->nifs].h = ah;
        d->ifs[d->nifs].num = id.bInterfaceNumber;
        d->nifs++;
        if (id.bInterfaceNumber == iface) return d->nifs - 1;
    }
    return -1;
}

/* WinUSB opens its interfaces exclusively when the device is opened: claiming only
 * looks the interface up. */
int lt_usb_claim(lt_usb_dev *d, int iface)
{
    if (d == NULL || iface < 0 || iface > 255) return LT_USB_EINVAL;
    return iface_index(d, iface) >= 0 ? 0 : LT_USB_EINVAL;
}

int lt_usb_set_alt(lt_usb_dev *d, int iface, int alt)
{
    int idx;
    if (d == NULL || iface < 0 || iface > 255 || alt < 0 || alt > 255) return LT_USB_EINVAL;
    idx = iface_index(d, iface);
    if (idx < 0) return LT_USB_EINVAL;
    if (!WinUsb_SetCurrentAlternateSetting(d->ifs[idx].h, (UCHAR)alt)) {
        return map_err(GetLastError());
    }
    memset(d->timeout[idx], 0xFF, sizeof d->timeout[idx]);  /* new pipes, default policy */
    return 0;
}

/* ---- transfers ------------------------------------------------------------------ */

static int set_timeout(lt_usb_dev *d, int idx, lt_u8 ep, int timeout_ms)
{
    ULONG to = timeout_ms > 0 ? (ULONG)timeout_ms : 0UL;
    int slot = (ep & 0x0F) | ((ep & 0x80) != 0 ? 16 : 0);
    if (d->timeout[idx][slot] == to) return 0;
    if (!WinUsb_SetPipePolicy(d->ifs[idx].h, (UCHAR)ep, PIPE_TRANSFER_TIMEOUT,
                              (ULONG)sizeof to, &to)) {
        return map_err(GetLastError());
    }
    d->timeout[idx][slot] = to;
    return 0;
}

int lt_usb_control(lt_usb_dev *d, lt_u8 request_type, lt_u8 request, lt_u16 value,
                   lt_u16 index, void *data, lt_u16 length, int timeout_ms)
{
    WINUSB_SETUP_PACKET sp;
    ULONG got = 0;
    if (d == NULL || (length > 0 && data == NULL)) return LT_USB_EINVAL;
    sp.RequestType = request_type;
    sp.Request = request;
    sp.Value = (USHORT)value;
    sp.Index = (USHORT)index;
    sp.Length = (USHORT)length;
    (void)set_timeout(d, 0, 0, timeout_ms);   /* best effort on the default pipe */
    if (!WinUsb_ControlTransfer(d->ifs[0].h, sp, (PUCHAR)data, (ULONG)length, &got, NULL)) {
        return map_err(GetLastError());
    }
    return (int)got;
}

/* index of the claimed interface whose current alternate setting has endpoint ep */
static int pipe_owner(lt_usb_dev *d, lt_u8 ep)
{
    int i, a, p;
    for (i = 0; i < d->nifs; i++) {
        WINUSB_INTERFACE_HANDLE h = d->ifs[i].h;
        UCHAR cur = 0;
        USB_INTERFACE_DESCRIPTOR id;
        if (!WinUsb_GetCurrentAlternateSetting(h, &cur)) cur = 0;
        /* settings are queried by index; find the current one */
        for (a = 0; a < 64; a++) {
            memset(&id, 0, sizeof id);
            if (!WinUsb_QueryInterfaceSettings(h, (UCHAR)a, &id)) break;
            if (id.bAlternateSetting == cur) {
                for (p = 0; p < id.bNumEndpoints; p++) {
                    WINUSB_PIPE_INFORMATION pi;
                    memset(&pi, 0, sizeof pi);
                    if (WinUsb_QueryPipe(h, (UCHAR)a, (UCHAR)p, &pi) && pi.PipeId == ep) return i;
                }
                break;
            }
        }
    }
    return 0;
}

int lt_usb_bulk(lt_usb_dev *d, lt_u8 endpoint, void *data, int length, int timeout_ms)
{
    ULONG got = 0;
    BOOL ok;
    int idx, r;
    if (d == NULL || length < 0 || (length > 0 && data == NULL)) return LT_USB_EINVAL;
    idx = pipe_owner(d, endpoint);
    r = set_timeout(d, idx, endpoint, timeout_ms);
    if (r < 0) return r;
    if (endpoint & 0x80) {
        ok = WinUsb_ReadPipe(d->ifs[idx].h, (UCHAR)endpoint, (PUCHAR)data, (ULONG)length, &got,
                             NULL);
    } else {
        ok = WinUsb_WritePipe(d->ifs[idx].h, (UCHAR)endpoint, (PUCHAR)data, (ULONG)length, &got,
                              NULL);
    }
    if (!ok) {
        r = map_err(GetLastError());
        /* clear the halt so the next transfer on this endpoint can proceed */
        if (r == LT_USB_EPIPE) (void)WinUsb_ResetPipe(d->ifs[idx].h, (UCHAR)endpoint);
        return r;
    }
    return (int)got;
}

/* ---- descriptors ------------------------------------------------------------------ */

int lt_usb_config_descriptor(lt_usb_dev *d, lt_u8 *buf, int cap)
{
    ULONG got = 0;
    if (d == NULL || buf == NULL || cap <= 0) return LT_USB_EINVAL;
    if (!WinUsb_GetDescriptor(d->ifs[0].h, LT_USB_DT_CONFIG, 0, 0, buf, (ULONG)cap, &got)) {
        return map_err(GetLastError());
    }
    return (int)got;
}

/* ---- serial port identity (plat.h lt_serial_list_info) ------------------ */
/* {4D36E978-E325-11CE-BFC1-08002BE10318}: the "Ports (COM & LPT)" setup class */
static const GUID lt_guid_class_ports = {
    0x4D36E978UL, 0xE325, 0x11CE, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 }
};

static int serial_add(lt_serial_info **a, int n, int *cap, const char *name)
{
    int i;
    for (i = 0; i < n; i++) {
        if (lt_stricmp((*a)[i].name, name) == 0) return n;
    }
    if (n >= *cap) {
        int nc = *cap > 0 ? *cap * 2 : 8;
        *a = (lt_serial_info *)lt_realloc(*a, (size_t)nc * sizeof(lt_serial_info));
        *cap = nc;
    }
    memset(&(*a)[n], 0, sizeof(lt_serial_info));
    lt_strlcpy((*a)[n].name, name, sizeof((*a)[n].name));
    return n + 1;
}

/* serial number from an instance id: "USB\VID_x&PID_y\SERIAL" (no '&': Windows did not
 * make it up) or FTDI's "FTDIBUS\VID_x+PID_y+SERIALA\0000" */
static void inst_serial(const char *inst, char *out, size_t cap)
{
    const char *last = strrchr(inst, '\\');
    out[0] = '\0';
    if (lt_strnicmp(inst, "FTDIBUS\\", 8) == 0) {
        const char *p = strchr(inst + 8, '+');
        const char *q = p != NULL ? strchr(p + 1, '+') : NULL;
        const char *e = q != NULL ? strchr(q + 1, '\\') : NULL;
        if (e != NULL && (size_t)(e - q) > 1 && (size_t)(e - q - 1) < cap) {
            memcpy(out, q + 1, (size_t)(e - q - 1));
            out[e - q - 1] = '\0';
        }
        return;
    }
    if (last != NULL && strchr(last + 1, '&') == NULL) lt_strlcpy(out, last + 1, cap);
}

int lt_serial_list_info(lt_serial_info **out)
{
    HDEVINFO set;
    DWORD i;
    char **names = NULL;
    lt_serial_info *a = NULL;
    int n = 0, cap = 0, nn, k;

    *out = NULL;
    set = SetupDiGetClassDevsA(&lt_guid_class_ports, NULL, NULL, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        for (i = 0; i < 256; i++) {
            SP_DEVINFO_DATA dev;
            HKEY key;
            char port[64], inst[256];
            DWORD type = 0, len = (DWORD)(sizeof port - 1);
            lt_serial_info *it;
            memset(&dev, 0, sizeof dev);
            dev.cbSize = sizeof dev;
            if (!SetupDiEnumDeviceInfo(set, i, &dev)) break;
            key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key == (HKEY)INVALID_HANDLE_VALUE) continue;
            memset(port, 0, sizeof port);
            if (RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)port, &len) != ERROR_SUCCESS ||
                type != REG_SZ || lt_strnicmp(port, "COM", 3) != 0) {
                RegCloseKey(key);
                continue;
            }
            RegCloseKey(key);
            port[sizeof port - 1] = '\0';
            n = serial_add(&a, n, &cap, port);
            it = &a[n - 1];
            if (!SetupDiGetDeviceInstanceIdA(set, &dev, inst, (DWORD)sizeof inst, NULL)) inst[0] = '\0';
            inst[sizeof inst - 1] = '\0';
            if (find_hex4(inst, "VID_", &it->vid) == 0 && find_hex4(inst, "PID_", &it->pid) == 0) {
                it->usb = 1;
                inst_serial(inst, it->serial, sizeof it->serial);
            } else {
                it->vid = 0;
                it->pid = 0;
                it->usb = lt_strnicmp(inst, "USB", 3) == 0 ? 1 : 0;
            }
            bus_product(set, &dev, it->product, sizeof it->product);
            if (it->product[0] == '\0') reg_prop(set, &dev, SPDRP_DEVICEDESC, it->product, sizeof it->product);
            reg_prop(set, &dev, SPDRP_MFG, it->manufacturer, sizeof it->manufacturer);
            reg_prop(set, &dev, SPDRP_SERVICE, it->driver, sizeof it->driver);
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    /* ports SetupAPI did not report (virtual ports, odd drivers) by name only */
    nn = lt_serial_list(&names);
    for (k = 0; k < nn; k++) n = serial_add(&a, n, &cap, names[k]);
    if (nn > 0) lt_plat_free_list(names, nn);
    if (n == 0) {
        lt_free(a);
        a = NULL;
    }
    *out = a;
    return n;
}

int lt_usb_string(lt_usb_dev *d, int index, char *buf, size_t cap)
{
    lt_u8 tmp[255];
    ULONG got = 0;
    if (buf != NULL && cap > 0) buf[0] = '\0';
    if (d == NULL || buf == NULL || cap == 0 || index <= 0 || index > 255) return LT_USB_EINVAL;
    if (WinUsb_GetDescriptor(d->ifs[0].h, LT_USB_DT_STRING, (UCHAR)index, LT_USB_LANG_EN_US,
                             tmp, (ULONG)sizeof tmp, &got)) {
        return lt_usb_string_from_desc(tmp, (int)got, buf, cap);
    }
    /* language fallback and anything else WinUSB refuses: plain control requests */
    return lt_usb_string_by_control(d, index, buf, cap);
}
