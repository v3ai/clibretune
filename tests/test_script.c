/* test_script.c - tests for src/script (the Lua subset interpreter) */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "script/lua.h"
#include "lt_test.h"

static lt_lua *L;

/* run src; returns the result text (or "ERR: <message>") */
static const char *R(const char *src)
{
    static char out[600];
    int rc = lt_lua_run(L, src, strlen(src), "t");
    if (rc != 0) { lt_snprintf(out, sizeof out, "ERR: %s", lt_lua_error(L)); }
    else { lt_strlcpy(out, lt_lua_result(L), sizeof out); }
    return out;
}

#define CHK_R(src, want) LT_CHECK_STR(R(src), want)

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

LT_TEST(basics)
{
    CHK_R("return 2 + 2", "4");
    CHK_R("return 7 / 2", "3.5");
    CHK_R("return 7 // 2, -7 // 2", "3\t-4");
    CHK_R("return 7 % 3, -7 % 3, 7 % -3, 5.5 % 2", "1\t2\t-2\t1.5");
    CHK_R("return 2 ^ 10, 2 ^ 0.5 > 1.41", "1024\ttrue");
    CHK_R("return 2 ^ 3 ^ 2", "512");
    CHK_R("return -2 ^ 2", "-4");
    CHK_R("return 1 + 2 * 3 - 4 / 2", "5");
    CHK_R("return (1 + 2) * 3", "9");
    CHK_R("return 'a' .. 'b' .. 1 .. 2", "ab12");
    CHK_R("return #'hello', #{1, 2, 3}", "5\t3");
    CHK_R("return 1 < 2, 2 <= 2, 3 > 4, 'a' < 'b', 'a' == 'a', 1 ~= 2", "true\ttrue\tfalse\ttrue\ttrue\ttrue");
    CHK_R("return nil and 1, false or 'x', 1 and 2, nil or false", "nil\tx\t2\tfalse");
    CHK_R("return not nil, not 0, not ''", "true\tfalse\tfalse");
    CHK_R("return 0x10, 0xff, 1e3, .5, 3.", "16\t255\t1000\t0.5\t3");
    CHK_R("return '10' + 5, '3' * '4', 10 .. ''", "15\t12\t10");
    CHK_R("return 1/0, -1/0", "inf\t-inf");
    CHK_R("local x = 0/0 return x ~= x", "true");
    CHK_R("return", "");
    CHK_R("", "");
    CHK_R("print('hello') return 1", "1");
    LT_CHECK_STR(lt_lua_output(L), "hello\n");
    CHK_R("print(1, 'a', nil, true, 2.5)", "");
    LT_CHECK_STR(lt_lua_output(L), "1\ta\tnil\ttrue\t2.5\n");
    CHK_R("return 100000000000000, 1e15, 2^53, 0.1, 1/3", "100000000000000\t1e+15\t9.007199254741e+15\t0.1\t0.33333333333333");
}

LT_TEST(variables_and_control)
{
    CHK_R("local a, b = 1, 2 a, b = b, a return a, b", "2\t1");
    CHK_R("local a, b, c = 1 return a, b, c", "1\tnil\tnil");
    CHK_R("x = 5 return x", "5");
    CHK_R("local t = {} t.x, t.y = 1, 2 return t.x + t.y", "3");
    CHK_R("local i = 1 local t = {} i, t[i] = i + 1, 20 return i, t[1], t[2]", "2\t20\tnil");
    CHK_R("if 1 > 2 then return 'a' elseif 2 > 1 then return 'b' else return 'c' end", "b");
    CHK_R("if nil then return 1 end return 2", "2");
    CHK_R("local n = 0 while n < 10 do n = n + 1 end return n", "10");
    CHK_R("local n = 0 repeat local k = n n = n + 1 until k >= 4 return n", "5");
    CHK_R("local s = 0 for i = 1, 10 do s = s + i end return s", "55");
    CHK_R("local s = 0 for i = 10, 1, -3 do s = s + i end return s", "22");
    CHK_R("local s = 0 for i = 1, 0 do s = s + 1 end return s", "0");
    CHK_R("local s = 0 for i = 0, 1, 0.25 do s = s + 1 end return s", "5");
    CHK_R("local s = 0 for i = 1, 100 do if i > 5 then break end s = s + i end return s", "15");
    CHK_R("do local x = 1 end return x", "nil");
    CHK_R("local x = 1 do local x = 2 end return x", "1");
    CHK_R("local t = {10, 20, 30} local s = 0 for i, v in ipairs(t) do s = s + i * v end return s", "140");
    CHK_R("local t = {a = 1, b = 2, c = 3} local s = 0 for k, v in pairs(t) do s = s + v end return s", "6");
    CHK_R("local n = 0 for _ in pairs({1, 2, 3, x = 1, y = 2}) do n = n + 1 end return n", "5");
    CHK_R("for i = 1, 3 do local x = i * 2 if x == 4 then return x end end", "4");
    CHK_R("local function f() for i = 1, 10 do if i == 3 then return i end end end return f()", "3");
    CHK_R("return type(nil), type(true), type(1), type('s'), type({}), type(print), type(function() end)",
       "nil\tboolean\tnumber\tstring\ttable\tfunction\tfunction");
}

