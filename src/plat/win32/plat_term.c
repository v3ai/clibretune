/* plat_term.c - Windows console support for the tty GUI backend.
 *
 * PLATFORM FILE: uses Win32 headers via win_priv.h (see the exception noted
 * there). Built only for TARGET=windows.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "win_priv.h"

#include <stdlib.h>
#include <string.h>

/* console-mode flags for VT sequences (ENABLE_VIRTUAL_TERMINAL_* in newer
 * SDKs; spelled here so old headers work and no E+UPPERCASE macro is defined) */
#define LT_WIN_VT_OUTPUT 0x0004UL
#define LT_WIN_VT_INPUT 0x0200UL

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
    m |= LT_WIN_VT_INPUT;
    SetConsoleMode(hi, m);
    SetConsoleMode(ho, g_out_mode | ENABLE_PROCESSED_OUTPUT | LT_WIN_VT_OUTPUT);
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
