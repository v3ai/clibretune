/* LT_TEST_STANDALONE: includes be_tty.c directly and stubs its deps */
/* test_tty.c - unit tests for the tty backend (no real terminal needed).
 * Includes be_tty.c directly to reach its static helpers and stubs the
 * lt_ base and lt_term_* functions it links against.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gui/be_tty.c"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs ---- */
void *lt_malloc(size_t n) { return malloc(n); }
void *lt_calloc(size_t c, size_t s) { return calloc(c, s); }
void *lt_realloc(void *p, size_t n) { return realloc(p, n); }
void lt_free(void *p) { free(p); }
void lt_err_set(const char *fmt, ...) { LT_UNUSED(fmt); }

static const char *s_in = "";
static int s_in_pos = 0;
static char s_wr[8192];
static int s_wr_len = 0, s_wr_calls = 0, s_resized = 0, s_raw = 0;

lt_bool lt_term_isatty(void) { return LT_TRUE; }
int lt_term_raw_begin(void) { s_raw = 1; return 0; }
void lt_term_raw_end(void) { s_raw = 0; }
void lt_term_size(int *c, int *r) { *c = 100; *r = 30; }
int lt_term_read(char *buf, int n, int timeout_ms)
{
    int avail = (int)strlen(s_in) - s_in_pos;
    LT_UNUSED(timeout_ms);
    if (avail <= 0) {
        return 0;
    }
    if (avail > n) {
        avail = n;
    }
    memcpy(buf, s_in + s_in_pos, (size_t)avail);
    s_in_pos += avail;
    return avail;
}
void lt_term_write(const char *buf, int n)
{
    if (s_wr_len + n < (int)sizeof(s_wr)) {
        memcpy(s_wr + s_wr_len, buf, (size_t)n);
        s_wr_len += n;
        s_wr[s_wr_len] = '\0';
    }
    s_wr_calls++;
}
lt_bool lt_term_resized(void) { int r = s_resized; s_resized = 0; return r; }

/* ---- helpers ---- */
static lt_backend *mk(void)
{
    return lt_backend_tty_create();
}

static void feed(const char *bytes)
{
    s_in = bytes;
    s_in_pos = 0;
}

/* collect all events until timeout */
static int collect(lt_backend *b, lt_event *evs, int max)
{
    int n = 0;
    while (n < max && b->poll(b, &evs[n], 10)) {
        n++;
    }
    return n;
}

