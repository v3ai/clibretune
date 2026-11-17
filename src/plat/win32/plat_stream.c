/* plat_stream.c - Windows byte streams: serial ports (COMn), winsock TCP,
 * the loopback TCP server and lt_plat_wait.
 *
 * PLATFORM FILE: uses Win32 headers via win_priv.h (see the exception noted
 * there). Built only for TARGET=windows. Link with ws2_32.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "win_priv.h"

#include <stdlib.h>
#include <string.h>

struct lt_stream {
    int is_sock;
    SOCKET sock;
    HANDLE h;
};

struct lt_server {
    SOCKET sock;
    int port;
};

static int g_wsa = 0;

static void wsa_init(void)
{
    WSADATA d;
    if (!g_wsa) {
        WSAStartup(MAKEWORD(2, 2), &d);
        g_wsa = 1;
    }
}

static lt_stream *new_sock_stream(SOCKET s)
{
    lt_stream *st = (lt_stream *)lt_calloc(1, sizeof *st);
    st->is_sock = 1;
    st->sock = s;
    st->h = INVALID_HANDLE_VALUE;
    return st;
}

static void set_timeouts(HANDLE h, int timeout_ms)
{
    COMMTIMEOUTS t;
    memset(&t, 0, sizeof t);
    if (timeout_ms <= 0) {
        t.ReadIntervalTimeout = MAXDWORD;
    } else {
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
    }
    t.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(h, &t);
}

static int baud_ok(long b)
{
    return b >= 300 && b <= 4000000L;
}

static int apply_dcb(HANDLE h, long baud)
{
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) return -1;
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    return SetCommState(h, &dcb) ? 0 : -1;
}

lt_stream *lt_serial_open(const char *port, long baud)
{
    char name[64];
    HANDLE h;
    lt_stream *s;
    if (!baud_ok(baud)) {
        lt_err_set("unsupported baud rate %ld", baud);
        return NULL;
    }
    if (strncmp(port, "\\\\.\\", 4) == 0) lt_strlcpy(name, port, sizeof name);
    else lt_snprintf(name, sizeof name, "\\\\.\\%s", port);
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        lt_err_set("cannot open %s (%lu)", port, (unsigned long)GetLastError());
        return NULL;
    }
    if (apply_dcb(h, baud) != 0) {
        lt_err_set("cannot configure %s", port);
        CloseHandle(h);
        return NULL;
    }
    SetupComm(h, 4096, 4096);
    set_timeouts(h, 0);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s = (lt_stream *)lt_calloc(1, sizeof *s);
    s->h = h;
    s->sock = INVALID_SOCKET;
    return s;
}

int lt_serial_set_baud(lt_stream *s, long baud)
{
    if (s == NULL || s->is_sock || !baud_ok(baud)) return -1;
    return apply_dcb(s->h, baud);
}

/* COM port number of a name like "COM12", or 0 */
static int com_number(const char *name)
{
    int n = 0;
    if (strncmp(name, "COM", 3) != 0 || name[3] == '\0') return 0;
    name += 3;
    while (*name >= '0' && *name <= '9' && n < 100000) n = n * 10 + (*name++ - '0');
    return *name == '\0' ? n : 0;
}

/* Ports Windows reports as present: HKLM\HARDWARE\DEVICEMAP\SERIALCOMM lists
 * "\\Device\\Serial0" = "COM3" for every live serial device. Reading the key
 * never opens a port (opening can reset boards that tie DTR to reset). */
static int list_from_registry(char ***names)
{
    HKEY key;
    char **out = NULL;
    int n = 0, i, j;
    DWORD idx;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return -1;
    }
    for (idx = 0; ; idx++) {
        char vname[128], data[64];
        DWORD nl = (DWORD)sizeof vname, dl = (DWORD)sizeof data - 1, type = 0;
        LONG r = RegEnumValueA(key, idx, vname, &nl, NULL, &type, (LPBYTE)data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        data[dl < sizeof data ? dl : sizeof data - 1] = '\0';
        if (com_number(data) == 0) continue;
        out = (char **)lt_realloc(out, (size_t)(n + 1) * sizeof(char *));
        out[n++] = lt_strdup(data);
    }
    RegCloseKey(key);
    /* numeric order: COM2 before COM10 */
    for (i = 1; i < n; i++) {
        char *t = out[i];
        for (j = i; j > 0 && com_number(out[j - 1]) > com_number(t); j--) out[j] = out[j - 1];
        out[j] = t;
    }
    *names = out;
    return n;
}

int lt_serial_list(char ***names)
{
    char **out = NULL;
    int n = 0, i;
    char path[32];
    *names = NULL;
    n = list_from_registry(names);
    if (n >= 0) return n;
    n = 0;
    /* no registry key: probe by opening (ports in use report access denied) */
    for (i = 1; i <= 32; i++) {
        HANDLE h;
        lt_snprintf(path, sizeof path, "\\\\.\\COM%d", i);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE || GetLastError() == ERROR_ACCESS_DENIED) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            out = (char **)lt_realloc(out, (size_t)(n + 1) * sizeof(char *));
            lt_snprintf(path, sizeof path, "COM%d", i);
            out[n++] = lt_strdup(path);
        }
    }
    *names = out;
    return n;
}

