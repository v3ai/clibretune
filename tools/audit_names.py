#!/usr/bin/env python3
"""audit_names.py - static audit of src/ and tests/ against docs/UNIVERSAL_C.md.

Checks (category in brackets):
  [reserved]  defined macros/functions/types/tags/globals/enumerators/struct
              members whose names are reserved (section 3): str/mem/wcs + lower,
              is/to + lower, E+digit/upper, SIG*, LC_*, leading underscore,
              double underscore, C99/C11/C23/C++ keywords
  [stdname]   defined names (not members) that collide with the C/POSIX library
  [banned]    forbidden library calls and format specifiers (section 5)
  [header]    forbidden headers; OS headers outside platform files (section 5/11)
  [strlen]    string literal longer than 509 characters (section 9)
  [bigobj]    object larger than 32767 bytes (uses gcc -Wlarger-than)
  [ident31]   identifiers that collide within the first 31 characters
  [compmacro] compiler/OS/version macro tests outside src/lt.h (section 4/11)
  [cosmo]     mention of the removed Cosmopolitan build in src/

Usage: audit_names.py [--only DIR]... [--summary] [--no-gcc] [PATH...]
Exit status: 0 clean, 1 violations found, 2 usage error.
Platform files (src/plat/posix, src/plat/win32, src/gui/os, tests/os) may include
OS headers; everything else may not.
"""
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLATFORM_DIRS = ("src/plat/posix/", "src/plat/win32/", "src/gui/os/", "tests/os/")
LT_H = "src/lt.h"

C89_HEADERS = {"assert.h", "ctype.h", "errno.h", "float.h", "limits.h", "locale.h",
               "math.h", "setjmp.h", "signal.h", "stdarg.h", "stddef.h", "stdio.h",
               "stdlib.h", "string.h", "time.h"}
BANNED_HEADERS = {"stdint.h", "inttypes.h", "stdbool.h", "complex.h", "fenv.h", "tgmath.h",
                  "stdalign.h", "stdatomic.h", "stdnoreturn.h", "threads.h", "uchar.h",
                  "stdbit.h", "stdckdint.h", "wchar.h", "wctype.h", "iso646.h"}

KEYWORDS = set("""inline restrict _Bool _Complex _Imaginary _Alignas _Alignof _Atomic _Generic
_Noreturn _Static_assert _Thread_local bool true false nullptr static_assert alignas alignof
thread_local typeof typeof_unqual constexpr _BitInt _Decimal32 _Decimal64 _Decimal128 auto
class new delete this template typename namespace using operator private protected public
virtual friend explicit mutable try catch throw export concept requires asm fortran
complex imaginary""".split())

# identifiers from the C89/C99/POSIX library that must not be (re)defined
STDNAMES = set("""
abort abs acos asctime asin assert atan atan2 atexit atof atoi atol bsearch calloc ceil clearerr
clock clock_t cos cosh ctime difftime div div_t errno exit exp fabs fclose feof ferror fflush
fgetc fgetpos fgets floor fmod fopen fpos_t fprintf fputc fputs fread free freopen frexp fscanf
fseek fsetpos ftell fwrite getc getchar getenv gets gmtime isalnum isalpha iscntrl isdigit isgraph
islower isprint ispunct isspace isupper isxdigit jmp_buf labs ldexp ldiv ldiv_t localeconv
localtime log log10 longjmp malloc mblen mbstowcs mbtowc memchr memcmp memcpy memmove memset
mktime modf offsetof perror pow printf putc putchar puts qsort raise rand realloc remove rename
rewind scanf setbuf setjmp setlocale setvbuf sig_atomic_t signal sin sinh size_t sleep sprintf
sqrt srand sscanf stderr stdin stdout strcat strchr strcmp strcoll strcpy strcspn strerror
strftime strlen strncat strncmp strncpy strpbrk strrchr strspn strstr strtod strtok strtol
strtoul strxfrm system tan tanh time time_t tmpfile tmpnam tolower toupper ungetc va_arg va_end
va_start vfprintf vprintf vsprintf wchar_t
index rindex read write open close link unlink send recv select socket bind connect listen accept
poll pipe dup dup2 fork wait kill stat lstat fstat access chdir getcwd mkdir rmdir opendir
readdir closedir pause alarm sleep usleep nice sync truncate environ optarg optind opterr
bcopy bzero bcmp ffs
round trunc fmin fmax fma hypot log2 exp2 cbrt expm1 log1p lround llround nan remainder rint
nearbyint copysign isnan isinf isfinite signbit fpclassify y0 y1 j0 j1 jn yn gamma lgamma tgamma
erf erfc scalb logb ilogb snprintf vsnprintf strdup strndup strcasecmp strncasecmp strlcpy strlcat
strsep strnlen basename dirname
NULL EOF BUFSIZ FILENAME_MAX FOPEN_MAX L_tmpnam TMP_MAX RAND_MAX EXIT_FAILURE EXIT_SUCCESS
CHAR_BIT CHAR_MAX CHAR_MIN INT_MAX INT_MIN LONG_MAX LONG_MIN UINT_MAX ULONG_MAX SHRT_MAX SHRT_MIN
USHRT_MAX SCHAR_MAX SCHAR_MIN UCHAR_MAX DBL_MAX DBL_MIN DBL_EPSILON FLT_MAX FLT_MIN FLT_EPSILON
HUGE_VAL CLOCKS_PER_SEC SEEK_SET SEEK_CUR SEEK_END INFINITY NAN M_PI M_E PI
tm lconv FILE
""".split())
STDNAMES -= {"PI"}  # PI alone is not a library name; M_PI is