static void test_input(void)
{
    lt_backend *b = mk();
    lt_event e[16];
    int n;
    CHECK(b != NULL);
    CHECK(b->open(b, "t", 800, 480) == 0);

    feed("a");
    n = collect(b, e, 16);
    CHECK(n == 2 && e[0].type == LT_EV_KEY_DOWN && e[0].key == 'a');
    CHECK(e[1].type == LT_EV_TEXT && strcmp(e[1].text, "a") == 0);

    feed("A");
    n = collect(b, e, 16);
    CHECK(n == 2 && e[0].key == 'a' && (e[0].mods & LT_MOD_SHIFT));
    CHECK(strcmp(e[1].text, "A") == 0);

    feed("\001");
    n = collect(b, e, 16);
    CHECK(n == 1 && e[0].key == 'a' && (e[0].mods & LT_MOD_CTRL));

    feed("\t\r\n\177\010");
    n = collect(b, e, 16);
    CHECK(n == 5 && e[0].key == LT_KEY_TAB && e[1].key == LT_KEY_ENTER
          && e[2].key == LT_KEY_ENTER && e[3].key == LT_KEY_BACKSPACE
          && e[4].key == LT_KEY_BACKSPACE);

    feed("\033");
    n = collect(b, e, 16);
    CHECK(n == 1 && e[0].key == LT_KEY_ESCAPE);

    feed("\033[A\033[B\033[C\033[D");
    n = collect(b, e, 16);
    CHECK(n == 4 && e[0].key == LT_KEY_UP && e[1].key == LT_KEY_DOWN
          && e[2].key == LT_KEY_RIGHT && e[3].key == LT_KEY_LEFT);

    feed("\033[H\033[F\033[1~\033[4~\033OH\033OF");
    n = collect(b, e, 16);
    CHECK(n == 6 && e[0].key == LT_KEY_HOME && e[1].key == LT_KEY_END
          && e[2].key == LT_KEY_HOME && e[3].key == LT_KEY_END
          && e[4].key == LT_KEY_HOME && e[5].key == LT_KEY_END);

    feed("\033[5~\033[6~\033[2~\033[3~");
    n = collect(b, e, 16);
    CHECK(n == 4 && e[0].key == LT_KEY_PAGEUP && e[1].key == LT_KEY_PAGEDOWN
          && e[2].key == LT_KEY_INSERT && e[3].key == LT_KEY_DELETE);

    feed("\033OP\033OS\033[15~\033[24~");
    n = collect(b, e, 16);
    CHECK(n == 4 && e[0].key == LT_KEY_F1 && e[1].key == LT_KEY_F4
          && e[2].key == LT_KEY_F5 && e[3].key == LT_KEY_F12);

    feed("\033[1;5A\033[1;2C");
    n = collect(b, e, 16);
    CHECK(n == 2 && e[0].key == LT_KEY_UP && e[0].mods == LT_MOD_CTRL
          && e[1].key == LT_KEY_RIGHT && e[1].mods == LT_MOD_SHIFT);

    feed("\033x");
    n = collect(b, e, 16);
    CHECK(n == 1 && e[0].key == 'x' && (e[0].mods & LT_MOD_ALT));

    feed("\303\251");
    n = collect(b, e, 16);
    CHECK(n == 2 && e[0].key == 0xE9 && strcmp(e[1].text, "\303\251") == 0);

    feed("\033[<0;10;5M\033[<0;10;5m\033[<2;1;1M\033[<64;3;3M\033[<65;3;3M\033[<32;4;4M");
    n = collect(b, e, 16);
    CHECK(n == 6);
    CHECK(e[0].type == LT_EV_MOUSE_DOWN && e[0].button == LT_MB_LEFT);
    CHECK(e[0].x == 9 * 8 + 4 && e[0].y == 4 * 16 + 8);
    CHECK(e[1].type == LT_EV_MOUSE_UP && e[1].button == LT_MB_LEFT);
    CHECK(e[2].type == LT_EV_MOUSE_DOWN && e[2].button == LT_MB_RIGHT);
    CHECK(e[3].type == LT_EV_WHEEL && e[3].wheel == 1);
    CHECK(e[4].type == LT_EV_WHEEL && e[4].wheel == -1);
    CHECK(e[5].type == LT_EV_MOUSE_MOVE);

    s_resized = 1;
    feed("");
    n = collect(b, e, 16);
    CHECK(n == 1 && e[0].type == LT_EV_RESIZE && e[0].x == 100 * 8 && e[0].y == 30 * 16);
    {
        int w = 0, h = 0;
        b->size(b, &w, &h);
        CHECK(w == 100 && h == 30);
    }

    b->close(b);
    lt_free(b->impl);
    lt_free(b);
}

static void test_quant(void)
{
    CHECK(tty_rgb_to_256(0, 0, 0) == 16);
    CHECK(tty_rgb_to_256(255, 255, 255) == 231);
    CHECK(tty_rgb_to_256(255, 0, 0) == 196);
    CHECK(tty_rgb_to_256(128, 128, 128) == 244);
    CHECK(tty_rgb_to_16(0, 0, 0) == 0);
    CHECK(tty_rgb_to_16(255, 255, 255) == 15);
    CHECK(tty_rgb_to_16(255, 90, 90) == 9);
    CHECK(tty_rgb_to_16(0, 0, 200) == 4);
    CHECK(tty_detect_depth("truecolor", "xterm") == TTY_D_TRUE);
    CHECK(tty_detect_depth("24bit", NULL) == TTY_D_TRUE);
    CHECK(tty_detect_depth(NULL, "xterm-256color") == TTY_D_256);
    CHECK(tty_detect_depth(NULL, "xterm") == TTY_D_16);
    CHECK(tty_detect_depth(NULL, "dumb") == TTY_D_MONO);
    CHECK(tty_detect_depth(NULL, "vt100") == TTY_D_MONO);
    CHECK(tty_detect_utf8("en_US.UTF-8", NULL, NULL) == 1);
    CHECK(tty_detect_utf8(NULL, NULL, "C") == 0);
    CHECK(tty_detect_utf8("", "", "de_DE.utf8") == 1);
    CHECK(tty_detect_utf8("C", NULL, "en_US.UTF-8") == 0);
}