LT_TEST(functions_and_closures)
{
    CHK_R("local function f(a, b) return a + b end return f(1, 2)", "3");
    CHK_R("function f(a, b) return a + b end return f(1, 2)", "3");
    CHK_R("local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end return fib(15)", "610");
    CHK_R("local function f(...) return select('#', ...) end return f(), f(nil), f(1, 2, 3)", "0\t1\t3");
    CHK_R("local function f(...) local a, b = ... return a, b end return f(1, 2, 3)", "1\t2");
    CHK_R("local function f(...) return ... end return f(1, 2, 3)", "1\t2\t3");
    CHK_R("local function f(...) return {...} end return #f(1, 2, 3, 4)", "4");
    CHK_R("local function f(a, ...) return a, select('#', ...) end return f(1, 2, 3)", "1\t2");
    CHK_R("local function f() return 1, 2, 3 end local t = {f()} return #t", "3");
    CHK_R("local function f() return 1, 2, 3 end local t = {f(), f()} return #t", "4");
    CHK_R("local function f() return 1, 2, 3 end local t = {(f())} return #t", "1");
    CHK_R("local function f() return 1, 2, 3 end return f(), 10", "1\t10");
    CHK_R("local function f() return 1, 2, 3 end return (f())", "1");
    CHK_R("local function mk() local c = 0 return function() c = c + 1 return c end end "
       "local a, b = mk(), mk() a() a() return a(), b()", "3\t1");
    CHK_R("local fs = {} for i = 1, 3 do fs[i] = function() return i end end return fs[1](), fs[2](), fs[3]()",
       "1\t2\t3");
    CHK_R("local fs = {} local i = 1 while i <= 3 do local j = i fs[i] = function() j = j + 10 return j end i = i + 1 end "
       "return fs[1](), fs[1](), fs[2]()", "11\t21\t12");
    CHK_R("local x = 1 local function f() return x end x = 2 return f()", "2");
    CHK_R("local function outer() local v = 5 local function mid() return function() v = v + 1 return v end end "
       "return mid() end local f = outer() f() return f()", "7");
    CHK_R("local obj = {n = 0} function obj:inc(d) self.n = self.n + (d or 1) return self end "
       "obj:inc():inc(5) return obj.n", "6");
    CHK_R("local t = {} function t.a.b() end", "ERR: t:1: attempt to index a nil value (field 'a')");
    CHK_R("local t = {a = {}} function t.a.f(x) return x * 2 end return t.a.f(4)", "8");
    CHK_R("local function f(a, b) return b end return f(1)", "nil");
    CHK_R("return (function(...) return select('#', ...) end)(nil, nil)", "2");
    CHK_R("local s = 'x' return s:rep(3), ('ab'):upper(), #s:rep(4)", "xxx\tAB\t4");
    CHK_R("return type(f)", "nil");
    CHK_R("print(select(-1, 'a', 'b', 'c')) return select(2, 'a', 'b', 'c')", "b\tc");
}

LT_TEST(tables)
{
    CHK_R("local t = {1, 2, 3, n = 4, [10] = 5} return #t, t.n, t[10]", "3\t4\t5");
    CHK_R("local t = {} for i = 1, 100 do t[i] = i end return #t, t[50]", "100\t50");
    CHK_R("local t = {} for i = 100, 1, -1 do t[i] = i end return #t", "100");
    CHK_R("local t = {1, 2, 3} t[#t + 1] = 4 return #t", "4");
    CHK_R("local t = {1, 2, 3} t[#t] = nil return #t", "2");
    CHK_R("local t = {} t[1.5] = 'a' t['1.5'] = 'b' return t[1.5], t['1.5']", "a\tb");
    CHK_R("local t = {} t[2^53] = 1 return t[2^53]", "1");
    CHK_R("local t = {} t[true] = 1 t[false] = 2 return t[true], t[false]", "1\t2");
    CHK_R("local k = {} local t = {[k] = 1} return t[k], t[{}]", "1\tnil");
    CHK_R("local t = {x = 1} t.x = nil return next(t)", "nil");
    CHK_R("return #{n = 1}, #{}, #{nil, 2}", "0\t0\t2");
    CHK_R("local t = {} t[nil] = 1", "ERR: t:1: table index is nil");
    CHK_R("local t = {} t[0/0] = 1", "ERR: t:1: table index is NaN");
    CHK_R("local t = {} return t[nil]", "nil");
    CHK_R("local t = {1, 2, 3} for k in pairs(t) do t[k] = nil end return next(t)", "nil");
    CHK_R("local t = {a = 1, b = 2} for k in pairs(t) do t[k] = nil end return next(t)", "nil");
    CHK_R("local t = {} for i = 1, 50 do t['k' .. i] = i end local s = 0 for k, v in pairs(t) do s = s + v end return s",
       "1275");
    CHK_R("local t = {} for i = 1, 200 do t[i] = i end for i = 1, 200 do t[i] = nil end return #t, next(t)", "0\tnil");
    CHK_R("local t = table.pack(1, nil, 3) return t.n, t[3]", "3\t3");
    CHK_R("return table.unpack({1, 2, 3})", "1\t2\t3");
    CHK_R("return table.unpack({1, 2, 3}, 2)", "2\t3");
    CHK_R("return table.concat({1, 2, 3}, ', ')", "1, 2, 3");
    CHK_R("return table.concat({'a', 'b', 'c'}, '-', 2, 3), table.concat({})", "b-c\t");
    CHK_R("return table.concat({1, {}})", "ERR: t:1: invalid value (at index 2) in table for 'concat'");
    CHK_R("local t = {} table.insert(t, 'a') table.insert(t, 'b') table.insert(t, 1, 'z') return table.concat(t)", "zab");
    CHK_R("local t = {1, 2, 3} return table.remove(t), table.remove(t, 1), #t, t[1]", "3\t1\t1\t2");
    CHK_R("local t = {} return table.remove(t), #t", "nil\t0");
    CHK_R("local t = {5, 2, 8, 1, 9, 3} table.sort(t) return table.concat(t, ',')", "1,2,3,5,8,9");
    CHK_R("local t = {5, 2, 8, 1} table.sort(t, function(a, b) return a > b end) return table.concat(t, ',')", "8,5,2,1");
    CHK_R("local t = {'b', 'c', 'a'} table.sort(t) return table.concat(t)", "abc");
    LT_CHECK(has(R("local t = {3, 'a'} table.sort(t)"), "attempt to compare"));
    CHK_R("local t = {} for i = 1, 300 do t[i] = (i * 7919) % 1000 end table.sort(t) "
       "for i = 2, 300 do if t[i - 1] > t[i] then return 'bad' end end return 'ok'", "ok");
    CHK_R("local t = {1, 2} t[5] = 5 t[3] = 3 t[4] = 4 return #t", "5");
}