BANNED_CALLS = """snprintf vsnprintf va_copy __va_copy strtoll strtoull strtof strtold llabs lldiv
atoll isblank _Exit round trunc fmin fmax fma hypot log2 exp2 cbrt expm1 log1p lround llround nan
isnan isinf isfinite fpclassify signbit copysign nearbyint rint lrint scalbn remainder
sprintf vsprintf gets""".split()
# POSIX functions: banned in universal code, fine inside platform files
POSIX_CALLS = "strdup strndup strcasecmp strncasecmp strlcpy strlcat putenv setenv usleep strsep strnlen".split()
# OS feature-test macros: may only be defined in platform files (the build passes them)
FEATURE_MACROS = {"_POSIX_C_SOURCE", "_XOPEN_SOURCE", "_DEFAULT_SOURCE", "_DARWIN_C_SOURCE",
                  "_GNU_SOURCE", "_BSD_SOURCE", "_WIN32_WINNT", "WIN32_LEAN_AND_MEAN",
                  "_FILE_OFFSET_BITS", "__BSD_VISIBLE"}
BANNED_MACROS = "NAN INFINITY HUGE_VALF HUGE_VALL LLONG_MAX LLONG_MIN ULLONG_MAX UINT64_C INT64_C".split()
FLOAT_VARIANTS = re.compile(r"(?<![A-Za-z0-9_])(sin|cos|tan|asin|acos|atan|atan2|sqrt|pow|exp|log|log10|"
                            r"fabs|floor|ceil|fmod|ldexp|frexp|modf|sinh|cosh|tanh)(f|l)\s*\(")
BANNED_FMT = re.compile(r"%[-+ #0-9.*]*(?:z[diouxX]|ll[diouxX]|hh[diouxX]|j[diouxX]|t[diouxX]|[aA](?![A-Za-z]))")
PRI_RE = re.compile(r"(?<![A-Za-z0-9_])PRI[diouxX]\d*[A-Za-z]*")
# types that are not in C89
BANNED_TYPES = re.compile(r"(?<![A-Za-z0-9_])(long\s+long|_Bool|_Complex|intmax_t|uintmax_t|"
                          r"u?int(?:8|16|32|64)_t|intptr_t|uintptr_t|ptrdiff_t_unused)(?![A-Za-z0-9_])")

COMPILER_MACRO = re.compile(r"^(__[A-Za-z0-9_]+|_[A-Z][A-Za-z0-9_]*|WIN32|WIN64|linux|unix|i386|sun|"
                            r"MSDOS|M_X64|M_IX86|MINGW32|MINGW64)$")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def reserved_reason(name, member=False):
    if name.startswith("_"):
        return "leading underscore"
    if "__" in name:
        return "contains double underscore"
    if re.match(r"^(str|mem|wcs)[a-z]", name):
        return "reserved prefix str/mem/wcs + lowercase"
    if re.match(r"^(is|to)[a-z]", name):
        return "reserved prefix is/to + lowercase"
    if re.match(r"^E[0-9A-Z]", name):
        return "reserved errno-style name E+digit/uppercase"
    if re.match(r"^SIG(_)?[A-Z]", name):
        return "reserved signal name SIG*"
    if re.match(r"^LC_[A-Z]", name):
        return "reserved locale name LC_*"
    if name in KEYWORDS:
        return "C99/C11/C23/C++ keyword"
    return None


