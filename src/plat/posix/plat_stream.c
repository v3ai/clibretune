/* plat_stream.c - POSIX byte streams: serial ports, TCP, unix sockets, the
 * loopback TCP server and lt_plat_wait.
 *
 * PLATFORM FILE: includes POSIX headers (termios.h, sys/socket.h, netdb.h,
 * ...). Feature-test macros come from the build (tools/target.sh). The #ifdef
 * lines below only probe OS-defined constants (baud rates, TCP_NODELAY), never
 * the compiler or OS identity.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

struct lt_stream {
    int fd;
    int is_sock;
};

struct lt_server {
    int fd;
    int port;
};

static lt_stream *new_stream(int fd, int is_sock)
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

int lt_posix_wait_fd(int fd, int rw, int timeout_ms)
{
    fd_set set;
    struct timeval tv;
    int r;
    if (fd < 0 || fd >= FD_SETSIZE) return -1;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (timeout_ms < 0) timeout_ms = 0;
    tv.tv_sec = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
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
    return new_stream(fd, 0);
}

int lt_serial_set_baud(lt_stream *s, long baud)
{
    if (s == NULL) return -1;
    return apply_baud(s->fd, baud);
}

static void set_nodelay(int fd)
{
#ifdef TCP_NODELAY
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&one, (socklen_t)sizeof one);
#else
    LT_UNUSED(fd);
#endif
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
            r = lt_posix_wait_fd(fd, 1, timeout_ms);
            if (r > 0) {
                int err = 0;
                socklen_t len = (socklen_t)sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &len) != 0) err = errno;
                if (err == 0) {
                    r = 0;
                } else {
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
    set_nodelay(fd);
    return new_stream(fd, 1);
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
    lt_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    if (connect(fd, (struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0) {
        lt_err_set("cannot connect to %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    ignore_sigpipe();
    return new_stream(fd, 1);
}

int lt_stream_read(lt_stream *s, void *buf, int n, int timeout_ms)
{
    int r;
    ssize_t got;
    if (s == NULL || n <= 0) return -1;
    r = lt_posix_wait_fd(s->fd, 0, timeout_ms);
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

int lt_stream_write(lt_stream *s, const void *buf, int n)
{
    int done = 0;
    if (s == NULL || n < 0) return -1;
    while (done < n) {
        ssize_t w = write(s->fd, (const char *)buf + done, (size_t)(n - done));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                lt_posix_wait_fd(s->fd, 1, 100);
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
    while (lt_posix_wait_fd(s->fd, 0, 0) > 0) {
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

/* ---- server ---- */

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
    if (lt_posix_wait_fd(srv->fd, 0, timeout_ms) <= 0) return NULL;
    fd = accept(srv->fd, NULL, NULL);
    if (fd < 0) return NULL;
    set_nodelay(fd);
    return new_stream(fd, 1);
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
    tv.tv_sec = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    r = select(maxfd + 1, maxfd >= 0 ? &set : NULL, NULL, NULL, &tv);
    return r > 0 ? r : 0;
}
