/* lua_hl.c - syntax classes for the script editors (no parsing, only lexical): comments, strings,
 * numbers, keywords and the names of the built-in library and of the rusEFI scripting API.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lua.h"

static const char *const hl_keywords[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "goto"
};

static const char *const hl_builtins[] = {
    "print", "type", "tostring", "tonumber", "pairs", "ipairs", "next", "select", "pcall", "xpcall",
    "error", "assert", "unpack", "rawget", "rawset", "rawequal", "rawlen", "setmetatable",
    "getmetatable", "string", "math", "table", "channel", "constant",
    "getSensor", "getSensorByIndex", "getTimeSinceBoot", "setTickRate", "setDebug", "table3d",
    "findTableIndex", "findCurveIndex", "curve", "interpolate", "setTable", "mcu_standby",
    "txCan", "startPwm", "setPwmDuty", "setPwmFreq", "getDigital", "setAuxDigital",
    "getAuxAnalog", "setLuaGauge", "stopEngine", "getCalibration", "setCalibration",
    "getOutput", "getCanRxById", "enableCanTx", "setClutchUpState", "setBrakePedalState",
    "onTick", "onCanRx", "onPinChange"
};

static int in_list(const char *const *list, size_t n, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strlen(list[i]) == len && memcmp(list[i], s, len) == 0) { return 1; }
    }
    return 0;
}

static int is_id_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit_ch(int c) { return c >= '0' && c <= '9'; }

/* [=*[ at i: level or -1 */
static int long_open(const char *s, size_t len, size_t i)
{
    size_t k = i + 1;
    int lvl = 0;
    if (i >= len || s[i] != '[') { return -1; }
    while (k < len && s[k] == '=') { lvl++; k++; }
    return (k < len && s[k] == '[') ? lvl : -1;
}

/* index just after the closing bracket of level lvl, or len */
static size_t long_close(const char *s, size_t len, size_t i, int lvl)
{
    while (i < len) {
        if (s[i] == ']') {
            size_t k = i + 1;
            int l = 0;
            while (k < len && s[k] == '=') { l++; k++; }
            if (l == lvl && k < len && s[k] == ']') { return k + 1; }
        }
        i++;
    }
    return len;
}

void lt_lua_highlight(const char *text, size_t len, unsigned char *cls)
{
    size_t i = 0, j;
    memset(cls, LT_LUA_HL_TEXT, len);
    while (i < len) {
        int c = (unsigned char)text[i];
        int lvl = -1;
        if (c == '-' && i + 1 < len && text[i + 1] == '-') {
            lvl = long_open(text, len, i + 2);
            if (lvl >= 0) {
                j = long_close(text, len, i + 4 + (size_t)lvl, lvl);
            } else {
                j = i + 2;
                while (j < len && text[j] != '\n') { j++; }
            }
            memset(cls + i, LT_LUA_HL_COMMENT, j - i);
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            j = i + 1;
            while (j < len && (unsigned char)text[j] != c && text[j] != '\n') {
                if (text[j] == '\\' && j + 1 < len) { j++; }
                j++;
            }
            if (j < len && (unsigned char)text[j] == c) { j++; }
            memset(cls + i, LT_LUA_HL_STRING, j - i);
            i = j;
            continue;
        }
        if (c == '[') { lvl = long_open(text, len, i); }
        if (lvl >= 0) {
            j = long_close(text, len, i + 2 + (size_t)lvl, lvl);
            memset(cls + i, LT_LUA_HL_STRING, j - i);
            i = j;
            continue;
        }
        if (is_digit_ch(c) || (c == '.' && i + 1 < len && is_digit_ch((unsigned char)text[i + 1]))) {
            j = i + 1;
            while (j < len && (is_id_start((unsigned char)text[j]) || is_digit_ch((unsigned char)text[j]) ||
                               text[j] == '.')) {
                j++;
            }
            memset(cls + i, LT_LUA_HL_NUMBER, j - i);
            i = j;
            continue;
        }
        if (is_id_start(c)) {
            j = i + 1;
            while (j < len && (is_id_start((unsigned char)text[j]) || is_digit_ch((unsigned char)text[j]))) { j++; }
            if (in_list(hl_keywords, sizeof(hl_keywords) / sizeof(hl_keywords[0]), text + i, j - i)) {
                memset(cls + i, LT_LUA_HL_KEYWORD, j - i);
            } else if (in_list(hl_builtins, sizeof(hl_builtins) / sizeof(hl_builtins[0]), text + i, j - i)) {
                memset(cls + i, LT_LUA_HL_BUILTIN, j - i);
            }
            i = j;
            continue;
        }
        i++;
    }
}