LT_TEST(strings)
{
    CHK_R("return ('hello'):sub(2, 4), ('hello'):sub(-3), ('hello'):sub(2), ('hello'):sub(0), ('hello'):sub(10)",
       "ell\tllo\tello\thello\t");
    CHK_R("return ('hello'):sub(-100, 2), ('hello'):sub(3, 2), ('hello'):sub(-2, -1)", "he\t\tlo");
    CHK_R("return string.len('abc'), ('abc'):upper(), ('ABC'):lower(), ('abc'):reverse()", "3\tABC\tabc\tcba");
    CHK_R("return ('ab'):rep(3), ('ab'):rep(3, ','), ('ab'):rep(0), ('ab'):rep(-1)", "ababab\tab,ab,ab\t\t");
    CHK_R("return ('A'):byte(), ('abc'):byte(1, -1)", "65\t97\t98\t99");
    CHK_R("return string.char(72, 105)", "Hi");
    CHK_R("return ('hello world'):find('wor'), ('hello'):find('xyz'), ('hello'):find('l', 4)", "7\tnil\t4\t4");
    CHK_R("return ('hello world'):find('wor')", "7\t9");
    CHK_R("return ('a.b'):find('.', 1, true), ('a.b'):find('%.')", "2\t2\t2");
    CHK_R("return ('hello'):find(''), ('hello'):find('', 10)", "1\tnil");
    CHK_R("return ('hello'):find('')", "1\t0");
    CHK_R("return ('key=val'):match('(%w+)=(%w+)')", "key\tval");
    CHK_R("return ('  trim  '):match('^%s*(.-)%s*$')", "trim");
    CHK_R("return ('abc123'):match('%d+'), ('abc'):match('%d+')", "123\tnil");
    CHK_R("return ('x'):match('()')", "1");
    CHK_R("return ('hello'):find('(l)(l)')", "3\t4\tl\tl");
    CHK_R("local r = {} for w in ('one two  three'):gmatch('%w+') do r[#r + 1] = w end return table.concat(r, '|')",
       "one|two|three");
    CHK_R("local r = {} for k, v in ('a=1, b=2'):gmatch('(%w+)=(%w+)') do r[#r + 1] = k .. v end return table.concat(r, '|')",
       "a1|b2");
    CHK_R("local n = 0 for _ in (''):gmatch('x*') do n = n + 1 end return n", "1");
    CHK_R("return ('hello world'):gsub('o', '0')", "hell0 w0rld\t2");
    CHK_R("return ('hello'):gsub('l', 'L', 1)", "heLlo\t1");
    CHK_R("return ('abc'):gsub('%w', '%0%0'), ('abc'):gsub('(a)(b)', '%2%1')", "aabbcc\tbac\t1");
    CHK_R("return ('hello world'):gsub('%w+', string.upper)", "HELLO WORLD\t2");
    CHK_R("return ('$name is $age'):gsub('%$(%w+)', {name = 'Bob', age = 42})", "Bob is 42\t2");
    CHK_R("return ('abc'):gsub('', '-')", "-a-b-c-\t4");
    CHK_R("return ('abc'):gsub('b', function() return nil end)", "abc\t1");
    CHK_R("return ('THE (quick) fox'):find('%((%w+)%)')", "5\t11\tquick");
    CHK_R("return ('x = (a(b)c)'):match('%b()')", "(a(b)c)");
    CHK_R("return ('THE END'):find('%f[%w]%w+%f[%W]')", "1\t3");
    CHK_R("return ('abab'):match('(ab)%1')", "ab");
    CHK_R("return ('a,b,,c'):gsub(',', ';')", "a;b;;c\t3");
    CHK_R("return ('x'):rep(3):gsub('x', 'y')", "yyy\t3");
    CHK_R("return ('[test]'):match('^%[(.*)%]$')", "test");
    CHK_R("return ('a-b'):match('[%w-]+'), ('a1'):match('[^%d]+')", "a-b\ta");
    CHK_R("return ('hello'):match('.-l'), ('hello'):match('l+'), ('ab'):match('a?b?c?')", "hel\tll\tab");
    CHK_R("return #'a\\nb\\65\\x41\\z   c', 'tab\\there'", "6\ttab\there");
    CHK_R("return [[long\nstring]], [==[a]]b]==]", "long\nstring\ta]]b");
    CHK_R("return 'it''s'", "ERR: t:1: '<eof>' expected near ''s''");
    CHK_R("return ('abc') < ('abd'), ('Z') < ('a'), ('') < ('a'), 'a' < 'ab'", "true\ttrue\ttrue\ttrue");
}

