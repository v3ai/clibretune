/* test_cocoa_map.c - the OS-independent half of the macOS backend
 * (key codes, modifiers, typed text, scroll accumulation). Runs everywhere. */
#include "lt_test.h"
#include "../src/lt.h"
#include "../src/gui/be_cocoa_map.h"

LT_TEST(cocoa_mods)
{
    LT_CHECK(lt_cocoa_mods(0UL) == 0);
    LT_CHECK(lt_cocoa_mods(LT_COCOA_FLAG_SHIFT) == LT_MOD_SHIFT);
    LT_CHECK(lt_cocoa_mods(LT_COCOA_FLAG_CONTROL) == LT_MOD_CTRL);
    LT_CHECK(lt_cocoa_mods(LT_COCOA_FLAG_OPTION) == LT_MOD_ALT);
    LT_CHECK(lt_cocoa_mods(LT_COCOA_FLAG_COMMAND) == LT_MOD_SUPER);
    LT_CHECK(lt_cocoa_mods(LT_COCOA_FLAG_SHIFT | LT_COCOA_FLAG_COMMAND | 0x10UL) ==
             (LT_MOD_SHIFT | LT_MOD_SUPER));
}

LT_TEST(cocoa_keycodes)
{
    LT_CHECK(lt_cocoa_map_keycode(0x24U) == LT_KEY_ENTER);
    LT_CHECK(lt_cocoa_map_keycode(0x4CU) == LT_KEY_ENTER);
    LT_CHECK(lt_cocoa_map_keycode(0x30U) == LT_KEY_TAB);
    LT_CHECK(lt_cocoa_map_keycode(0x33U) == LT_KEY_BACKSPACE);
    LT_CHECK(lt_cocoa_map_keycode(0x75U) == LT_KEY_DELETE);
    LT_CHECK(lt_cocoa_map_keycode(0x35U) == LT_KEY_ESCAPE);
    LT_CHECK(lt_cocoa_map_keycode(0x7BU) == LT_KEY_LEFT);
    LT_CHECK(lt_cocoa_map_keycode(0x7CU) == LT_KEY_RIGHT);
    LT_CHECK(lt_cocoa_map_keycode(0x7DU) == LT_KEY_DOWN);
    LT_CHECK(lt_cocoa_map_keycode(0x7EU) == LT_KEY_UP);
    LT_CHECK(lt_cocoa_map_keycode(0x73U) == LT_KEY_HOME);
    LT_CHECK(lt_cocoa_map_keycode(0x77U) == LT_KEY_END);
    LT_CHECK(lt_cocoa_map_keycode(0x74U) == LT_KEY_PAGEUP);
    LT_CHECK(lt_cocoa_map_keycode(0x79U) == LT_KEY_PAGEDOWN);
    LT_CHECK(lt_cocoa_map_keycode(0x7AU) == LT_KEY_F1);
    LT_CHECK(lt_cocoa_map_keycode(0x6FU) == LT_KEY_F12);
    LT_CHECK(lt_cocoa_map_keycode(0x00U) == LT_KEY_NONE);      /* 'a' is a character key */
    LT_CHECK(lt_cocoa_map_keycode(0x31U) == LT_KEY_NONE);      /* space comes as a character */
    {
        /* all twelve function keys are distinct and consecutive */
        static const unsigned int f[12] = { 0x7AU, 0x78U, 0x63U, 0x76U, 0x60U, 0x61U,
                                            0x62U, 0x64U, 0x65U, 0x6DU, 0x67U, 0x6FU };
        int i;
        for (i = 0; i < 12; i++) {
            LT_CHECK(lt_cocoa_map_keycode(f[i]) == LT_KEY_F1 + i);
        }
    }
}

