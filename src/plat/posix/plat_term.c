/* plat_term.c - POSIX raw terminal support for the tty GUI backend, plus
 * the detached "open URL" helper.
 *
 * PLATFORM FILE: includes POSIX headers (termios.h, unistd.h, sys/ioctl.h,
 * sys/wait.h, fcntl.h). Feature-test macros come from the build. The #ifdef
 * lines only probe OS-defined constants (SIGWINCH, TIOCGWINSZ).
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
#include <sys/ioctl.h>
#include <sys/wait.h>

static struct termios g_saved_tio;
static int g_raw_active = 0;
static volatile sig_atomic_t g_winch = 0;

#ifdef SIGWINCH
static void on_winch(int sig)
{
    LT_UNUSED(sig);
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
    r = lt_posix_wait_fd(0, 0, timeout_ms);
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
                lt_posix_wait_fd(1, 1, 50);
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

int lt_posix_spawn_open(const char *cmd, const char *url)
{
    pid_t pid;
    int status = 0;
    if (url == NULL || url[0] == '-') return -1;
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