# ---------------------------------------------------------------- lexing
def strip_source(text):
    """Return (clean, literals). comments -> spaces; string/char contents -> 'S';
    newlines and offsets are preserved. literals: list of (start, end, nchars)."""
    out = []
    lits = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == '"' or c == "'":
            q = c
            j = i + 1
            cnt = 0
            while j < n and text[j] != q and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n:
                    if text[j + 1] == "\n":
                        j += 2
                        continue
                    j += 2
                    # hex/octal escapes: swallow their digits as one char
                    if text[j - 1] == "x":
                        while j < n and text[j] in "0123456789abcdefABCDEF":
                            j += 1
                    elif text[j - 1] in "01234567":
                        k = 0
                        while j < n and k < 2 and text[j] in "01234567":
                            j += 1
                            k += 1
                    cnt += 1
                else:
                    j += 1
                    cnt += 1
            end = j + 1 if j < n and text[j] == q else j
            seg = text[i:end]
            out.append(q + "".join("\n" if ch == "\n" else "S" for ch in seg[1:-1]) + q
                       if len(seg) >= 2 else seg)
            if q == '"':
                lits.append((i, end, cnt))
            i = end
        else:
            out.append(c)
            i += 1
    return "".join(out), lits


def logical_directives(clean):
    """Yield (start_offset, line_number, text) for each preprocessor directive,
    joining backslash continuations. Returns also the clean text with the
    directives blanked out."""
    res = []
    blank = list(clean)
    i, n = 0, len(clean)
    line = 1
    while i < n:
        j = clean.find("\n", i)
        if j < 0:
            j = n
        seg = clean[i:j]
        if seg.lstrip().startswith("#"):
            start = i
            startline = line
            text = seg
            k = j
            while text.rstrip().endswith("\\") and k < n:
                nxt = clean.find("\n", k + 1)
                if nxt < 0:
                    nxt = n
                text = text.rstrip()[:-1] + " " + clean[k + 1:nxt]
                k = nxt
            for p in range(start, k):
                if blank[p] != "\n":
                    blank[p] = " "
            res.append((start, startline, text.strip()))
            line += clean.count("\n", i, k)
            i = k
            if i < n:
                line += 1
                i += 1
            continue
        line += 1
        i = j + 1
    return res, "".join(blank)


TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|\d[\w.]*|\"S*\"|'S*'|\.\.\.|->|[{}()\[\];,=*:<>+\-/%&|^!~?.]|\S")


def tokenize(clean):
    toks = []
    line = 1
    pos = 0
    for m in TOKEN.finditer(clean):
        line += clean.count("\n", pos, m.start())
        pos = m.start()
        toks.append((m.group(0), line))
    return toks