LT_TEST(cocoa_chars)
{
    LT_CHECK(lt_cocoa_map_char((lt_u32)'a') == 'a');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'Q') == 'q');
    LT_CHECK(lt_cocoa_map_char((lt_u32)' ') == ' ');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'5') == '5');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'!') == '1');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'_') == '-');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'+') == '=');
    LT_CHECK(lt_cocoa_map_char((lt_u32)':') == ';');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'|') == '\\');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'"') == '\'');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'~') == '`');
    LT_CHECK(lt_cocoa_map_char((lt_u32)'/') == '/');
    LT_CHECK(lt_cocoa_map_char(0x7FUL) == LT_KEY_NONE);
    LT_CHECK(lt_cocoa_map_char(0x0DUL) == LT_KEY_NONE);
    LT_CHECK(lt_cocoa_map_char(0xE9UL) == LT_KEY_NONE);
    LT_CHECK(lt_cocoa_map_char(0xF700UL) == LT_KEY_NONE);
}

LT_TEST(cocoa_text)
{
    char buf[8];
    LT_CHECK(lt_cocoa_text("a", buf, sizeof buf) == 1 && strcmp(buf, "a") == 0);
    LT_CHECK(lt_cocoa_text("\xC3\xA9", buf, sizeof buf) == 2 && strcmp(buf, "\xC3\xA9") == 0);
    LT_CHECK(lt_cocoa_text("\r", buf, sizeof buf) == 0 && buf[0] == '\0');
    LT_CHECK(lt_cocoa_text("\x7F", buf, sizeof buf) == 0);
    LT_CHECK(lt_cocoa_text("\x1B", buf, sizeof buf) == 0);
    /* U+F700 (arrow up) is AppKit's private-use function-key code point */
    LT_CHECK(lt_cocoa_text("\xEF\x9C\x80", buf, sizeof buf) == 0);
    LT_CHECK(lt_cocoa_text("\xEF\x9C\x80x", buf, sizeof buf) == 1 && strcmp(buf, "x") == 0);
    LT_CHECK(lt_cocoa_text(NULL, buf, sizeof buf) == 0);
    /* never splits a code point and always terminates */
    LT_CHECK(lt_cocoa_text("abcdef\xC3\xA9", buf, 8) == 6);
    LT_CHECK(strcmp(buf, "abcdef") == 0);
    LT_CHECK(lt_cocoa_text("ab\xC3\xA9", buf, 3) == 2);
    LT_CHECK(lt_cocoa_text("abc", buf, 1) == 0 && buf[0] == '\0');
    LT_CHECK(lt_cocoa_text("abc", NULL, 8) == 0);
}

LT_TEST(cocoa_scroll)
{
    lt_cocoa_scroll s;
    int h = 99;
    s.acc_x = 0.0;
    s.acc_y = 0.0;
    /* wheel: whole lines */
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 1.0, LT_FALSE, &h) == 1 && h == 0);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, -3.0, LT_FALSE, &h) == -3);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 0.4, LT_FALSE, &h) == 0);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 0.7, LT_FALSE, &h) == 1);   /* 1.1 total */
    /* trackpad: points accumulate across events */
    s.acc_y = 0.0;
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 5.0, LT_TRUE, &h) == 0);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 5.0, LT_TRUE, &h) == 0);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 5.0, LT_TRUE, &h) == 1);
    /* reversing direction drops the remainder */
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 5.0, LT_TRUE, &h) == 0);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, -5.0, LT_TRUE, &h) == 0);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, -5.0, LT_TRUE, &h) == 0);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, -5.0, LT_TRUE, &h) == -1);
    /* huge flings are limited */
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 1000.0, LT_FALSE, &h) == 8);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, -1000.0, LT_FALSE, &h) == -8);
    /* horizontal: positive delta = content moves right = user looks left */
    s.acc_x = 0.0;
    s.acc_y = 0.0;
    LT_CHECK(lt_cocoa_scroll_steps(&s, 2.0, 0.0, LT_FALSE, &h) == 0 && h == -2);
    LT_CHECK(lt_cocoa_scroll_steps(&s, -1.0, 0.0, LT_FALSE, &h) == 0 && h == 1);
    LT_CHECK(lt_cocoa_scroll_steps(&s, 0.0, 0.0, LT_FALSE, NULL) == 0);
}

int main(void)
{
    LT_RUN(cocoa_mods);
    LT_RUN(cocoa_keycodes);
    LT_RUN(cocoa_chars);
    LT_RUN(cocoa_text);
    LT_RUN(cocoa_scroll);
    LT_TEST_MAIN_END
}
