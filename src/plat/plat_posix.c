/* plat_posix.c - POSIX implementation of plat.h (also used by cosmopolitan).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#if defined(_WIN32) && !defined(__COSMOPOLITAN__)
/* native Windows: see plat_win32.c */
typedef int lt_plat_posix_unused_;
#else

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif
#endif

#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/wait.h>

#ifdef __COSMOPOLITAN__
#include <libc/dce.h>
#include <cosmo.h>
#endif

struct lt_stream {
    int fd;
    int is_sock;
};

struct lt_server {
    int fd;
    int port;
};

/* ------------------------------------------------------------------ */
/* OS identity */

lt_os lt_plat_os(void)
{
#ifdef __COSMOPOLITAN__
    if (IsWindows()) return LT_OS_WINDOWS;
    if (IsXnu()) return LT_OS_MACOS;
    if (IsLinux()) return LT_OS_LINUX;
    return LT_OS_BSD;
#elif defined(__APPLE__)
    return LT_OS_MACOS;
#elif defined(__linux__)
    return LT_OS_LINUX;
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
    return LT_OS_BSD;
#else
    return LT_OS_UNIX;
#endif
}

const char *lt_plat_os_name(void)
{
    switch (lt_plat_os()) {
    case LT_OS_LINUX: return "Linux";
    case LT_OS_MACOS: return "macOS";
    case LT_OS_BSD: return "BSD";
    case LT_OS_WINDOWS: return "Windows";
    default: break;
    }
    return "Unix";
}

/* ------------------------------------------------------------------ */
/* time */

double lt_plat_now_ms(void)
{
    static double last = 0.0;
    double t;
#if defined(CLOCK_MONOTONIC) && !defined(__APPLE__)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        t = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    } else
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        t = (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
    }
    if (t < last) t = last;
    last = t;
    return t;
}

void lt_plat_sleep_ms(int ms)
{
    struct timeval tv;
    if (ms <= 0) return;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(0, NULL, NULL, NULL, &tv);
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

/* ------------------------------------------------------------------ */
/* filesystem */

int lt_plat_mkdir(const char *path)
{
    if (mkdir(path, 0777) == 0) return 0;
    if (errno == EEXIST && lt_plat_is_dir(path)) return 0;
    lt_err_set("mkdir %s: %s", path, strerror(errno));
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
            if (!lt_plat_is_dir(p)) mkdir(p, 0777); /* errors surface at the end */
            p[i] = c;
        }
    }
    rc = lt_plat_mkdir(p);
    lt_free(p);
    return rc;
}

lt_bool lt_plat_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return LT_FALSE;
    return S_ISDIR(st.st_mode) ? LT_TRUE : LT_FALSE;
}

int lt_plat_remove(const char *path)
{
    if (remove(path) == 0) return 0;
    if (rmdir(path) == 0) return 0;
    lt_err_set("remove %s: %s", path, strerror(errno));
    return -1;
}