LT_TEST(format)
{
    CHK_R("return string.format('%d %5d|%-5d|%05d|%+d', 42, 42, 42, 42, 42)", "42    42|42   |00042|+42");
    CHK_R("return string.format('%d', -7), string.format('%.3d', 5), string.format('%d', 3.0)", "-7\t005\t3");
    CHK_R("return string.format('%x %X %o %#x', 255, 255, 8, 255)", "ff FF 10 0xff");
    CHK_R("return string.format('%f %.2f %8.3f|%-8.1f|', 3.14159, 3.14159, 3.14159, 2.5)",
       "3.141590 3.14    3.142|2.5     |");
    CHK_R("return string.format('%e %g %g %g', 12345.678, 0.0001, 1e20, 100)", "1.234568e+04 0.0001 1e+20 100");
    CHK_R("return string.format('%s %s %s %10s|%-6s|%.2s', 'a', 1, true, 'right', 'left', 'trunc')",
       "a 1 true      right|left  |tr");
    CHK_R("return string.format('%c%c', 72, 105), string.format('%%'), string.format('%q', 'a\"b\\n')",
       "Hi\t%\t\"a\\\"b\\n\"");
    CHK_R("return string.format('%s', nil), string.format('%s', {}) ~= nil", "nil\ttrue");
    CHK_R("return string.format('%5.1f%%', 99.44)", " 99.4%");
    CHK_R("return string.format('%d', 1.5)", "ERR: t:1: bad argument #2 to 'format' (number has no integer representation)");
    CHK_R("return string.format('%d')", "ERR: t:1: bad argument #2 to 'format' (number expected, got no value)");
    CHK_R("return string.format('%y', 1)", "ERR: t:1: invalid conversion '%y' to 'format'");
    CHK_R("return string.format('%d', 'x')", "ERR: t:1: bad argument #2 to 'format' (number expected, got string)");
    CHK_R("return string.format('%f', 1/0), string.format('%5.1f', -1/0)", "inf\t -inf");
    CHK_R("return string.format('%100d', 1)", "ERR: t:1: invalid conversion (width too long)");
    CHK_R("return ('%d items'):format(3)", "3 items");
    CHK_R("return tostring(12), tostring(1.5), tostring(nil), tostring(true), tostring('x')", "12\t1.5\tnil\ttrue\tx");
    CHK_R("return tonumber('12'), tonumber('0x1F'), tonumber(' 5 '), tonumber('5x'), tonumber(''), tonumber('1e2')",
       "12\t31\t5\tnil\tnil\t100");
    CHK_R("return tonumber('ff', 16), tonumber('zz', 36), tonumber('12', 2), tonumber('-101', 2)", "255\t1295\tnil\t-5");
    CHK_R("return tonumber('inf'), tonumber('nan'), tonumber('0x'), tonumber('1 2'), tonumber(nil)", "nil\tnil\tnil\tnil\tnil");
}

LT_TEST(math_lib)
{
    CHK_R("return math.floor(3.7), math.ceil(3.2), math.floor(-3.5), math.abs(-4), math.sqrt(16)", "3\t4\t-4\t4\t4");
    CHK_R("return math.max(1, 5, 3), math.min(4, 2, 8), math.huge > 1e308, -math.huge < 0", "5\t2\ttrue\ttrue");
    CHK_R("return math.pi > 3.14 and math.pi < 3.15, math.fmod(7, 3), math.log(1), math.exp(0)", "true\t1\t0\t1");
    CHK_R("return math.log(8, 2) > 2.99, math.log(100, 10), math.sin(0), math.cos(0), math.atan(1, 1) > 0.78", "true\t2\t0\t1\ttrue");
    CHK_R("return math.modf(3.5)", "3\t0.5");
    CHK_R("math.randomseed(1) local ok = true for i = 1, 200 do local r = math.random() "
       "if r < 0 or r >= 1 then ok = false end local k = math.random(6) if k < 1 or k > 6 then ok = false end "
       "local j = math.random(5, 7) if j < 5 or j > 7 then ok = false end end return ok", "true");
    CHK_R("return math.random(0)", "ERR: t:1: bad argument #1 to 'random' (interval is empty)");
    CHK_R("return math.floor('x')", "ERR: t:1: bad argument #1 to 'floor' (number expected, got string)");
    CHK_R("return math.max()", "ERR: t:1: bad argument #1 to 'max' (number expected, got no value)");
    CHK_R("return math.deg(math.pi), math.rad(180) > 3.14", "180\ttrue");
}