static void fill(lt_cell *c, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        c[i].ch = (lt_u32)('a' + (i % 26));
        c[i].fg = LT_RGB(200, 200, 200);
        c[i].bg = LT_RGB(0, 0, 0);
        c[i].attr = 0;
    }
}

/* The output buffer is not NUL terminated: copy it for the string searches. */
static const char *outs(const tty *t)
{
    static char copy[8192];
    size_t n = t->out.len < sizeof copy - 1 ? t->out.len : sizeof copy - 1;

    if (n) memcpy(copy, t->out.p, n);
    copy[n] = '\0';
    return copy;
}

static void test_diff(void)
{
    tty t;
    lt_cells g;
    size_t first;
    lt_cell cells[8 * 4];
    memset(&t, 0, sizeof(t));
    t.cols = 8;
    t.rows = 4;
    t.depth = TTY_D_TRUE;
    t.utf8 = 1;
    g.cells = cells;
    g.cols = 8;
    g.rows = 4;
    fill(cells, 32);

    tty_render(&t, &g);
    first = t.out.len;
    CHECK(first > 32);
    CHECK(strstr(outs(&t), "\033[38;2;200;200;200;48;2;0;0;0m") == NULL
          || strstr(outs(&t), ";38;2;200;200;200") != NULL);

    tty_render(&t, &g);
    CHECK(t.out.len == 0);

    cells[9].ch = 'Z';
    tty_render(&t, &g);
    CHECK(t.out.len > 0 && t.out.len < 40);
    CHECK(memchr(t.out.p, 'Z', t.out.len) != NULL);
    CHECK(strstr(outs(&t), "\033[2;2H") != NULL);

    /* color depth variants */
    t.depth = TTY_D_256;
    t.have_prev = 0;
    tty_render(&t, &g);
    CHECK(strstr(outs(&t), ";38;5;") != NULL);
    t.depth = TTY_D_16;
    t.have_prev = 0;
    tty_render(&t, &g);
    CHECK(strstr(outs(&t), ";37") != NULL || strstr(outs(&t), ";90") != NULL);
    t.depth = TTY_D_MONO;
    t.have_prev = 0;
    cells[0].bg = LT_RGB(255, 255, 255);
    tty_render(&t, &g);
    CHECK(strstr(outs(&t), ";7m") != NULL);

    /* ASCII fallback */
    t.utf8 = 0;
    t.depth = TTY_D_16;
    t.have_prev = 0;
    cells[0].ch = 0x2500;
    cells[1].ch = 0x2502;
    cells[2].ch = 0x250C;
    cells[3].ch = 0x2588;
    tty_render(&t, &g);
    CHECK(strstr(outs(&t), "-") != NULL && strstr(outs(&t), "|") != NULL);
    CHECK(strstr(outs(&t), "+") != NULL && strstr(outs(&t), "#") != NULL);
    CHECK(memchr(t.out.p, 0xE2, t.out.len) == NULL);

    if (t.prev) {
        free(t.prev);
    }
    free(t.out.p);
}

static void test_open_close(void)
{
    lt_backend *b = mk();
    lt_cells g;
    lt_cell cells[4];
    int calls;
    lt_rect dirty0;
    s_wr_len = 0;
    s_wr_calls = 0;
    dirty0.x = dirty0.y = dirty0.w = dirty0.h = 0;
    CHECK(b->open(b, "x", 0, 0) == 0);
    CHECK(s_raw == 1);
    CHECK(strstr(s_wr, "\033[?1049h") != NULL && strstr(s_wr, "\033[?1006h") != NULL);
    g.cells = cells;
    g.cols = 2;
    g.rows = 2;
    fill(cells, 4);
    calls = s_wr_calls;
    b->present(b, NULL, &g, dirty0);
    CHECK(s_wr_calls == calls + 1);
    b->close(b);
    CHECK(s_raw == 0);
    CHECK(strstr(s_wr, "\033[?1049l") != NULL);
    lt_free(b->impl);
    lt_free(b);
}

int main(void)
{
    test_input();
    test_quant();
    test_diff();
    test_open_close();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
