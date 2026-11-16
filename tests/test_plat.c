/* test_plat.c - tests for src/plat. Uses POSIX pty only when available. */
#if !defined(_WIN32) || defined(__COSMOPOLITAN__)
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#define LT_TEST_HAVE_PTY 1
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#endif

#include "lt_test.h"
#include "../src/plat/plat.h"
#include <stdlib.h>
#include <string.h>

static char tmp_root[256];

LT_TEST(test_time)
{
    double a = lt_plat_now_ms(), b;
    char ts[32];
    lt_plat_sleep_ms(30);
    b = lt_plat_now_ms();
    LT_CHECK(b >= a + 20.0);
    LT_CHECK(b < a + 2000.0);
    lt_plat_timestamp(ts, sizeof ts);
    LT_CHECK(strlen(ts) == 19);
    LT_CHECK(ts[4] == '-' && ts[10] == ' ' && ts[13] == ':');
    LT_CHECK(lt_plat_os_name() != NULL);
}

LT_TEST(test_fs)
{
    char *d1, *d2, *f1, *f2, *f3;
    char **names = NULL;
    int n;
    d1 = lt_path_join(tmp_root, "a");
    d2 = lt_path_join(d1, "b/c");
    LT_CHECK(lt_plat_mkdirs(d2) == 0);
    LT_CHECK(lt_plat_is_dir(d2));
    LT_CHECK(lt_plat_mkdirs(d2) == 0);
    LT_CHECK(!lt_plat_is_dir("/definitely/not/here"));
    f1 = lt_path_join(d1, "zeta.txt");
    f2 = lt_path_join(d1, "alpha.txt");
    f3 = lt_path_join(d1, "copy.txt");
    LT_CHECK(lt_file_write(f1, "hello", 5) == 0);
    LT_CHECK(lt_plat_copy_file(f1, f2) == 0);
    LT_CHECK(lt_plat_rename(f2, f3) == 0);
    LT_CHECK(lt_file_exists(f3) && !lt_file_exists(f2));
    n = lt_plat_list_dir(d1, &names);
    LT_CHECK(n == 3);
    if (n == 3) {
        LT_CHECK_STR(names[0], "b");
        LT_CHECK_STR(names[1], "copy.txt");
        LT_CHECK_STR(names[2], "zeta.txt");
    }
    lt_plat_free_list(names, n > 0 ? n : 0);
    LT_CHECK(lt_plat_list_dir("/definitely/not/here", &names) < 0);
    LT_CHECK(lt_plat_remove(f1) == 0);
    LT_CHECK(lt_plat_remove(f3) == 0);
    LT_CHECK(lt_plat_remove(d2) == 0);
    LT_CHECK(!lt_plat_is_dir(d2));
    lt_free(d1); lt_free(d2); lt_free(f1); lt_free(f2); lt_free(f3);
}

LT_TEST(test_dirs)
{
    char *c = lt_plat_projects_dir();
    char *e = lt_plat_exe_path();
    LT_CHECK(c != NULL && c[0] != '\0');
    lt_free(c);
    LT_CHECK(e != NULL);
    lt_free(e);
    {
        int cols = 0, rows = 0;
        lt_term_size(&cols, &rows);
        LT_CHECK(cols > 0 && rows > 0);
    }
}

LT_TEST(test_tcp_echo)
{
    lt_server *srv = lt_server_listen(0);
    lt_stream *cl, *sv;
    lt_server *srvs[2];
    lt_stream *sts[2];
    char buf[16];
    int port;
    LT_CHECK(srv != NULL);
    if (srv == NULL) return;
    port = lt_server_port(srv);
    LT_CHECK(port > 0);
    LT_CHECK(lt_server_accept(srv, 10) == NULL);
    cl = lt_tcp_connect("127.0.0.1", port, 2000);
    LT_CHECK(cl != NULL);
    sv = lt_server_accept(srv, 2000);
    LT_CHECK(sv != NULL);
    if (cl == NULL || sv == NULL) return;
    LT_CHECK(lt_stream_write(cl, "ping", 4) == 4);
    sts[0] = sv; sts[1] = NULL;
    LT_CHECK(lt_plat_wait(sts, NULL, -1, 2000) > 0);
    LT_CHECK(lt_stream_read_full(sv, buf, 4, 2000) == 4);
    LT_CHECK(memcmp(buf, "ping", 4) == 0);
    LT_CHECK(lt_stream_write(sv, "pong!", 5) == 5);
    LT_CHECK(lt_stream_read_full(cl, buf, 5, 2000) == 5);
    LT_CHECK(memcmp(buf, "pong!", 5) == 0);
    LT_CHECK(lt_stream_read(cl, buf, 4, 0) == 0);
    LT_CHECK(lt_stream_read_full(cl, buf, 4, 50) == 0);
    srvs[0] = srv; srvs[1] = NULL;
    LT_CHECK(lt_plat_wait(NULL, srvs, -1, 10) == 0);
    lt_stream_close(sv);
    LT_CHECK(lt_stream_read(cl, buf, 4, 1000) == -1);
    lt_stream_close(cl);
    lt_server_close(srv);
    /* connection refused */
    LT_CHECK(lt_tcp_connect("127.0.0.1", port, 500) == NULL);
}

#ifdef LT_TEST_HAVE_PTY
LT_TEST(test_serial_pty)
{
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    char *name;
    lt_stream *s;
    char buf[16];
    if (m < 0) return;
    if (grantpt(m) != 0 || unlockpt(m) != 0 || (name = ptsname(m)) == NULL) {
        close(m);
        return;
    }
    s = lt_serial_open(name, 115200);
    LT_CHECK(s != NULL);
    if (s != NULL) {
        LT_CHECK(lt_serial_set_baud(s, 9600) == 0);
        LT_CHECK(lt_serial_set_baud(s, 115200) == 0);
        LT_CHECK(lt_serial_set_baud(s, 12345) < 0);
        LT_CHECK(write(m, "\x01\r\n\xff", 4) == 4);
        LT_CHECK(lt_stream_read_full(s, buf, 4, 1000) == 4);
        LT_CHECK(memcmp(buf, "\x01\r\n\xff", 4) == 0);
        LT_CHECK(lt_stream_write(s, "ab\n", 3) == 3);
        {
            int got = 0, r;
            while (got < 3 && (r = (int)read(m, buf + got, (size_t)(3 - got))) > 0) got += r;
            LT_CHECK(got == 3 && memcmp(buf, "ab\n", 3) == 0);
        }
        LT_CHECK(lt_stream_read(s, buf, 4, 0) == 0);
        lt_stream_close(s);
    }
    close(m);
#endif
}
#endif

LT_TEST(test_serial_list)
{
    char **names = NULL;
    int n = lt_serial_list(&names);
    LT_CHECK(n >= 0);
    lt_plat_free_list(names, n);
    LT_CHECK(lt_serial_open("/dev/null/nope", 115200) == NULL);
}

int main(void)
{
    char *base;
    lt_plat_mkdirs("build");
    lt_snprintf(tmp_root, sizeof tmp_root, "build/test_plat_tmp");
    base = lt_strdup(tmp_root);
    lt_plat_mkdirs(base);
    lt_free(base);
    LT_RUN(test_time);
    LT_RUN(test_fs);
    LT_RUN(test_dirs);
    LT_RUN(test_tcp_echo);
#ifdef LT_TEST_HAVE_PTY
    LT_RUN(test_serial_pty);
#endif
    LT_RUN(test_serial_list);
    lt_plat_remove(tmp_root);
    LT_TEST_MAIN_END
}
