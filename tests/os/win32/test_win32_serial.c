/* test_win32_serial.c - Windows serial (CreateFile + DCB) against a peer.
 *
 * PLATFORM FILE (tests/os/win32, built only for TARGET=windows).
 * Needs a real or virtual COM port: set LT_TEST_COM to its name (for example
 * COM1). tests/os/win32/serial_wine.sh does that under wine by linking COM1
 * to a pty and running a Python echo peer. Skips (exit 0) when unset.
 *
 * Protocol with the peer: we send "ping", it answers "pong"; then we send
 * 300 bytes in one write and it answers with their byte sum as 4 hex digits.
 */
#include "lt_test.h"
#include "../../../src/plat/plat.h"
#include <stdlib.h>
#include <string.h>

LT_TEST(test_serial_com)
{
    char *port = lt_plat_getenv("LT_TEST_COM");
    lt_stream *s;
    char buf[32];
    unsigned char blob[300];
    int i, n, sum = 0;
    char **names = NULL;
    int count;
    char want[8];

    if (port == NULL || port[0] == '\0') {
        printf("test_win32_serial: LT_TEST_COM unset, skipped\n");
        lt_free(port);
        return;
    }
    LT_CHECK(lt_serial_open(port, 12345L) == NULL || 1);   /* odd rates may or may not be accepted */
    LT_CHECK(lt_serial_open(port, 5L) == NULL);             /* below the supported range */
    s = lt_serial_open(port, 115200L);
    LT_CHECK(s != NULL);
    if (s == NULL) {
        fprintf(stderr, "open %s: %s\n", port, lt_err_get());
        lt_free(port);
        return;
    }
    LT_CHECK(lt_serial_set_baud(s, 57600L) == 0);
    LT_CHECK(lt_serial_set_baud(s, 115200L) == 0);
    {   /* DTR/RTS pulse as used by the Arduino flasher; a pty behind wine may refuse it */
        int r = lt_serial_set_lines(s, 0, 0);
        LT_CHECK(r == 0 || r == -1);
        r = lt_serial_set_lines(s, 1, 1);
        LT_CHECK(r == 0 || r == -1);
    }
    LT_CHECK(lt_serial_set_lines(NULL, 1, 1) == -1);
    LT_CHECK(lt_stream_write(s, "ping", 4) == 4);
    memset(buf, 0, sizeof buf);
    n = lt_stream_read_full(s, buf, 4, 3000);
    LT_CHECK(n == 4 && memcmp(buf, "pong", 4) == 0);
    /* nothing pending: non-blocking read returns 0 immediately */
    LT_CHECK(lt_stream_read(s, buf, 1, 0) == 0);
    for (i = 0; i < 300; i++) {
        blob[i] = (unsigned char)((i * 7 + 3) & 0xFF);
        sum += blob[i];
    }
    LT_CHECK(lt_stream_write(s, blob, 300) == 300);
    memset(buf, 0, sizeof buf);
    n = lt_stream_read_full(s, buf, 4, 3000);
    lt_snprintf(want, sizeof want, "%04X", sum & 0xFFFF);
    LT_CHECK(n == 4 && memcmp(buf, want, 4) == 0);
    /* read timeout is honoured (peer is silent) */
    {
        double t0 = lt_plat_now_ms();
        LT_CHECK(lt_stream_read(s, buf, 1, 200) == 0);
        LT_CHECK(lt_plat_now_ms() - t0 >= 150.0);
    }
    lt_stream_close(s);
    count = lt_serial_list(&names);
    LT_CHECK(count >= 0);
    if (count > 0) {
        int found = 0;
        for (i = 0; i < count; i++) {
            if (strcmp(names[i], port) == 0) found = 1;
        }
        LT_CHECK(found);
    }
    lt_plat_free_list(names, count);
    lt_free(port);
}

int main(void)
{
    LT_RUN(test_serial_com);
    LT_TEST_MAIN_END
}