LT_TEST(errors)
{
    CHK_R("x = ", "ERR: t:1: unexpected symbol near <eof>");
    CHK_R("x = 1 +", "ERR: t:1: unexpected symbol near <eof>");
    CHK_R("local = 1", "ERR: t:1: <name> expected near '='");
    CHK_R("if x then", "ERR: t:1: 'end' expected near <eof>");
    CHK_R("if x then\n\nreturn 1", "ERR: t:3: 'end' expected (to close 'if' at line 1) near <eof>");
    CHK_R("for i = 1 do end", "ERR: t:1: ',' expected near 'do'");
    CHK_R("x = = 2", "ERR: t:1: unexpected symbol near '='");
    CHK_R("return 1 2", "ERR: t:1: '<eof>' expected near '2'");
    CHK_R("return return", "ERR: t:1: unexpected symbol near 'return'");
    CHK_R("f(", "ERR: t:1: unexpected symbol near <eof>");
    CHK_R("x = 3x", "ERR: t:1: malformed number near '3x'");
    CHK_R("x = 'abc", "ERR: t:1: unfinished string near ''abc'");
    CHK_R("x = [[abc", "ERR: t:1: unfinished long string near '[[abc'");
    LT_CHECK(has(R("--[[ unterminated"), "unfinished long comment"));
    CHK_R("x = @", "ERR: t:1: unexpected symbol near '@'");
    LT_CHECK(has(R("break"), "break outside a loop"));
    CHK_R("x = '\\q'", "ERR: t:1: invalid escape sequence near ''\\q'");
    CHK_R("x = '\\300'", "ERR: t:1: decimal escape too large near ''\\300'");
    CHK_R("local function f() return ... end", "ERR: t:1: cannot use '...' outside a vararg function near '...'");
    CHK_R("1 = 2", "ERR: t:1: unexpected symbol near '1'");
    CHK_R("x\n\n\ny = 1 +", "ERR: t:4: syntax error near 'y'");
    CHK_R("x = 1\n\n\ny = 1 +", "ERR: t:4: unexpected symbol near <eof>");
    CHK_R("local x = nil\n\nreturn x.y", "ERR: t:3: attempt to index a nil value (local 'x')");
    CHK_R("return undefinedvar.x", "ERR: t:1: attempt to index a nil value (global 'undefinedvar')");
    CHK_R("local t = {} return t.a.b", "ERR: t:1: attempt to index a nil value (field 'a')");
    CHK_R("undefinedfn()", "ERR: t:1: attempt to call a nil value (global 'undefinedfn')");
    CHK_R("local t = {} t.x()", "ERR: t:1: attempt to call a nil value (field 'x')");
    CHK_R("local t = {} t:nomethod()", "ERR: t:1: attempt to call a nil value (method 'nomethod')");
    CHK_R("local x = 5 x()", "ERR: t:1: attempt to call a number value (local 'x')");
    CHK_R("return 1 + nil", "ERR: t:1: attempt to perform arithmetic on a nil value");
    CHK_R("local a = {} return a + 1", "ERR: t:1: attempt to perform arithmetic on a table value (local 'a')");
    CHK_R("return 'abc' + 1", "ERR: t:1: attempt to perform arithmetic on a string value");
    CHK_R("return -{}", "ERR: t:1: attempt to perform arithmetic on a table value");
    CHK_R("return 1 < 'x'", "ERR: t:1: attempt to compare number with string");
    CHK_R("return {} < {}", "ERR: t:1: attempt to compare two table values");
    CHK_R("return nil .. 'x'", "ERR: t:1: attempt to concatenate a nil value");
    CHK_R("local t = {} return t .. 'x'", "ERR: t:1: attempt to concatenate a table value (local 't')");
    CHK_R("return #5", "ERR: t:1: attempt to get length of a number value");
    CHK_R("for i = 1, 'x' do end", "ERR: t:1: 'for' limit must be a number");
    CHK_R("for i = 1, 10, 0 do end", "ERR: t:1: 'for' step is zero");
    CHK_R("for i in 5 do end", "ERR: t:1: attempt to call a number value");
    CHK_R("error('boom')", "ERR: t:1: boom");
    CHK_R("error('boom', 0)", "ERR: boom");
    CHK_R("error({code = 7})", "ERR: t:1: (error object is a table value)");
    CHK_R("error()", "ERR: t:1: (error object is a nil value)");
    CHK_R("local function f() error('deep') end\n\nf()", "ERR: t:1: deep");
    CHK_R("assert(false)", "ERR: assertion failed!");
    CHK_R("assert(nil, 'custom')", "ERR: custom");
    CHK_R("return assert(1, 'x')", "1\tx");
    CHK_R("return pcall(error, 'x', 0)", "false\tx");
    CHK_R("return pcall(function() error('x') end)", "false\tt:1: x");
    CHK_R("return pcall(function() local x = nil; return x.y end)", "false\tt:1: attempt to index a nil value (local 'x')");
    CHK_R("return select(2, pcall(error, {code = 5})).code", "5");
    CHK_R("local ok, e = pcall(function() error({1, 2}) end) return ok, type(e), #e", "false\ttable\t2");
    CHK_R("return pcall(pcall, error, 'e')", "true\tfalse\tt:1: e");
    CHK_R("return pcall(function(a, b) return a + b end, 1, 2)", "true\t3");
    CHK_R("return xpcall(function() error('x', 0) end, function(m) return 'H:' .. m end)", "false\tH:x");
    CHK_R("return xpcall(function(a) return a end, print, 9)", "true\t9");
    CHK_R("return pcall()", "ERR: t:1: bad argument #1 to 'pcall' (value expected)");
    CHK_R("return ('x'):bad()", "ERR: t:1: attempt to call a nil value (method 'bad')");
    CHK_R("return tostring()", "ERR: t:1: bad argument #1 to 'tostring' (value expected)");
    CHK_R("return ipairs()", "ERR: t:1: bad argument #1 to 'ipairs' (table expected, got no value)");
    CHK_R("return setmetatable(1, {})", "ERR: t:1: bad argument #1 to 'setmetatable' (table expected, got number)");
    CHK_R("return next({}, 'nokey')", "ERR: t:1: invalid key to 'next'");
    CHK_R("return ('x'):rep(1e10)", "ERR: t:1: not enough memory");
    /* the state is reusable after errors */
    CHK_R("return 1", "1");
    LT_CHECK_STR(lt_lua_error(L), "");
}

LT_TEST(metatables)
{
    CHK_R("local mt = {__index = function(t, k) return k .. '!' end} local t = setmetatable({}, mt) return t.foo, t[1]",
       "foo!\t1!");
    CHK_R("local base = {greet = function() return 'hi' end} local d = setmetatable({}, {__index = base}) return d.greet()", "hi");
    CHK_R("local A = {} A.__index = A function A.new(x) return setmetatable({x = x}, A) end "
       "function A:get() return self.x end local B = setmetatable({}, {__index = A}) B.__index = B "
       "local b = setmetatable({x = 9}, B) return A.new(3):get(), b:get()", "3\t9");
    CHK_R("local log = {} local t = setmetatable({}, {__newindex = function(t, k, v) rawset(t, k, v * 2) end}) t.a = 4 return t.a", "8");
    CHK_R("local t = setmetatable({}, {__call = function(self, a, b) return a + b end}) return t(1, 2)", "3");
    CHK_R("local t = setmetatable({}, {__tostring = function() return 'T!' end}) return tostring(t)", "T!");
    CHK_R("local V = {} V.__index = V V.__add = function(a, b) return setmetatable({v = a.v + b.v}, V) end "
       "V.__eq = function(a, b) return a.v == b.v end V.__lt = function(a, b) return a.v < b.v end "
       "V.__le = function(a, b) return a.v <= b.v end V.__len = function() return 42 end "
       "V.__unm = function(a) return setmetatable({v = -a.v}, V) end V.__concat = function(a, b) return 'cat' end "
       "local a, b = setmetatable({v = 1}, V), setmetatable({v = 2}, V) local c = a + b "
       "return c.v, a == b, a < b, a <= b, #a, (-a).v, a .. 'x'", "3\tfalse\ttrue\ttrue\t42\t-1\tcat");
    CHK_R("local t = setmetatable({}, {__index = function(t, k) return nil end}) return rawget(t, 'x'), t.x", "nil\tnil");
    CHK_R("local t = {} local m = {} setmetatable(t, m) return getmetatable(t) == m, getmetatable({}) == nil", "true\ttrue");
    CHK_R("local t = setmetatable({}, {}) t.__index = t return rawequal(t, t), rawlen({1, 2}), rawget({5}, 1)", "true\t2\t5");
    CHK_R("local a = {} local b = setmetatable({}, {__index = a}) setmetatable(a, {__index = b}) return b.zzz",
       "ERR: t:1: '__index' chain too long; possible loop");
    CHK_R("return getmetatable('x').len('abc')", "3");
}