int lt_plat_rename(const char *from, const char *to)
{
    if (rename(from, to) == 0) return 0;
    lt_err_set("rename %s: %s", from, strerror(errno));
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
    if (ferror(in)) rc = -1;
    if (fclose(out) != 0) rc = -1;
    fclose(in);
    if (rc != 0) lt_err_set("copy %s -> %s failed", from, to);
    return rc;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int lt_plat_list_dir(const char *path, char ***names)
{
    DIR *d;
    struct dirent *e;
    char **arr = NULL;
    int n = 0, cap = 0;
    *names = NULL;
    d = opendir(path);
    if (d == NULL) {
        lt_err_set("opendir %s: %s", path, strerror(errno));
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            arr = (char **)lt_realloc(arr, (size_t)cap * sizeof(char *));
        }
        arr[n++] = lt_strdup(e->d_name);
    }
    closedir(d);
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

static char *home_dir(void)
{
    char *h = lt_plat_getenv("HOME");
    if (h == NULL) h = lt_plat_getenv("USERPROFILE");
    if (h == NULL) h = lt_strdup(".");
    return h;
}

char *lt_plat_config_dir(void)
{
    char *base, *dir;
    lt_os os = lt_plat_os();
    if (os == LT_OS_WINDOWS) {
        base = lt_plat_getenv("APPDATA");
        if (base == NULL) {
            char *h = home_dir();
            base = lt_path_join(h, "AppData/Roaming");
            lt_free(h);
        }
        dir = lt_path_join(base, "LibreTune");
    } else if (os == LT_OS_MACOS) {
        char *h = home_dir();
        base = lt_path_join(h, "Library/Application Support");
        lt_free(h);
        dir = lt_path_join(base, "LibreTune");
    } else {
        base = lt_plat_getenv("XDG_CONFIG_HOME");
        if (base == NULL) {
            char *h = home_dir();
            base = lt_path_join(h, ".config");
            lt_free(h);
        }
        dir = lt_path_join(base, "libretune");
    }
    lt_free(base);
    lt_plat_mkdirs(dir);
    return dir;
}

char *lt_plat_projects_dir(void)
{
    char *h = home_dir();
    char *dir;
    if (lt_plat_os() == LT_OS_WINDOWS) {
        char *docs = lt_path_join(h, "Documents");
        dir = lt_path_join(docs, "LibreTune Projects");
        lt_free(docs);
    } else {
        dir = lt_path_join(h, "LibreTune Projects");
    }
    lt_free(h);
    return dir;
}

char *lt_plat_exe_path(void)
{
#ifdef __COSMOPOLITAN__
    const char *p = GetProgramExecutableName();
    if (p != NULL && p[0] != '\0') return lt_strdup(p);
    return NULL;
#else
    static const char *const links[] = {
        "/proc/self/exe", "/proc/curproc/file", "/proc/self/path/a.out"
    };
    char buf[1024];
    size_t i;
    for (i = 0; i < LT_COUNTOF(links); i++) {
        ssize_t r = readlink(links[i], buf, sizeof buf - 1);
        if (r > 0) {
            buf[r] = '\0';
            return lt_strdup(buf);
        }
    }
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* streams */

static lt_stream *stream_new(int fd, int is_sock)
{
    lt_stream *s = (lt_stream *)lt_malloc(sizeof *s);
    s->fd = fd;
    s->is_sock = is_sock;
    return s;
}

static void ignore_sigpipe(void)
{
#ifdef SIGPIPE
    static int done = 0;
    if (!done) {
        done = 1;
        signal(SIGPIPE, SIG_IGN);
    }
#endif
}

static void set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    if (on) fl |= O_NONBLOCK;
    else fl &= ~O_NONBLOCK;
    fcntl(fd, F_SETFL, fl);
}

/* wait for fd readable (rw=0) or writable (rw=1); >0 ready, 0 timeout, -1 err */
static int wait_fd(int fd, int rw, int timeout_ms)
{
    fd_set set;
    struct timeval tv;
    int r;
    if (fd < 0 || fd >= FD_SETSIZE) return -1;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (timeout_ms < 0) timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(fd + 1, rw ? NULL : &set, rw ? &set : NULL, NULL, &tv);
    if (r < 0 && errno == EINTR) return 0;
    return r;
}

static int baud_const(long baud, speed_t *out)
{
    switch (baud) {
    case 1200: *out = B1200; return 0;
    case 2400: *out = B2400; return 0;
    case 4800: *out = B4800; return 0;
    case 9600: *out = B9600; return 0;
    case 19200: *out = B19200; return 0;
    case 38400: *out = B38400; return 0;
#ifdef B57600
    case 57600: *out = B57600; return 0;
#endif
#ifdef B115200
    case 115200: *out = B115200; return 0;
#endif
#ifdef B230400
    case 230400: *out = B230400; return 0;
#endif
#ifdef B460800
    case 460800: *out = B460800; return 0;
#endif
#ifdef B500000
    case 500000: *out = B500000; return 0;
#endif
#ifdef B576000
    case 576000: *out = B576000; return 0;
#endif
#ifdef B921600
    case 921600: *out = B921600; return 0;
#endif
#ifdef B1000000
    case 1000000: *out = B1000000; return 0;
#endif
#ifdef B1500000
    case 1500000: *out = B1500000; return 0;
#endif
#ifdef B2000000
    case 2000000: *out = B2000000; return 0;
#endif
    default: break;
    }
    return -1;
}

static int apply_baud(int fd, long baud)
{
    struct termios tio;
    speed_t sp;
    if (baud_const(baud, &sp) != 0) {
        lt_err_set("unsupported baud rate %ld", baud);
        return -1;
    }
    if (tcgetattr(fd, &tio) != 0) {
        lt_err_set("tcgetattr: %s", strerror(errno));
        return -1;
    }
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        lt_err_set("tcsetattr: %s", strerror(errno));
        return -1;
    }
    return 0;
}

