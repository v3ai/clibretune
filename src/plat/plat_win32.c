/* plat_win32.c - native Win32 implementation of plat.h (MSVC / MinGW).
 * Compiled only when _WIN32 && !__COSMOPOLITAN__. Untested on real Windows.
 * Link with: ws2_32 shell32
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#if defined(_WIN32) && !defined(__COSMOPOLITAN__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

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

lt_os lt_plat_os(void) { return LT_OS_WINDOWS; }
const char *lt_plat_os_name(void) { return "Windows"; }

double lt_plat_now_ms(void)
{
    static double freq = 0.0;
    LARGE_INTEGER c;
    if (freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (double)f.QuadPart / 1000.0;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / freq;
}

void lt_plat_sleep_ms(int ms)
{
    if (ms > 0) Sleep((DWORD)ms);
}

void lt_plat_timestamp(char *buf, size_t cap)
{
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    if (cap == 0) return;
    if (tmv == NULL || strftime(buf, cap, "%Y-%m-%d %H:%M:%S", tmv) == 0) {
        lt_strlcpy(buf, "0000-00-00 00:00:00", cap);
    }
}

/* ---- filesystem ---- */

lt_bool lt_plat_is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return LT_FALSE;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? LT_TRUE : LT_FALSE;
}

int lt_plat_mkdir(const char *path)
{
    if (CreateDirectoryA(path, NULL)) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS && lt_plat_is_dir(path)) return 0;
    lt_err_set("mkdir %s failed (%lu)", path, (unsigned long)GetLastError());
    return -1;
}

int lt_plat_mkdirs(const char *path)
{
    char *p;
    size_t i, n;
    int rc;
    if (path == NULL || path[0] == '\0') return -1;
    p = lt_strdup(path);
    n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = '\0';
    for (i = 1; i < n; i++) {
        if (p[i] == '/' || p[i] == '\\') {
            char c = p[i];
            p[i] = '\0';
            if (!lt_plat_is_dir(p)) CreateDirectoryA(p, NULL);
            p[i] = c;
        }
    }
    rc = lt_plat_mkdir(p);
    lt_free(p);
    return rc;
}

int lt_plat_remove(const char *path)
{
    if (lt_plat_is_dir(path)) {
        if (RemoveDirectoryA(path)) return 0;
    } else if (DeleteFileA(path)) {
        return 0;
    }
    lt_err_set("remove %s failed (%lu)", path, (unsigned long)GetLastError());
    return -1;
}

int lt_plat_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) return 0;
    lt_err_set("rename %s failed (%lu)", from, (unsigned long)GetLastError());
    return -1;
}

int lt_plat_copy_file(const char *from, const char *to)
{
    FILE *in, *out;
    char buf[4096];
    size_t n;
    int rc = 0;
    in = fopen(from, "rb");
    if (in == NULL) {
        lt_err_set("cannot open %s", from);
        return -1;
    }
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        lt_err_set("cannot create %s", to);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (fclose(out) != 0) rc = -1;
    fclose(in);
    return rc;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int lt_plat_list_dir(const char *path, char ***names)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *pat;
    char **arr = NULL;
    int n = 0, cap = 0;
    *names = NULL;
    pat = lt_path_join(path, "*");
    h = FindFirstFileA(pat, &fd);
    lt_free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        lt_err_set("cannot list %s", path);
        return -1;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            arr = (char **)lt_realloc(arr, (size_t)cap * sizeof(char *));
        }
        arr[n++] = lt_strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n > 1) qsort(arr, (size_t)n, sizeof(char *), cmp_names);
    *names = arr;
    return n;
}

void lt_plat_free_list(char **names, int n)
{
    int i;
    for (i = 0; i < n; i++) lt_free(names[i]);
    lt_free(names);
}

char *lt_plat_getenv(const char *name)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') return NULL;
    return lt_strdup(v);
}

char *lt_plat_config_dir(void)
{
    char *base = lt_plat_getenv("APPDATA");
    char *dir;
    if (base == NULL) base = lt_strdup(".");
    dir = lt_path_join(base, "LibreTune");
    lt_free(base);
    lt_plat_mkdirs(dir);
    return dir;
}

char *lt_plat_projects_dir(void)
{
    char *h = lt_plat_getenv("USERPROFILE");
    char *docs, *dir;
    if (h == NULL) h = lt_strdup(".");
    docs = lt_path_join(h, "Documents");
    dir = lt_path_join(docs, "LibreTune Projects");
    lt_free(docs);
    lt_free(h);
    return dir;
}

char *lt_plat_exe_path(void)
{
    char buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return NULL;
    return lt_strdup(buf);
}