LT_TEST(host_functions)
{
    /* defined below */
}

static int h_add(lt_lua_call *c, void *ud)
{
    double a = 0, b = 0;
    (void)ud;
    if (!lt_lua_argnum(c, 0, &a) || !lt_lua_argnum(c, 1, &b)) { lt_lua_fail(c, "add needs two numbers"); }
    lt_lua_pushnum(c, a + b);
    return 1;
}

static int h_info(lt_lua_call *c, void *ud)
{
    const char *s = lt_lua_argstr(c, 0);
    (void)ud;
    lt_lua_pushstr(c, s != NULL ? s : "(none)");
    lt_lua_pushbool(c, lt_lua_nargs(c) > 1);
    lt_lua_pushnil(c);
    lt_lua_pushnum(c, (double)lt_lua_argtype(c, 0));
    return 4;
}

static void out_cb(void *ud, const char *line, size_t len)
{
    lt_buf *b = (lt_buf *)ud;
    lt_buf_append(b, line, len);
    lt_buf_putc(b, '|');
}

LT_TEST(host_api)
{
    lt_buf b;
    LT_CHECK(lt_lua_register(L, "add", h_add, NULL) == 0);
    LT_CHECK(lt_lua_register(L, "info", h_info, NULL) == 0);
    LT_CHECK(lt_lua_register(L, "", h_add, NULL) < 0);
    CHK_R("return add(2, '3')", "5");
    CHK_R("return add(1)", "ERR: t:1: add needs two numbers");
    CHK_R("return pcall(add)", "false\tt:1: add needs two numbers");
    CHK_R("return info('x', 1)", "x\ttrue\tnil\t3");
    CHK_R("return info()", "(none)\tfalse\tnil\t0");
    CHK_R("return type(add), tostring(add):sub(1, 9)", "function\tfunction:");
    lt_buf_init(&b);
    lt_lua_set_print(L, out_cb, &b);
    CHK_R("print('a') print(1, 2) return 0", "0");
    LT_CHECK_STR(b.len > 0 ? b.data : "", "a|1\t2|");
    LT_CHECK_STR(lt_lua_output(L), "");
    lt_lua_set_print(L, NULL, NULL);
    lt_buf_free(&b);
    /* globals do not survive between runs, host functions do */
    CHK_R("g1 = 5", "");
    CHK_R("return g1", "nil");
    CHK_R("return add(1, 1)", "2");
}

LT_TEST(limits)
{
    unsigned long st;
    lt_lua_set_limits(L, 100000, 0);
    CHK_R("while true do end", "ERR: t:1: instruction limit exceeded");
    CHK_R("for i = 1, 1e9 do end", "ERR: t:1: instruction limit exceeded");
    CHK_R("repeat until false", "ERR: t:1: instruction limit exceeded");
    CHK_R("local function f() return f() end return f()", "ERR: t:1: stack overflow");
    CHK_R("local function f(n) return 1 + f(n + 1) end return f(1)", "ERR: t:1: stack overflow");
    CHK_R("local t = {} while true do t[#t + 1] = 1 end", "ERR: t:1: instruction limit exceeded");
    /* the limit cannot be caught */
    CHK_R("while true do pcall(function() while true do end end) end", "ERR: t:1: instruction limit exceeded");
    CHK_R("pcall(function() while true do end end) return 'escaped'", "ERR: t:1: instruction limit exceeded");
    CHK_R("local t = {} for i = 1, 1e6 do table.insert(t, 1, i) end", "ERR: t:1: instruction limit exceeded");
    CHK_R("return ('x'):rep(1e8):gsub('x', 'y')", "ERR: t:1: not enough memory");
    lt_lua_set_limits(L, 50000000ul, 0);
    lt_lua_set_limits(L, 0, 200000);
    CHK_R("local s = 'x' while true do s = s .. s end", "ERR: t:1: not enough memory");
    CHK_R("local t = {} for i = 1, 1e6 do t[i] = {i} end", "ERR: t:1: not enough memory");
    CHK_R("local t = {} for i = 1, 1e6 do t['k' .. i] = i end", "ERR: t:1: not enough memory");
    CHK_R("return ('x'):rep(1e7)", "ERR: t:1: not enough memory");
    CHK_R("return string.rep('abc', 1e9, ',')", "ERR: t:1: not enough memory");
    CHK_R("return pcall(string.rep, 'x', 1e9)", "ERR: t:1: not enough memory");
    LT_CHECK(lt_lua_mem_used(L) > 0);
    lt_lua_set_limits(L, 0, 64ul * 1024ul * 1024ul);
    lt_lua_set_limits(L, 5000000ul, 0);
    /* a sane script stays well inside */
    CHK_R("local s = 0 for i = 1, 10000 do s = s + i end return s", "50005000");
    st = lt_lua_steps(L);
    LT_CHECK(st > 10000 && st < 200000);
    /* output limit */
    CHK_R("local s = ('x'):rep(1000) for i = 1, 5000 do print(s) end", "ERR: t:1: output limit exceeded");
    /* deep nesting is rejected by the parser, not the C stack */
    {
        char *big = (char *)lt_malloc(40000);
        size_t i, n = 0;
        strcpy(big, "return ");
        n = strlen(big);
        for (i = 0; i < 5000; i++) { big[n++] = '('; }
        big[n++] = '1';
        for (i = 0; i < 5000; i++) { big[n++] = ')'; }
        big[n] = '\0';
        LT_CHECK(has(R(big), "too many syntax levels"));
        n = 0;
        strcpy(big, "x = ");
        n = strlen(big);
        for (i = 0; i < 5000; i++) { big[n++] = '{'; }
        big[n] = '\0';
        LT_CHECK(has(R(big), "ERR:"));
        n = 0;
        for (i = 0; i < 3000; i++) { memcpy(big + n, "if 1 then ", 10); n += 10; }
        big[n] = '\0';
        LT_CHECK(has(R(big), "too many syntax levels"));
        n = 0;
        strcpy(big, "return ");
        n = strlen(big);
        for (i = 0; i < 4000; i++) { big[n++] = '-'; big[n++] = ' '; }
        strcpy(big + n, "1");
        LT_CHECK(has(R(big), "too many syntax levels"));
        lt_free(big);
    }
    /* long concatenation chain and long argument lists are fine */
    {
        char *big = (char *)lt_malloc(40000);
        size_t i, n;
        strcpy(big, "return 'a'");
        n = strlen(big);
        for (i = 0; i < 90; i++) { memcpy(big + n, " .. 'b'", 7); n += 7; }
        big[n] = '\0';
        LT_CHECK(strlen(R(big)) == 91);
        lt_free(big);
    }
    CHK_R("local t = {} for i = 1, 7500 do t[i] = i end return select('#', table.unpack(t))",
       "ERR: t:1: too many results to unpack");
    CHK_R("local t = {} for i = 1, 5000 do t[i] = 'x' end return #table.concat(t)", "5000");
    lt_lua_set_limits(L, LT_LUA_DEF_STEPS, LT_LUA_DEF_MEM);
}