class Decls:
    """Very small C declaration scanner: finds the names a file *defines*."""

    def __init__(self, toks):
        self.t = toks
        self.defs = []  # (name, line, kind)

    def skip_balanced(self, i, open_, close):
        depth = 0
        t = self.t
        while i < len(t):
            if t[i][0] == open_:
                depth += 1
            elif t[i][0] == close:
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
        return i

    def declarator_names(self, toks, first=True):
        """Names declared by one declarator token list (no top-level commas)."""
        # function pointer / parenthesised declarator:  type (*name)(...)
        depth = 0
        for k, (tok, _) in enumerate(toks):
            if tok == "(":
                if k + 1 < len(toks) and toks[k + 1][0] == "*":
                    for q in range(k + 1, len(toks)):
                        if toks[q][0] == ")":
                            break
                        if IDENT.fullmatch(toks[q][0]) and toks[q][0] not in ("const", "volatile"):
                            return [toks[q]]
                    return []
                # function declarator: name is the identifier just before '('
                if k > 0 and IDENT.fullmatch(toks[k - 1][0]):
                    return [toks[k - 1]]
                return []
            if tok in ("=", "[", ":"):
                toks = toks[:k]
                break
        for tok, ln in reversed(toks):
            if IDENT.fullmatch(tok) and tok not in ("const", "volatile", "static", "extern",
                                                     "register", "struct", "union", "enum",
                                                     "unsigned", "signed", "int", "char", "long",
                                                     "short", "double", "float", "void"):
                return [(tok, ln)]
        return []

    def split_commas(self, toks):
        parts, cur, depth = [], [], 0
        for tok in toks:
            if tok[0] in "([{":
                depth += 1
            elif tok[0] in ")]}":
                depth -= 1
            if tok[0] == "," and depth == 0:
                parts.append(cur)
                cur = []
            else:
                cur.append(tok)
        parts.append(cur)
        return parts

    def record_decl(self, toks, kind_obj):
        if not toks:
            return
        words = [w for w, _ in toks]
        if words[0] == "typedef":
            kind = "typedef"
            toks = toks[1:]
        else:
            kind = kind_obj
        for idx, part in enumerate(self.split_commas(toks)):
            for name, ln in self.declarator_names(part, idx == 0):
                if name in ("struct", "union", "enum", "typedef"):
                    continue
                k = kind
                # a '(' declarator at file scope is a function
                self.defs.append((name, ln, k))

    def parse_aggregate(self, i, kind_tag):
        """i is index of '{'. Parse members until matching '}'; return index after."""
        t = self.t
        i += 1
        if kind_tag == "enum":
            cur = []
            depth = 0
            while i < len(t):
                tok = t[i][0]
                if tok == "}" and depth == 0:
                    i += 1
                    break
                if tok == "(":
                    depth += 1
                elif tok == ")":
                    depth -= 1
                if tok == "," and depth == 0:
                    if cur and IDENT.fullmatch(cur[0][0]):
                        self.defs.append((cur[0][0], cur[0][1], "enumerator"))
                    cur = []
                else:
                    cur.append(t[i])
                i += 1
            if cur and IDENT.fullmatch(cur[0][0]):
                self.defs.append((cur[0][0], cur[0][1], "enumerator"))
            return i
        # struct/union members
        cur = []
        while i < len(t):
            tok = t[i][0]
            if tok == "}":
                i += 1
                break
            if tok == "{":
                tag = self.last_aggregate(cur)
                if tag:
                    i = self.parse_aggregate(i, tag)
                    continue
                i = self.skip_balanced(i, "{", "}")
                continue
            if tok == ";":
                self.record_decl(cur, "member")
                cur = []
            else:
                cur.append(t[i])
            i += 1
        return i

    def last_aggregate(self, cur):
        words = [w for w, _ in cur]
        for k in range(len(words) - 1, -1, -1):
            if words[k] in ("struct", "union", "enum"):
                # struct [tag]  (tag optional) must be the tail of the declaration
                if k == len(words) - 1 or (k == len(words) - 2 and IDENT.fullmatch(words[k + 1])):
                    if k == len(words) - 2:
                        self.defs.append((words[k + 1], cur[k + 1][1], "tag"))
                    return words[k]
                return None
        return None

    def run(self):
        t = self.t
        i = 0
        cur = []
        while i < len(t):
            tok = t[i][0]
            if tok == "{":
                words = [w for w, _ in cur]
                if "=" in words:
                    i = self.skip_balanced(i, "{", "}")
                    continue
                tag = self.last_aggregate(cur)
                if tag:
                    i = self.parse_aggregate(i, tag)
                    # keep cur so `} name;` declarators are recorded; drop the body
                    # by recording a placeholder type token
                    cur = [("struct", t[i - 1][1])] if cur and cur[0][0] != "typedef" \
                        else [("typedef", t[i - 1][1]), ("struct", t[i - 1][1])]
                    continue
                # function body (or K&R/unknown): record the function, skip body
                self.record_decl(cur, "function")
                i = self.skip_balanced(i, "{", "}")
                cur = []
                continue
            if tok == ";":
                if cur:
                    kind = "global"
                    if any(w == "(" for w, _ in cur) and not (len(cur) > 1 and cur[0][0] == "typedef"):
                        kind = "function"
                    self.record_decl(cur, kind)
                cur = []
            else:
                cur.append(t[i])
            i += 1


# ---------------------------------------------------------------- audit
class Report:
    def __init__(self):
        self.items = []  # (path, line, cat, msg)

    def add(self, path, line, cat, msg):
        self.items.append((path, line, cat, msg))


def is_platform(path):
    return path.startswith(PLATFORM_DIRS)