lt_stream *lt_tcp_connect(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char pstr[16];
    SOCKET fd = INVALID_SOCKET;
    wsa_init();
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    lt_snprintf(pstr, sizeof pstr, "%d", port);
    if (getaddrinfo(host, pstr, &hints, &res) != 0) {
        lt_err_set("cannot resolve %s", host);
        return NULL;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        u_long nb = 1;
        int r;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        ioctlsocket(fd, (long)FIONBIO, &nb); /* ABI constant is unsigned */
        r = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        if (r != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set w, e;
            struct timeval tv;
            FD_ZERO(&w);
            FD_ZERO(&e);
            FD_SET(fd, &w);
            FD_SET(fd, &e);
            if (timeout_ms < 0) timeout_ms = 0;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            r = select(0, NULL, &w, &e, &tv);
            r = (r > 0 && FD_ISSET(fd, &w)) ? 0 : -1;
        }
        if (r == 0) {
            nb = 0;
            ioctlsocket(fd, (long)FIONBIO, &nb); /* ABI constant is unsigned */
            break;
        }
        closesocket(fd);
        fd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) {
        lt_err_set("cannot connect to %s:%d", host, port);
        return NULL;
    }
    return new_sock_stream(fd);
}

lt_stream *lt_unix_connect(const char *path)
{
    LT_UNUSED(path);
    return NULL;
}

static int sock_wait(SOCKET s, int timeout_ms)
{
    fd_set r;
    struct timeval tv;
    FD_ZERO(&r);
    FD_SET(s, &r);
    if (timeout_ms < 0) timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(0, &r, NULL, NULL, &tv);
}

int lt_stream_read(lt_stream *s, void *buf, int n, int timeout_ms)
{
    if (s == NULL || n <= 0) return -1;
    if (s->is_sock) {
        int r = sock_wait(s->sock, timeout_ms);
        if (r < 0) return -1;
        if (r == 0) return 0;
        r = recv(s->sock, (char *)buf, n, 0);
        return r <= 0 ? -1 : r;
    } else {
        DWORD got = 0;
        set_timeouts(s->h, timeout_ms);
        if (!ReadFile(s->h, buf, (DWORD)n, &got, NULL)) return -1;
        return (int)got;
    }
}

int lt_stream_write(lt_stream *s, const void *buf, int n)
{
    int done = 0;
    if (s == NULL || n < 0) return -1;
    while (done < n) {
        if (s->is_sock) {
            int w = send(s->sock, (const char *)buf + done, n - done, 0);
            if (w <= 0) return -1;
            done += w;
        } else {
            DWORD w = 0;
            if (!WriteFile(s->h, (const char *)buf + done, (DWORD)(n - done), &w, NULL) ||
                w == 0) return -1;
            done += (int)w;
        }
    }
    return n;
}

void lt_stream_flush_input(lt_stream *s)
{
    char tmp[256];
    if (s == NULL) return;
    if (!s->is_sock) {
        PurgeComm(s->h, PURGE_RXCLEAR);
        return;
    }
    while (sock_wait(s->sock, 0) > 0) {
        if (recv(s->sock, tmp, (int)sizeof tmp, 0) <= 0) break;
    }
}

void lt_stream_close(lt_stream *s)
{
    if (s == NULL) return;
    if (s->is_sock) closesocket(s->sock);
    else CloseHandle(s->h);
    lt_free(s);
}

int lt_stream_fd(lt_stream *s)
{
    LT_UNUSED(s);
    return -1;
}

lt_server *lt_server_listen(int port)
{
    struct sockaddr_in sa;
    int len;
    SOCKET fd;
    BOOL one = TRUE;
    lt_server *srv;
    wsa_init();
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return NULL;
    /* no SO_REUSEADDR on Windows: it would allow port hijacking */
    LT_UNUSED(one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&sa, (int)sizeof sa) != 0 || listen(fd, 8) != 0) {
        lt_err_set("cannot listen on port %d", port);
        closesocket(fd);
        return NULL;
    }
    len = (int)sizeof sa;
    getsockname(fd, (struct sockaddr *)&sa, &len);
    srv = (lt_server *)lt_malloc(sizeof *srv);
    srv->sock = fd;
    srv->port = (int)ntohs(sa.sin_port);
    return srv;
}

int lt_server_port(lt_server *srv)
{
    return srv ? srv->port : -1;
}

lt_stream *lt_server_accept(lt_server *srv, int timeout_ms)
{
    SOCKET c;
    if (srv == NULL || sock_wait(srv->sock, timeout_ms) <= 0) return NULL;
    c = accept(srv->sock, NULL, NULL);
    if (c == INVALID_SOCKET) return NULL;
    return new_sock_stream(c);
}

void lt_server_close(lt_server *srv)
{
    if (srv == NULL) return;
    closesocket(srv->sock);
    lt_free(srv);
}

int lt_plat_wait(lt_stream **streams, lt_server **servers, int extra_fd, int timeout_ms)
{
    double end = lt_plat_now_ms() + (double)(timeout_ms < 0 ? 0 : timeout_ms);
    int i;
    LT_UNUSED(extra_fd); /* console stdin is handled by lt_term_read polling */
    for (;;) {
        for (i = 0; streams != NULL && streams[i] != NULL; i++) {
            lt_stream *s = streams[i];
            if (s->is_sock) {
                if (sock_wait(s->sock, 0) > 0) return 1;
            } else {
                COMSTAT cs;
                DWORD errs = 0;
                if (ClearCommError(s->h, &errs, &cs) && cs.cbInQue > 0) return 1;
            }
        }
        for (i = 0; servers != NULL && servers[i] != NULL; i++) {
            if (sock_wait(servers[i]->sock, 0) > 0) return 1;
        }
        if (lt_plat_now_ms() >= end) return 0;
        Sleep(2);
    }
}
