/* test_plat_pty.c - serial port test against a POSIX pseudo-terminal.
 *
 * PLATFORM FILE (tests/os/posix, built only for POSIX targets): includes
 * unistd.h/fcntl.h/termios.h; feature-test macros come from the build
 * (tools/target.sh). Skips silently when no pty can be opened.
 */
#include "lt_test.h"
#include "../../../src/plat/plat.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

LT_TEST(test_serial_pty)
{
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
        {   /* modem lines: a pty may not have them (-1), but the call must be safe */
            int r = lt_serial_set_lines(s, 0, 0);
            LT_CHECK(r == 0 || r == -1);
            r = lt_serial_set_lines(s, 1, 1);
            LT_CHECK(r == 0 || r == -1);
        }
        LT_CHECK(lt_serial_set_lines(NULL, 1, 1) == -1);
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
}


int main(void)
{
    LT_RUN(test_serial_pty);
    LT_TEST_MAIN_END
}