LT_TEST(misc_semantics)
{
    CHK_R("local t = {} local function f() return t end f().x = 1 return t.x", "1");
    CHK_R("local a = {b = {c = 5}} return a.b.c, a['b']['c']", "5\t5");
    CHK_R("local s = 0 for i = 1, 3 do for j = 1, 3 do if j == 2 then break end s = s + 1 end end return s", "3");
    CHK_R("local x = 10 local function f() local x = 20 return x end return f(), x", "20\t10");
    CHK_R("local r = {} for i = 1, 3 do r[#r + 1] = (function() return i * i end)() end return table.concat(r, ',')", "1,4,9");
    CHK_R("local t = {f = function(self, x) return x end} return t:f(7), t.f(t, 8)", "7\t8");
    CHK_R("return (function() return 1, 2 end)()", "1\t2");
    CHK_R("local a <const> = 1", "ERR: t:1: unexpected symbol near '<'");
    LT_CHECK(has(R("goto done"), "ERR:"));
    CHK_R("return 1; ", "1");
    CHK_R(";;; return 2", "2");
    CHK_R("-- comment\nreturn --[[ inline ]] 3", "3");
    CHK_R("#!shebang\nreturn 4", "4");
    CHK_R("return _VERSION:sub(1, 3), _G.print == print", "Lua\ttrue");
    CHK_R("_G.newglobal = 1 return newglobal", "1");
    CHK_R("local t = {[1] = 'a', [2] = 'b'} return #t, t[1] .. t[2]", "2\tab");
    CHK_R("return 5 // 0 == 1/0, 5 % math.huge", "true\t5");
    CHK_R("local x = 5 x = x + 1 x = x * 2 return x", "12");
    CHK_R("local a = {10, 20, 30} local i = 1 i, a[i] = 2, 99 return a[1], a[2]", "99\t20");
    CHK_R("return select('#', nil, nil), select('#')", "2\t0");
    CHK_R("local function f() end return f(), 1", "nil\t1");
    CHK_R("local function f() end return (f())", "nil");
    CHK_R("local t = {f()}", "ERR: t:1: attempt to call a nil value (global 'f')");
    CHK_R("return 2^31, 2^32 + 1, -2^31", "2147483648\t4294967297\t-2147483648");
    CHK_R("return 1e300 * 1e10, -(0/0) ~= -(0/0)", "inf\ttrue");
    CHK_R("return math.floor(-0.5), 0 == -0", "-1\ttrue");
    CHK_R("local t = {} t[1] = 1 t[3] = 3 t[2] = 2 return #t", "3");
    CHK_R("return #arg", "ERR: t:1: attempt to get length of a nil value (global 'arg')");
    CHK_R("return os", "nil");
    CHK_R("return io, require, load, dofile, loadstring", "nil\tnil\tnil\tnil\tnil");
    LT_CHECK(lt_lua_mem_used(L) > 0);
}

/* random garbage and mutated programs must never crash (run under ASan/UBSan) */
static unsigned long rs = 0x1234567ul;
static unsigned long rnd(void)
{
    rs = (rs * 1103515245ul + 12345ul) & 0x7FFFFFFFul;
    return rs >> 8;
}

LT_TEST(fuzz_bytes)
{
    char buf[300] = "";
    int i;
    size_t j, n;
    lt_lua_set_limits(L, 20000, 1000000);
    for (i = 0; i < 1500; i++) {
        n = (size_t)(rnd() % 120u);
        for (j = 0; j < n; j++) { buf[j] = (char)(rnd() & 0xFFu); }
        (void)lt_lua_run(L, buf, n, "fz");
    }
    lt_lua_set_limits(L, LT_LUA_DEF_STEPS, LT_LUA_DEF_MEM);
    LT_CHECK(1);
}