lt_stream *lt_serial_open(const char *port, long baud)
{
    int fd;
    struct termios tio;
    speed_t sp;
    if (baud_const(baud, &sp) != 0) {
        lt_err_set("unsupported baud rate %ld", baud);
        return NULL;
    }
    /* TODO(cosmo/Windows): COMn names are passed straight to open(); verify
     * that cosmo's NT layer maps them (it may need "\\\\.\\COMn"). */
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        lt_err_set("cannot open %s: %s", port, strerror(errno));
        return NULL;
    }
    if (tcgetattr(fd, &tio) != 0) {
        lt_err_set("%s is not a serial port: %s", port, strerror(errno));
        close(fd);
        return NULL;
    }
    tio.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
                               IXON | IXOFF | IXANY);
    tio.c_oflag &= ~(tcflag_t)OPOST;
    tio.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    tio.c_cflag &= ~(tcflag_t)CRTSCTS;
#endif
    tio.c_cflag |= (tcflag_t)(CS8 | CLOCAL | CREAD);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        lt_err_set("tcsetattr %s: %s", port, strerror(errno));
        close(fd);
        return NULL;
    }
    set_nonblock(fd, 0);
    tcflush(fd, TCIOFLUSH);
    return stream_new(fd, 0);
}

int lt_serial_set_baud(lt_stream *s, long baud)
{
    if (s == NULL) return -1;
    return apply_baud(s->fd, baud);
}

static int add_name(char ***arr, int *n, int *cap, const char *name)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = (char **)lt_realloc(*arr, (size_t)*cap * sizeof(char *));
    }
    (*arr)[(*n)++] = lt_strdup(name);
    return 0;
}

int lt_serial_list(char ***names)
{
    static const char *const prefixes[] = {
        "ttyUSB", "ttyACM", "cu.usb", "tty.usb", "cuaU", "ttyU"
    };
    static const char *const fixed[] = { "ttyS0", "ttyS1", "ttyS2", "ttyS3" };
    char **dev = NULL;
    char **out = NULL;
    int nd, n = 0, cap = 0, i;
    size_t j;
    char path[300];

    *names = NULL;
#ifdef __COSMOPOLITAN__
    if (IsWindows()) {
        /* TODO: enumerate real COM ports; list the usual candidates. */
        for (i = 1; i <= 16; i++) {
            lt_snprintf(path, sizeof path, "COM%d", i);
            add_name(&out, &n, &cap, path);
        }
        *names = out;
        return n;
    }
#endif
    nd = lt_plat_list_dir("/dev", &dev);
    if (nd < 0) return 0;
    for (i = 0; i < nd; i++) {
        int match = 0;
        for (j = 0; j < LT_COUNTOF(prefixes) && !match; j++) {
            if (lt_str_starts(dev[i], prefixes[j])) match = 1;
        }
        for (j = 0; j < LT_COUNTOF(fixed) && !match; j++) {
            if (strcmp(dev[i], fixed[j]) == 0) match = 1;
        }
        if (match) {
            lt_snprintf(path, sizeof path, "/dev/%s", dev[i]);
            add_name(&out, &n, &cap, path);
        }
    }
    lt_plat_free_list(dev, nd);
    *names = out;
    return n;
}