/* ---- streams ---- */

static lt_stream *stream_new_sock(SOCKET s)
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

int lt_serial_list(char ***names)
{
    char **out = NULL;
    int n = 0, i;
    char path[32];
    *names = NULL;
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
        ioctlsocket(fd, FIONBIO, &nb);
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
            ioctlsocket(fd, FIONBIO, &nb);
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
    return stream_new_sock(fd);
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

int lt_stream_read_full(lt_stream *s, void *buf, int n, int timeout_ms)
{
    int total = 0;
    double end = lt_plat_now_ms() + (double)timeout_ms;
    while (total < n) {
        double left = end - lt_plat_now_ms();
        int r;
        if (left < 0) left = 0;
        r = lt_stream_read(s, (char *)buf + total, n - total, (int)left);
        if (r < 0) return -1;
        total += r;
        if (r == 0 && end - lt_plat_now_ms() <= 0) break;
    }
    return total;
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

/* ---- server ---- */

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

int lt_server_port(lt_server *srv) { return srv ? srv->port : -1; }

lt_stream *lt_server_accept(lt_server *srv, int timeout_ms)
{
    SOCKET c;
    if (srv == NULL || sock_wait(srv->sock, timeout_ms) <= 0) return NULL;
    c = accept(srv->sock, NULL, NULL);
    if (c == INVALID_SOCKET) return NULL;
    return stream_new_sock(c);
}

void lt_server_close(lt_server *srv)
{
    if (srv == NULL) return;
    closesocket(srv->sock);
    lt_free(srv);
}

/* Polls in short steps because serial HANDLEs cannot join select(). */
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

/* ---- terminal ---- */

static DWORD g_in_mode, g_out_mode;
static int g_raw = 0;
static int g_last_cols = 0, g_last_rows = 0;

lt_bool lt_term_isatty(void)
{
    DWORD m;
    return (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m) &&
            GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m)) ? LT_TRUE : LT_FALSE;
}

int lt_term_raw_begin(void)
{
    HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m;
    if (g_raw) return 0;
    if (!GetConsoleMode(hi, &g_in_mode) || !GetConsoleMode(ho, &g_out_mode)) return -1;
    m = g_in_mode & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    m |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(hi, m);
    SetConsoleMode(ho, g_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    g_raw = 1;
    lt_term_size(&g_last_cols, &g_last_rows);
    return 0;
}

void lt_term_raw_end(void)
{
    if (!g_raw) return;
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_in_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_out_mode);
    g_raw = 0;
}

void lt_term_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    int c = 80, r = 24;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) {
        c = ci.srWindow.Right - ci.srWindow.Left + 1;
        r = ci.srWindow.Bottom - ci.srWindow.Top + 1;
    }
    if (cols) *cols = c;
    if (rows) *rows = r;
}

int lt_term_read(char *buf, int n, int timeout_ms)
{
    HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
    DWORD w, got = 0;
    if (timeout_ms < 0) timeout_ms = 0;
    w = WaitForSingleObject(hi, (DWORD)timeout_ms);
    if (w == WAIT_TIMEOUT) return 0;
    if (w != WAIT_OBJECT_0) return -1;
    /* with VT input, only key events produce bytes; drain non-key events */
    {
        DWORD cnt = 0;
        INPUT_RECORD rec;
        DWORD nr;
        if (GetNumberOfConsoleInputEvents(hi, &cnt) && cnt > 0 &&
            PeekConsoleInputA(hi, &rec, 1, &nr) && nr > 0 && rec.EventType != KEY_EVENT) {
            ReadConsoleInputA(hi, &rec, 1, &nr);
            return 0;
        }
    }
    if (!ReadFile(hi, buf, (DWORD)n, &got, NULL)) return -1;
    return got == 0 ? -1 : (int)got;
}

void lt_term_write(const char *buf, int n)
{
    DWORD w;
    int done = 0;
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    while (done < n) {
        if (!WriteFile(ho, buf + done, (DWORD)(n - done), &w, NULL) || w == 0) return;
        done += (int)w;
    }
}

lt_bool lt_term_resized(void)
{
    int c, r;
    lt_term_size(&c, &r);
    if (c != g_last_cols || r != g_last_rows) {
        g_last_cols = c;
        g_last_rows = r;
        return LT_TRUE;
    }
    return LT_FALSE;
}

int lt_plat_open_url(const char *url)
{
    HINSTANCE r = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)r > 32) ? 0 : -1;
}

#else
typedef int lt_plat_win32_unused_;
#endif /* _WIN32 && !__COSMOPOLITAN__ */