LT_TEST(fuzz_mutations)
{
    static const char *const seeds[] = {
        "local t = {1, 2, 3, x = 'y'} for k, v in pairs(t) do print(k, v) end return #t",
        "local function f(n) if n < 2 then return n end return f(n - 1) + f(n - 2) end return f(8)",
        "local s = 'hello world' return s:gsub('(%w+)', '<%1>'), s:find('o w'), s:format()",
        "local mt = {__index = function(t, k) return k end} local t = setmetatable({}, mt) return t.a .. t[1]",
        "return string.format('%5.2f|%-5d|%s|%x', 3.14159, 42, 'x', 255)",
        "local ok, e = pcall(function(...) local a, b = ... return a.b.c end, 1) return ok, e",
        "local r = {} for w in ('a b c'):gmatch('%w') do r[#r + 1] = w end table.sort(r, function(a, b) return a > b end) return table.concat(r)",
        "while true do local x = {} x[1] = x end",
        "repeat local a = 1 until a == 1 return [[x]] .. [==[y]==] .. '\\65\\x41\\z  z'"
    };
    char buf[400];
    int s, i;
    lt_lua_set_limits(L, 30000, 1000000);
    for (s = 0; s < (int)(sizeof(seeds) / sizeof(seeds[0])); s++) {
        size_t len = strlen(seeds[s]);
        for (i = 0; i < 400; i++) {
            size_t k, nm = 1 + rnd() % 4u;
            memcpy(buf, seeds[s], len);
            for (k = 0; k < nm; k++) {
                size_t pos = rnd() % len;
                switch (rnd() % 3u) {
                case 0: buf[pos] = (char)(32 + rnd() % 95u); break;
                case 1: buf[pos] = "(){}[]=,.:;'\"%-+*/#^<>~"[rnd() % 22u]; break;
                default: buf[pos] = ' '; break;
                }
            }
            (void)lt_lua_run(L, buf, len, "fz");
        }
        /* every prefix */
        for (i = 0; i < (int)len; i++) { (void)lt_lua_run(L, seeds[s], (size_t)i, "fz"); }
    }
    lt_lua_set_limits(L, LT_LUA_DEF_STEPS, LT_LUA_DEF_MEM);
    LT_CHECK(1);
}

LT_TEST(fuzz_runtime_values)
{
    /* every library function called with odd arguments: errors are fine, crashes are not */
    static const char *const fns[] = {
        "print", "type", "tostring", "tonumber", "next", "pairs", "ipairs", "select", "unpack", "error",
        "assert", "pcall", "rawget", "rawset", "rawlen", "setmetatable", "getmetatable",
        "string.sub", "string.rep", "string.byte", "string.char", "string.format", "string.find",
        "string.match", "string.gmatch", "string.gsub", "string.upper", "string.reverse", "string.len",
        "math.floor", "math.max", "math.random", "math.fmod", "math.log", "math.atan",
        "table.insert", "table.remove", "table.concat", "table.sort", "table.unpack", "table.pack"
    };
    static const char *const args[] = {
        "", "nil", "1", "-1", "0", "2^53", "1e300", "-1e300", "0/0", "1/0", "'x'", "''", "'%'", "'%d'",
        "'(%'", "'[a'", "'%b'", "'%f'", "'(()'", "{}", "{1, 2}", "print", "true",
        "nil, nil", "1, 2", "'a', 'b'", "'abc', 2, 1", "{}, 1, 2", "'x', '%1'", "'abc', '(a)(b)', '%3'",
        "'x', -1e300", "'x', 1e300", "{1, 'x'}, 1, 2", "{3, 1, 2}, print"
    };
    char src[200];
    size_t i, j;
    lt_lua_set_limits(L, 100000, 4000000);
    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        for (j = 0; j < sizeof(args) / sizeof(args[0]); j++) {
            lt_snprintf(src, sizeof(src), "return pcall(%s%s%s)", fns[i], args[j][0] ? ", " : "", args[j]);
            (void)lt_lua_run(L, src, strlen(src), "fz");
            lt_snprintf(src, sizeof(src), "return %s(%s)", fns[i], args[j]);
            (void)lt_lua_run(L, src, strlen(src), "fz");
        }
    }
    lt_lua_set_limits(L, LT_LUA_DEF_STEPS, LT_LUA_DEF_MEM);
    LT_CHECK(1);
}

LT_TEST(number_conversion)
{
    char b[48];
    double d = 0;
    lt_lua_fmt_number(b, sizeof b, 42.0); LT_CHECK_STR(b, "42");
    lt_lua_fmt_number(b, sizeof b, -0.0); LT_CHECK_STR(b, "0");
    lt_lua_fmt_number(b, sizeof b, 0.5); LT_CHECK_STR(b, "0.5");
    lt_lua_fmt_number(b, sizeof b, 1e100); LT_CHECK_STR(b, "1e+100");
    lt_lua_fmt_number(b, sizeof b, 1.0 / 0.0 * 0.0); LT_CHECK_STR(b, "nan");
    LT_CHECK(lt_lua_parse_number(" 0x10 ", 6, &d) && d == 16.0);
    LT_CHECK(lt_lua_parse_number("-1.5e2", 6, &d) && d == -150.0);
    LT_CHECK(!lt_lua_parse_number("1e", 2, &d));
    LT_CHECK(!lt_lua_parse_number(".", 1, &d));
    LT_CHECK(!lt_lua_parse_number("", 0, &d));
    LT_CHECK(!lt_lua_parse_number("0x", 2, &d));
    LT_CHECK(!lt_lua_parse_number("infinity", 8, &d));
    LT_CHECK(!lt_lua_parse_number("1 1", 3, &d));
}

int main(void)
{
    L = lt_lua_new();
    LT_RUN(number_conversion);
    LT_RUN(basics);
    LT_RUN(variables_and_control);
    LT_RUN(functions_and_closures);
    LT_RUN(tables);
    LT_RUN(strings);
    LT_RUN(format);
    LT_RUN(math_lib);
    LT_RUN(errors);
    LT_RUN(metatables);
    LT_RUN(host_functions);
    LT_RUN(host_api);
    LT_RUN(limits);
    LT_RUN(misc_semantics);
    LT_RUN(fuzz_bytes);
    LT_RUN(fuzz_mutations);
    LT_RUN(fuzz_runtime_values);
    lt_lua_free(L);
    LT_TEST_MAIN_END
}