lt_stream *lt_tcp_connect(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char pstr[16];
    int gai, fd = -1;
    ignore_sigpipe();
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    lt_snprintf(pstr, sizeof pstr, "%d", port);
    gai = getaddrinfo(host, pstr, &hints, &res);
    if (gai != 0) {
        lt_err_set("cannot resolve %s: %s", host, gai_strerror(gai));
        return NULL;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int r;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_nonblock(fd, 1);
        r = connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        if (r != 0 && (errno == EINPROGRESS || errno == EINTR)) {
            r = wait_fd(fd, 1, timeout_ms);
            if (r > 0) {
                int err = 0;
                socklen_t len = (socklen_t)sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &len) != 0) err = errno;
                if (err == 0) r = 0;
                else {
                    errno = err;
                    r = -1;
                }
            } else {
                if (r == 0) errno = ETIMEDOUT;
                r = -1;
            }
        }
        if (r == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        lt_err_set("cannot connect to %s:%d: %s", host, port, strerror(errno));
        return NULL;
    }
    set_nonblock(fd, 0);
#ifdef TCP_NODELAY
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&one, (socklen_t)sizeof one);
    }
#endif
    return stream_new(fd, 1);
}

lt_stream *lt_unix_connect(const char *path)
{
    struct sockaddr_un sa;
    int fd;
    if (path == NULL || strlen(path) >= sizeof sa.sun_path) return NULL;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0) {
        lt_err_set("cannot connect to %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    ignore_sigpipe();
    return stream_new(fd, 1);
}

int lt_stream_read(lt_stream *s, void *buf, int n, int timeout_ms)
{
    int r;
    ssize_t got;
    if (s == NULL || n <= 0) return -1;
    r = wait_fd(s->fd, 0, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    got = read(s->fd, buf, (size_t)n);
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        return -1;
    }
    if (got == 0) return s->is_sock ? -1 : 0;
    return (int)got;
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
        ssize_t w = write(s->fd, (const char *)buf + done, (size_t)(n - done));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                wait_fd(s->fd, 1, 100);
                continue;
            }
            return -1;
        }
        done += (int)w;
    }
    return n;
}

void lt_stream_flush_input(lt_stream *s)
{
    char tmp[256];
    if (s == NULL) return;
    if (!s->is_sock) {
        tcflush(s->fd, TCIFLUSH);
        return;
    }
    while (wait_fd(s->fd, 0, 0) > 0) {
        if (read(s->fd, tmp, sizeof tmp) <= 0) break;
    }
}

void lt_stream_close(lt_stream *s)
{
    if (s == NULL) return;
    close(s->fd);
    lt_free(s);
}

int lt_stream_fd(lt_stream *s)
{
    return s ? s->fd : -1;
}

/* ------------------------------------------------------------------ */
/* server */

lt_server *lt_server_listen(int port)
{
    struct sockaddr_in sa;
    socklen_t len;
    int fd, one = 1;
    lt_server *srv;
    ignore_sigpipe();
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        lt_err_set("socket: %s", strerror(errno));
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&one, (socklen_t)sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0 || listen(fd, 8) != 0) {
        lt_err_set("cannot listen on port %d: %s", port, strerror(errno));
        close(fd);
        return NULL;
    }
    len = (socklen_t)sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
        close(fd);
        return NULL;
    }
    srv = (lt_server *)lt_malloc(sizeof *srv);
    srv->fd = fd;
    srv->port = (int)ntohs(sa.sin_port);
    return srv;
}

int lt_server_port(lt_server *srv)
{
    return srv ? srv->port : -1;
}

lt_stream *lt_server_accept(lt_server *srv, int timeout_ms)
{
    int fd;
    if (srv == NULL) return NULL;
    if (wait_fd(srv->fd, 0, timeout_ms) <= 0) return NULL;
    fd = accept(srv->fd, NULL, NULL);
    if (fd < 0) return NULL;
#ifdef TCP_NODELAY
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&one, (socklen_t)sizeof one);
    }
#endif
    return stream_new(fd, 1);
}

void lt_server_close(lt_server *srv)
{
    if (srv == NULL) return;
    close(srv->fd);
    lt_free(srv);
}