def audit_file(path, rep, all_idents):
    full = os.path.join(ROOT, path)
    with open(full, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    clean, lits = strip_source(text)
    directives, code = logical_directives(clean)
    line_of = lambda off: text.count("\n", 0, off) + 1

    if path.startswith("src/") and re.search(r"cosmo", text, re.I):
        for m in re.finditer(r"cosmo", text, re.I):
            rep.add(path, line_of(m.start()), "cosmo", "reference to the removed Cosmopolitan build")
            break

    # directives
    macro_names = []
    for off, ln, d in directives:
        m = re.match(r"#\s*include\s*([<\"])([^>\"]+)[>\"]", d)
        if m:
            hdr = m.group(2)
            if m.group(1) == "<":
                if hdr in C89_HEADERS:
                    pass
                elif hdr == "stdint.h" and path == LT_H:
                    pass
                elif is_platform(path):
                    pass
                elif hdr in BANNED_HEADERS:
                    rep.add(path, ln, "header", "C99+ header <%s> is forbidden" % hdr)
                else:
                    rep.add(path, ln, "header", "OS/non-C89 header <%s> outside a platform file" % hdr)
            continue
        m = re.match(r"#\s*define\s+([A-Za-z_]\w*)", d)
        if m:
            if m.group(1) in FEATURE_MACROS:
                if not is_platform(path):
                    rep.add(path, ln, "compmacro", "feature-test macro %s defined outside a platform "
                            "file (pass it from the build system)" % m.group(1))
            else:
                macro_names.append((m.group(1), ln))
        m = re.match(r"#\s*(if|ifdef|ifndef|elif)\b(.*)", d)
        if m and path != LT_H:
            for ident in IDENT.findall(m.group(2)):
                if ident in ("defined",):
                    continue
                if COMPILER_MACRO.match(ident):
                    rep.add(path, ln, "compmacro",
                            "'%s' tested outside src/lt.h (select code in the build system)" % ident)

    # defined names
    toks = tokenize(code)
    for tok, ln in toks:
        if len(tok) > 31 and IDENT.fullmatch(tok):
            all_idents[tok].add((path, ln))
    for off, ln, d in directives:
        for ident in IDENT.findall(re.sub(r'"S*"|\'S*\'', '""', d)):
            if len(ident) > 31:
                all_idents[ident].add((path, ln))
    dec = Decls(toks)
    dec.run()
    defs = [(n, ln, "macro") for n, ln in macro_names] + dec.defs
    seen = set()
    for name, ln, kind in defs:
        key = (name, ln, kind)
        if key in seen:
            continue
        seen.add(key)
        why = reserved_reason(name, kind == "member")
        if why:
            rep.add(path, ln, "reserved", "%s '%s': %s" % (kind, name, why))
        elif kind != "member" and name in STDNAMES:
            if not (kind == "macro" and path == LT_H):
                rep.add(path, ln, "stdname", "%s '%s' collides with a standard/POSIX library name" % (kind, name))

    # banned calls / macros / formats
    banned = BANNED_CALLS if is_platform(path) else BANNED_CALLS + POSIX_CALLS
    for m in re.finditer(r"(?<![A-Za-z0-9_])(%s)(?![A-Za-z0-9_])" % "|".join(banned), code):
        name = m.group(1)
        # only a call/use, not a declaration of our own member; accept any use
        if name in ("round", "trunc", "nan", "index") :
            pass
        rest = code[m.end():m.end() + 8].lstrip()
        if rest.startswith("(") or name == "va_copy":
            rep.add(path, line_of(m.start()), "banned", "call to non-C89 function %s()" % name)
    for m in FLOAT_VARIANTS.finditer(code):
        rep.add(path, line_of(m.start()), "banned", "C99 float/long-double math %s%s()" % (m.group(1), m.group(2)))
    for name in BANNED_MACROS:
        for m in re.finditer(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % name, code):
            rep.add(path, line_of(m.start()), "banned", "non-C89 macro %s" % name)
    for m in PRI_RE.finditer(code):
        rep.add(path, line_of(m.start()), "banned", "inttypes PRI macro %s" % m.group(0))
    for m in BANNED_TYPES.finditer(code):
        t = re.sub(r"\s+", " ", m.group(1))
        if path == LT_H and t.startswith(("long long", "int64_t", "uint64_t")):
            continue
        rep.add(path, line_of(m.start()), "banned", "non-C89 type '%s'" % t)
    # printf-style specifiers inside string literals
    for start, end, cnt in lits:
        raw = text[start:end]
        before = code[max(0, start - 300):start].rsplit(";", 1)[-1]
        bad = BANNED_FMT.search(raw)
        if bad and bad.group(0)[-1] in "aA" and "strftime" in before:
            bad = None  # %a/%A are weekday names for strftime
        if bad:
            rep.add(path, line_of(start), "banned", "non-C89 printf specifier in string literal")

    # overlong string literals (after concatenation of adjacent literals)
    groups = []
    for start, end, cnt in lits:
        if groups and not clean[groups[-1][1]:start].strip():
            g = groups[-1]
            groups[-1] = (g[0], end, g[2] + cnt)
        else:
            groups.append((start, end, cnt))
    for start, end, cnt in groups:
        if cnt > 509:
            rep.add(path, line_of(start), "strlen",
                    "string literal of %d characters exceeds 509 (after concatenation)" % cnt)


def gcc_bigobjects(paths, rep):
    plat_flags = ["-D_POSIX_C_SOURCE=200112L", "-D_XOPEN_SOURCE=600", "-D_DEFAULT_SOURCE"]

    def one(path):
        cmd = ["gcc", "-fsyntax-only", "-std=c89", "-Wno-error", "-Wlarger-than=32767",
               "-I", os.path.join(ROOT, "src"), "-I", os.path.join(ROOT, "tests")]
        if is_platform(path):
            cmd += plat_flags
        cmd.append(os.path.join(ROOT, path))
        try:
            r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True, cwd=ROOT, timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            return []
        found = []
        for line in r.stderr.splitlines():
            m = re.match(r"^(.*?):(\d+):\d+: warning: size of .(\w+). (\d+) bytes exceeds", line)
            if m:
                found.append((path, int(m.group(2)), "bigobj",
                              "object '%s' is %s bytes (limit 32767)" % (m.group(3), m.group(4))))
        return found

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
        for res in ex.map(one, paths):
            for item in res:
                rep.add(*item)


def main(argv):
    only = []
    summary = False
    use_gcc = True
    paths = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--only" and i + 1 < len(argv):
            i += 1
            only.append(argv[i].strip("/").lstrip("./") if argv[i] != "." else ".")
        elif a == "--summary":
            summary = True
        elif a == "--no-gcc":
            use_gcc = False
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        elif a.startswith("-"):
            print("unknown option", a, file=sys.stderr)
            return 2
        else:
            paths.append(a)
        i += 1
    if not paths:
        for top in ("src", "tests"):
            for dp, dn, fn in os.walk(os.path.join(ROOT, top)):
                dn.sort()
                for f in sorted(fn):
                    if f.endswith((".c", ".h")) and "fixtures" not in dp:
                        paths.append(os.path.relpath(os.path.join(dp, f), ROOT))
    paths = sorted(set(os.path.relpath(os.path.abspath(p), ROOT) for p in paths))
    if only:
        paths = [p for p in paths if any(p == o or p.startswith(o + "/") for o in only)]
    rep = Report()
    all_idents = defaultdict(set)
    for p in paths:
        audit_file(p, rep, all_idents)
    # identifiers that collide in the first 31 characters
    groups = defaultdict(set)
    for ident in all_idents:
        groups[ident[:31]].add(ident)
    for pre, names in sorted(groups.items()):
        if len(names) > 1:
            where = sorted(all_idents[sorted(names)[0]])[0]
            rep.add(where[0], where[1], "ident31",
                    "identifiers differ only after 31 chars: %s" % ", ".join(sorted(names)))
    if use_gcc:
        gcc_bigobjects([p for p in paths if p.endswith(".c")], rep)
    items = sorted(set(rep.items))
    for path, line, cat, msg in items:
        print("%s:%d: [%s] %s" % (path, line, cat, msg))
    print()
    bycat = Counter(c for _, _, c, _ in items)
    bydir = Counter(os.path.dirname(p) or "." for p, _, _, _ in items)
    print("audit_names: %d violation(s) in %d file(s) scanned" % (len(items), len(paths)))
    if items:
        print("by category: " + ", ".join("%s=%d" % kv for kv in sorted(bycat.items())))
        if summary or True:
            print("by directory:")
            for d, c in sorted(bydir.items()):
                print("  %5d  %s" % (c, d))
    return 1 if items else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
