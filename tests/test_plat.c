/* test_plat.c - tests for src/plat (portable parts; the POSIX pty serial
 * test lives in tests/os/posix/test_plat_pty.c). */
#include "lt_test.h"
#include "../src/plat/plat.h"
#include "../src/plat/plat_priv.h"
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

static void fold_is(const char *cwd, const char *path, lt_bool win, const char *want)
{
    char *r = lt_plat_path_fold(cwd, path, win);
    LT_CHECK_STR(r, want);
    lt_free(r);
}

LT_TEST(test_path_fold)
{
    /* POSIX rules */
    fold_is("/home/u", "proj", LT_FALSE, "/home/u/proj");
    fold_is("/home/u", "./proj/", LT_FALSE, "/home/u/proj");
    fold_is("/home/u", "../v//proj/./x/..", LT_FALSE, "/home/v/proj");
    fold_is("/home/u", "/abs/path/", LT_FALSE, "/abs/path");
    fold_is("/home/u", "//abs///p", LT_FALSE, "/abs/p");
    fold_is("/", "../../..", LT_FALSE, "/");
    fold_is("/home/u", "/", LT_FALSE, "/");
    fold_is("/", ".", LT_FALSE, "/");
    fold_is("/a", "b\\c", LT_FALSE, "/a/b\\c");            /* a backslash is a name character */
    fold_is("", "a/../..", LT_FALSE, "..");
    fold_is("", "a/..", LT_FALSE, ".");
    /* Windows rules */
    fold_is("C:\\Users\\u", "proj", LT_TRUE, "C:\\Users\\u\\proj");
    fold_is("C:\\Users\\u", "..\\v/proj/", LT_TRUE, "C:\\Users\\v\\proj");
    fold_is("C:\\Users\\u", "D:/Tunes/./a/../b\\", LT_TRUE, "D:\\Tunes\\b");
    fold_is("C:\\Users\\u", "\\Data", LT_TRUE, "C:\\Data");
    fold_is("C:\\Users\\u", "c:sub", LT_TRUE, "C:\\Users\\u\\sub");
    fold_is("C:\\Users\\u", "E:sub", LT_TRUE, "E:\\sub");
    fold_is("C:\\Users\\u", "C:\\..\\..", LT_TRUE, "C:\\");
    fold_is("C:\\", "C:/", LT_TRUE, "C:\\");
    fold_is("C:\\x", "\\\\server\\share\\dir\\..\\p\\", LT_TRUE, "\\\\server\\share\\p");
    fold_is("C:\\x", "//server/share/", LT_TRUE, "\\\\server\\share");
    fold_is("C:\\x", "\\\\server\\share\\..", LT_TRUE, "\\\\server\\share");
}

LT_TEST(test_abspath)
{
    char *a, *b, *c, *d, *rel;
    LT_CHECK(lt_plat_abspath(NULL) == NULL);
    LT_CHECK(lt_plat_abspath("") == NULL);
    rel = lt_path_join(tmp_root, "abs/one");
    LT_CHECK(lt_plat_mkdirs(rel) == 0);
    a = lt_plat_abspath(rel);
    lt_free(rel);
    rel = lt_path_join(tmp_root, "abs/two/../one/./");      /* "two" does not exist */
    b = lt_plat_abspath(rel);
    lt_free(rel);
    rel = lt_path_join(tmp_root, "abs/missing/x");
    c = lt_plat_abspath(rel);
    lt_free(rel);
    LT_CHECK(a != NULL && b != NULL && c != NULL);
    if (a != NULL && b != NULL && c != NULL) {
        size_t n = strlen(a);
        LT_CHECK(lt_plat_path_same(a, b));
        LT_CHECK(a[0] == '/' || (a[1] == ':' && a[2] == '\\'));          /* absolute */
        LT_CHECK(a[n - 1] != '/' && a[n - 1] != '\\');                    /* no trailing separator */
        LT_CHECK(strstr(a, "..") == NULL);
        LT_CHECK(strstr(c, "missing") != NULL && strstr(c, "..") == NULL);
        LT_CHECK(lt_plat_os() == LT_OS_WINDOWS ? strchr(a, '/') == NULL : strchr(a, '\\') == NULL);
        d = lt_plat_abspath(a);                                            /* idempotent */
        LT_CHECK(d != NULL && strcmp(a, d) == 0);
        lt_free(d);
        LT_CHECK(lt_plat_is_dir(a));
    }
    LT_CHECK(!lt_plat_path_same(a, c));
    LT_CHECK(!lt_plat_path_same(NULL, a));
    LT_CHECK(lt_plat_path_same("C:\\A", "c:\\a") == (lt_plat_os() == LT_OS_WINDOWS));
    lt_free(a); lt_free(b); lt_free(c);
    rel = lt_path_join(tmp_root, "abs/one");
    (void)lt_plat_remove(rel);
    lt_free(rel);
    rel = lt_path_join(tmp_root, "abs");
    (void)lt_plat_remove(rel);
    lt_free(rel);
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
    LT_RUN(test_path_fold);
    LT_RUN(test_abspath);
    LT_RUN(test_dirs);
    LT_RUN(test_tcp_echo);
    LT_RUN(test_serial_list);
    lt_plat_remove(tmp_root);
    LT_TEST_MAIN_END
}