int lt_plat_wait(lt_stream **streams, lt_server **servers, int extra_fd, int timeout_ms)
{
    fd_set set;
    struct timeval tv;
    int maxfd = -1, i, r;
    FD_ZERO(&set);
    for (i = 0; streams != NULL && streams[i] != NULL; i++) {
        int fd = streams[i]->fd;
        if (fd >= 0 && fd < FD_SETSIZE) {
            FD_SET(fd, &set);
            if (fd > maxfd) maxfd = fd;
        }
    }
    for (i = 0; servers != NULL && servers[i] != NULL; i++) {
        int fd = servers[i]->fd;
        if (fd >= 0 && fd < FD_SETSIZE) {
            FD_SET(fd, &set);
            if (fd > maxfd) maxfd = fd;
        }
    }
    if (extra_fd >= 0 && extra_fd < FD_SETSIZE) {
        FD_SET(extra_fd, &set);
        if (extra_fd > maxfd) maxfd = extra_fd;
    }
    if (timeout_ms < 0) timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(maxfd + 1, maxfd >= 0 ? &set : NULL, NULL, NULL, &tv);
    return r > 0 ? r : 0;
}

/* ------------------------------------------------------------------ */
/* terminal */

static struct termios g_saved_tio;
static int g_raw_active = 0;
static volatile sig_atomic_t g_winch = 0;

#ifdef SIGWINCH
static void on_winch(int sig)
{
    (void)sig;
    g_winch = 1;
}
#endif

lt_bool lt_term_isatty(void)
{
    return (isatty(0) && isatty(1)) ? LT_TRUE : LT_FALSE;
}

int lt_term_raw_begin(void)
{
    struct termios t;
    if (g_raw_active) return 0;
    if (tcgetattr(0, &g_saved_tio) != 0) {
        lt_err_set("stdin is not a terminal");
        return -1;
    }
    t = g_saved_tio;
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t.c_cflag |= (tcflag_t)CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) {
        lt_err_set("tcsetattr: %s", strerror(errno));
        return -1;
    }
    g_raw_active = 1;
#ifdef SIGWINCH
    signal(SIGWINCH, on_winch);
#endif
    return 0;
}

void lt_term_raw_end(void)
{
    if (!g_raw_active) return;
    tcsetattr(0, TCSANOW, &g_saved_tio);
    g_raw_active = 0;
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    long n;
    if (v == NULL || !lt_parse_long(v, &n) || n <= 0 || n > 10000) return def;
    return (int)n;
}

void lt_term_size(int *cols, int *rows)
{
    int c = 0, r = 0;
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        c = (int)ws.ws_col;
        r = (int)ws.ws_row;
    }
#endif
    if (c <= 0) c = env_int("COLUMNS", 80);
    if (r <= 0) r = env_int("LINES", 24);
    if (cols) *cols = c;
    if (rows) *rows = r;
}

int lt_term_read(char *buf, int n, int timeout_ms)
{
    int r;
    ssize_t got;
    r = wait_fd(0, 0, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    got = read(0, buf, (size_t)n);
    if (got < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (got == 0) return -1;
    return (int)got;
}

void lt_term_write(const char *buf, int n)
{
    int done = 0;
    while (done < n) {
        ssize_t w = write(1, buf + done, (size_t)(n - done));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                wait_fd(1, 1, 50);
                continue;
            }
            return;
        }
        done += (int)w;
    }
}

lt_bool lt_term_resized(void)
{
    if (g_winch) {
        g_winch = 0;
        return LT_TRUE;
    }
    return LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* misc */

int lt_plat_open_url(const char *url)
{
    pid_t pid;
    const char *cmd = "xdg-open";
    int status = 0;
    lt_os os = lt_plat_os();
    if (url == NULL || url[0] == '-') return -1;
    if (os == LT_OS_MACOS) cmd = "open";
    else if (os == LT_OS_WINDOWS) cmd = "explorer";
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) {
                dup2(fd, 0);
                dup2(fd, 1);
                dup2(fd, 2);
            }
            execlp(cmd, cmd, url, (char *)NULL);
            _exit(127);
        }
        _exit(pid2 < 0 ? 1 : 0);
    }
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

#endif /* !(_WIN32 && !__COSMOPOLITAN__) */
