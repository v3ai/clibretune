# Spec 02 - INI expressions, output channels, realtime decode, UI sections

Source: `libretune-core/src/ini/{expression,output_channels,gauges,diagnostic_logger}.rs`, `realtime/evaluator.rs`, and the [Menu]/[UserDefined]/[TableEditor]/[CurveEditor]/[GaugeConfigurations]/[FrontPage]/[Datalog]/[LoggerDefinition] handlers of `ini/parser.rs`. All C89; use `double` for numbers, `long` is NOT 64-bit portable: use a `lt_i64` typedef (see 0.2).

OUT OF SCOPE: network/TLS/LLM/Lua/WASM/git; `.inc` table loading (`table()` calls a host callback), Tauri event emission, MCP, AI agent, math_channels, pin_conflicts.

## 0. Shared conventions

### 0.1 Strings and memory
Keys are matched case-insensitively (ASCII). INI values are byte strings; treat as ASCII/UTF-8 opaque (the Rust lexer uses Unicode `is_alphabetic`; in C accept ASCII letters, `_`, and any byte >= 0x80 as identifier chars). Maps in the Rust (HashMap) become any string-keyed table; **insertion order is not preserved by Rust but menus/dialog components/frontpage are Vecs and ARE ordered**.

### 0.2 Integer type
Bit ops cast `double -> i64` (truncate toward zero; Rust `as` saturates on overflow and NaN->0). Define `typedef long long lt_i64` where available else a struct/`long` fallback; saturate: NaN -> 0, >= 2^63 -> INT64_MAX, <= -2^63 -> INT64_MIN. i64 -> double on result.

### 0.3 `split_ini_line(value)` (used everywhere below)
Split on `,` only when NOT inside `"..."` and brace depth == 0. Each piece is trimmed (whitespace). Always returns >=1 piece (last piece pushed even if empty). `"` toggles quote state (and is kept). `{`/`}` change depth only when not in quotes (depth may go negative on stray `}`; then commas never split until balanced - replicate: split only when depth == 0 exactly). Quote chars are NOT stripped; callers `trim_matches('"')` (strips ALL leading/trailing `"` chars, not just one).
```c
size_t lt_split_ini_line(const char *v, char ***parts_out); /* returns count; caller frees */
```
Edge: `a, "b,c", {f(x, y)}, d` -> `a` | `"b,c"` | `{f(x, y)}` | `d`.

### 0.4 `strip_comment(line)` (INI line preprocessing, shared by all sections)
1. If first non-whitespace char is `;` -> whole line is empty.
2. has_equals = line contains `=` outside quotes.
3. Scan: `"` toggles in_quotes; first `=` outside quotes sets found_equals; `;` outside quotes: if has_equals and not yet found_equals -> keep (it is `name;+help = value` syntax); if has_equals and found_equals -> truncate here; if !has_equals -> truncate here.
Then trim. Line continuation (before strip): while line matches `\\\s*$` and more lines exist: remove the backslash+trailing ws, append next line trimmed.
`key = value`: split at first `=`, both trimmed (`parse_key_value`). Lines without `=` that are not headers/directives are ignored.
Section header: line starts `[` ends `]`; inner trimmed; if contains `&` it is `[Name &sym1 &!sym2]` conditional header: name = text before first `&`; header suppressed (current section cleared -> following keys ignored) unless every `&sym` is defined (`!sym` = not defined) in the preprocessor symbol set. `UiDialogs` == alias of `UserDefined`. Section dispatch is by lower-cased name: `megatune, tunerstudio, constants, outputchannels, burstmode, gaugeconfigurations, settinggroups, pcvariables, datalog, defaults, menu, userdefined, settingcontexthelp, frontpage, controllercommands, loggerdefinition, porteditor, referencetables, ftpbrowser, datalogviews, keyactions, veanalyze, wueanalyze, gammae, constantsextensions`; any other section whose lower-case name CONTAINS `tableeditor` -> table editor parser; CONTAINS `curveeditor` -> curve editor parser. Unknown, non-passive sections just warn. Passive (silently ignored): `file_header verbiageoverride turbobaud replay extendedreplay accelerationwizard encodeddata tuningviews eventtriggers tools`. Preprocessor (`#if SYM / #else / #endif / #set SYM / #unset SYM / #define / #include`) is covered by another spec; only: false branches skip all content.

### 0.5 Braced-condition helpers
- `unbrace(s)`: strip ALL leading/trailing `{` `}` chars (Rust `trim_matches`) ; in some call sites then `trim()` (noted per site). A piece "is a condition" iff trimmed starts with `{` and ends with `}`.
- "trivial" condition: after unbrace+trim is `""`, `"0"` or `"1"` (menu and panel only; see sections).

## 1. Expression language

### 1.1 Values
```c
typedef enum { LT_V_NUM, LT_V_BOOL, LT_V_STR } lt_vtype;
typedef struct { lt_vtype t; double n; int b; char *s; } lt_value;
```
- `as_f64`: Num->n; Bool->1.0/0.0; Str->**0.0**.
- `as_bool`: Num-> n != 0.0 (NaN is true); Bool->b; Str-> non-empty.
- `to_display_string`: Num: if `fract(n)==0 && |n| < 1e15` -> `%lld` of (i64)n; else Rust `f64::to_string` (shortest round-trip decimal, never exponent for normal magnitudes; C: try `%.15g`,`%.16g`,`%.17g` until strtod round-trips, then expand exponents to plain decimal; "NaN"/"inf" for non-finite). Bool -> `true`/`false`. Str -> itself.

### 1.2 Surrounding braces
INI expressions are written `{ expr }`. The lexer simply DISCARDS every `{` and `}` as whitespace (so `{rpm} / 1000` and `{ a } + { b }` work). Callers that receive text with braces may pass it straight through.

### 1.3 Lexer (exact)
Skip: space, tab, CR, LF, `{`, `}`.
Single-char tokens: `( ) , ? : + - * / % ~ ^`.
Two-char: `!=`->Ne; `!`->Bang; `==`->EqEq; lone `=` -> **lex error** "Unexpected '=' ..."; `<=` Le, `<<` Shl, `<` Lt; `>=` Ge, `>>` Shr, `>` Gt; `&&` AmpAmp, `&` Amp; `||` PipePipe, `|` Pipe.
String: `"` ... `"` (no escapes; unterminated string runs to end, accepted). Token String.
Number: starts with digit, or `.` followed by a digit. Consume digits and `.` (multiple dots allowed in scan; then `strtod`-style parse of whole string must succeed else lex error "Malformed number literal", e.g. `1.2.3` errors). Then if next is `e`/`E`: look ahead for optional `+`/`-` then >=1 digit; only if so consume exponent (so `1exp` = Number 1 then Ident `exp`; note this then yields a trailing-token parse error). No hex, no suffixes.
`$`: identifier `$` + `[alnum_]*` (path functions).
Ident: starts alpha or `_`, continues alnum or `_`. No dots, so `a.b` is a lex error ('.' not followed by digit... a lone `.` -> "Unexpected character").
Anything else: lex error `Unexpected character 'c' in expression`.
Any lex error makes `parse` fail with that message.

### 1.4 Grammar / precedence (lowest -> highest)
```
expr      := logical_or [ '?' expr ':' expr ]            (ternary; right-assoc via recursion; cond is logical_or)
logical_or  := logical_and { '||' logical_and }
logical_and := bit_or { '&&' bit_or }
bit_or    := bit_xor { '|' bit_xor }
bit_xor   := bit_and { '^' bit_and }
bit_and   := equality { '&' equality }                   (& binds LOWER than ==, same as C)
equality  := comparison { ('=='|'!=') comparison }
comparison:= shift { ('<'|'>'|'<='|'>=') shift }
shift     := additive { ('<<'|'>>') additive }
additive  := multiplicative { ('+'|'-') multiplicative }
multiplicative := unary { ('*'|'/'|'%') unary }
unary     := ('-'|'!'|'~') unary | primary
primary   := Number | String | 'true' | 'false' | Ident '(' [expr {',' expr}] ')' | Ident | '(' expr ')'
```
Precedence order is exactly as written: `||` < `&&` < `|` < `^` < `&` < `== !=` < `< > <= >=` < `<< >>` < `+ -` < `* / %` < unary. So `a & 4 == 4` parses as `a & (4 == 4)` (INI authors write `(a & 4) == 4`). All binary ops left-assoc. `true`/`false` are case-sensitive keywords (checked before function-call test). Function names are idents followed by `(`; no whitespace restriction.
Errors (strings): "Unexpected token" (primary got EOF or non-primary), "Expected ')'", "Expected ',' or ')'", "Expected ':' in ternary expression", "Unexpected trailing input after expression (token N of M)", "Expression nested deeper than 64 levels" (depth counter incremented in `parse_expr`, which is the recursion entry for parens, ternary arms, function args; limit MAX_PARSE_DEPTH=64; `-----...1` chain of unary is NOT depth-limited in Rust-Impl-wise but recurses; add a guard in C). Empty input -> "Unexpected token".
AST:
```c
typedef enum { LT_E_LIT, LT_E_VAR, LT_E_BIN, LT_E_TERN, LT_E_UN, LT_E_CALL } lt_etype;
typedef enum { LT_OP_ADD,SUB,MUL,DIV,MOD,EQ,NE,LT,GT,LE,GE,AND,OR,BAND,BOR,BXOR,SHL,SHR } lt_binop;
typedef enum { LT_UN_NEG, LT_UN_NOT, LT_UN_BITNOT } lt_unop;
typedef struct lt_expr lt_expr;   /* tagged union: lit value | var name | bin(l,op,r) | tern(c,a,b) | un(op,e) | call(name,args[],nargs) */
lt_expr *lt_expr_parse(const char *src, char *err, size_t errlen);   /* NULL on error */
void     lt_expr_free(lt_expr*);
```

### 1.5 Evaluation
Context: `string -> double` map (`lt_ctx`) plus optional `lt_strctx` callbacks (1.7).
```c
int lt_expr_eval(const lt_expr*, const lt_ctx*, const lt_strctx*, lt_value *out, char *err, size_t n); /* 0 ok */
```
- **Variable**: lookup exact (case-SENSITIVE) name in ctx; missing -> Num 0.0 (never an error).
- Literal -> itself. Ternary: `as_bool(cond)` selects one branch; only chosen branch evaluated. NOTE `&&`, `||` are NOT short-circuit (both sides always evaluated; matters only for errors in function calls).
- Unary: Neg -> Num(-as_f64); Not -> Bool(!as_bool); BitNot -> Num((double)(~(i64)as_f64)).
- Binary (both operands evaluated first):
  - Add: if BOTH Str -> Str concat; else Num(l.f64 + r.f64) (Str operand counts 0).
  - Sub, Mul: Num. Div: if r.f64 == 0.0 -> Num 0.0 (NOT inf/nan) else l/r. Mod: r==0 -> 0.0 else C `fmod(l, r)` (sign of dividend).
  - Eq/Ne: both Str -> string compare; else Bool(l.f64 == / != r.f64) (exact double compare; Bool vs Num ok).
  - Lt Gt Le Ge: Bool on f64 values (strings -> 0).
  - And/Or: Bool(as_bool op as_bool).
  - BAnd/BOr/BXor: Num((double)((i64)l op (i64)r)).
  - Shl/Shr: count = r.f64; if !finite or not in [0,64) -> result 0.0; else `(i64)l << n` (checked: Rust `checked_shl` only fails for n>=64 so same) / arithmetic `>>` on i64 (sign-extending). Result Num. Use unsigned shift for left to avoid UB then cast.
- Result of boolean-ish ops is `Bool`; consumers use `as_bool`/`as_f64` (true=1.0).

### 1.6 Built-in functions
Name matched case-insensitively (lower-cased); `$`-names too. Arg count mismatch -> error string (`Function X requires N argument(s), got M`). Unknown function -> Err "Unknown function: NAME" (evaluation fails; callers treat as default). All numeric args via `as_f64`.
| fn | args | result |
|---|---|---|
| abs round floor ceil sqrt log exp sin cos tan asin acos atan | 1 | Num. `round` = half away from zero (C `round`), `log` = natural ln |
| pow, atan2 | 2 | `pow(x,y)`; `atan2(x,y)` = C `atan2(x, y)` (arg1 is y-numerator per Rust `x.atan2(y)` i.e. C `atan2(x,y)`) |
| min, max | >=2 | fold with fmin/fmax (NaN-ignoring like Rust f64::min) |
| isNaN | 1 | Bool |
| isAdvancedMathAvailable | any (args not evaluated) | Bool true |
| log10 | 1 | log10(x); if result not finite -> 0.0 |
| recip | 1 | x==0 ? 0.0 : 1/x |
| if | 3 | cond as_bool ? arg2 : arg3 (lazy); returns whichever Value |
| int, trunc | 1 | trunc toward zero |
| not | 1 | Bool(!as_bool) |
| boolean, bool | 1 | Bool(as_bool) |
| pastValue | 2 | evaluates arg1 only, returns it (no history); arg2 ignored/not evaluated |
| getAuxDigital | 1 | evaluates arg, returns Num 0 |
| bitStringValue | 2 | arg1 MUST be a bare variable (else Err "bitStringValue first argument must be a constant name"); idx=(size_t)as_f64(arg2) (negative->0 via saturating cast); if host `get_bit_options(name)` yields list and idx < len -> Str list[idx]; else Str `INVALID[<idx>]` |
| stringValue | 1 | arg is Variable name or string literal (else Err); host `get_string_value(name)` or Str "" |
| table | 2 | arg1 numeric value; arg2 string literal or variable NAME (name text, not value); host `table_lookup(filename, x)`; missing -> Num 0.0 |
| timeNow | 0 | host `start_time` set -> (wallclock_epoch_seconds - start_time) else 0.0 |
| isOnline | 0 | host `is_online()` else Bool false |
| arrayValue | 2 | arg1 variable or string literal NAME; idx = eval(arg2); host `array_value(name, idx)` (interpolating) else Num 0.0 |
| $getProjectsDirPath / $getProjectsDir, $getWorkingDirPath / $getWorkingDir | 0 | host callback string else "" |
Any other `$name` -> Unknown function.

### 1.7 Host context
```c
typedef struct lt_strctx {
  void *ud;
  const char *(*get_string_value)(void*ud,const char*name);        /* may be NULL */
  const char *const *(*get_bit_options)(void*ud,const char*name,size_t*n);
  const char *(*get_projects_dir)(void*); const char *(*get_working_dir)(void*);
  int  (*table_lookup)(void*,const char*file,double x,double *out);
  int  (*is_online)(void*);
  int  have_start_time; double start_time;   /* epoch seconds */
  int  (*array_value)(void*,const char*name,double idx,double *out);
} lt_strctx;
```
### 1.8 Stateful helper (`ExpressionState`; NOT reachable from the expression parser - library only, port optionally)
Per-channel `{last, max, min (optional), accumulator, smoothed (opt), last_update_time, max_reset_time, min_reset_time}`. `update_channel(name,v,now)`: last=v; for max: if max_reset_time set and now>=it -> max=v, clear timer; else max=(max exists? fmax(max,v) : v); same for min; last_update_time=now. `accumulate(name,v)`: acc+=v, return acc. `smooth_basic(name,v,f)`: f clamped [0,1]; first call -> v; else prev*f + v*(1-f). `set_max_reset_timer(name,now,secs)` -> max_reset_time = now+secs (same for min). `reset()` clears all.

### 1.9 Utilities
- `identifiers(expr_text)`: set of every run of `[alnum_]` that starts with alpha or `_` (functions/keywords included; numeric-leading runs dropped). Used for dependency checks.
```c
size_t lt_expr_identifiers(const char *s, char ***names); /* unique */
```
- `evaluate_display_string(raw, ctx, strctx)`: trim raw; if len>=2 and starts `{` and ends `}`: inner = trim(inside); if inner empty -> return raw; parse+eval; on any error return raw unchanged; else `to_display_string(value)`. Else return raw. (Used for gauge titles, axis labels, units, command-button/indicator labels.) Note it is NOT used when `{` appears mid-string; the whole label must be one braced expr.
- Visibility/enable evaluation (app layer `evaluate_visibility`): condition None -> true; parse error or eval error -> **true** (visible/enabled by default); else `as_bool(result)`. Menu items get `visible`/`enabled` flags recomputed from their `*_condition` strings on each menu fetch (recursively into SubMenu); Separator untouched. Dialog fields/panels/checkboxes: same function; Field "enable" condition false => render disabled (not hidden); visibility false => hidden.

### 1.10 How variables resolve (context construction)
Context `name -> double` is assembled by the app, in this layering (later overrides earlier): (1) tune constants/pcVariables: Scalar -> value; Bool -> 1/0; Array -> element[0]; Option-string (bits constant stored as its label) -> index of label in constant's `bit_options` (exact, then ASCII case-insensitive; omitted if not found). INI `[Defaults] defaultValue` supplies values for constants absent from the tune. (2) latest realtime output-channel values (section 3). (3) computed channels folded in (3.4). Names are case-sensitive. Only identifiers actually referenced (via `identifiers()`) need be materialised. Unknown names read 0. `bitStringValue(const, idxexpr)` needs the constant's `bit_options` strings (from [Constants] `bits` option lists); `stringValue(c)` the tune's string constant value.
Dynamic constants fields: `scale`/`translate`/`min`/`max` of a [Constants] entry written `{expr}` are resolved by evaluating expr with context = tune values + computed output channels (3.4) (`resolve_dynamic_scales`): result must be finite; Scale additionally must be != 0; update only if changed; range is trusted only once every declared min/max expr resolved.

## 2. [GaugeConfigurations]
Entry: `gaugeName = channel, "Title", "Units", lo, hi, loDanger, loWarn, hiWarn, hiDanger, digits[, extra ignored]`.
```c
typedef struct { char *name,*channel,*title,*units;
  double low_warning, low_danger, high_warning, high_danger, lo, hi; unsigned char digits;
  char *lo_expr,*hi_expr,*low_danger_expr,*low_warning_expr,*high_warning_expr,*high_danger_expr; } lt_gauge;
```
`parse_gauge_line(name,value)`: parts=split_ini_line; None if empty or parts[0] empty. channel = parts[0] stripped of `"`. Defaults: title "", units "", lo=0, hi=100, low_danger=0, low_warning=0, high_warning=100, high_danger=100, digits 0 (note `new()` default field order differs from line order!). Line positions: [1] title, [2] units (both `"`-trimmed; a braced title like `{ bitStringValue(a, b) }` stays verbatim incl. braces), [3] lo, [4] hi, [5] low_danger, [6] low_warning, [7] high_warning, [8] high_danger, [9] digits (u8 parse, failure -> 0). Numeric fields via `parse_num_or_expr(raw, fallback)`: `strtod`-whole-string success -> value, no expr; else if trimmed raw is `{...}` -> value=fallback (lo 0, hi 100, loD 0, loW 0, hiW 100, hiD 100) and `*_expr` = inner trimmed; else fallback, no expr. Callers must resolve exprs against tune values (otherwise tach pegs at 100).
Stored in map keyed by gauge name (later duplicate replaces).
Zones: `is_danger(v) = v <= low_danger || v >= high_danger`; `is_warning(v) = (v <= low_warning && v > low_danger) || (v >= high_warning && v < high_danger)`; `is_normal(v) = v > low_warning && v < high_warning`.
Tests: line `rpm, "Engine Speed", "RPM", 0, 8000, 300, 600, 6500, 7000, 0` -> hi 8000, title "Engine Speed". Line `rpm, "Engine Speed", "RPM", 0, {rpmhigh}, 300, 600, {rpmwarn}, {rpmdang}, 0, 0` -> hi_expr "rpmhigh", high_warning_expr "rpmwarn", high_danger_expr "rpmdang", lo_expr NULL, low_danger 300, lo 0. rpm gauge with loD 500, loW 800, hiW 6500, hiD 7000: danger(400), danger(7500), warning(600), warning(6800), normal(3000). `gppwmOutput1, { bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }, "%", 0, 100, 0, 0, 100, 100, 1` -> title is the braced text, units "%", digits 1; `evaluate_display_string(title)` with bit_options RPM,MAP,TPS and gppwm1_loadAxis=1 -> "MAP"; with `{ stringValue(gpPwmNote1) }` and string ctx returning "Radiator Fan" -> "Radiator Fan"; with no ctx -> "".

## 3. [OutputChannels], realtime frame decode

### 3.1 Struct
```c
typedef enum { LT_U08,LT_S08,LT_U16,LT_S16,LT_U32,LT_S32,LT_F32,LT_F64,LT_STRING,LT_BITS } lt_dtype;
typedef enum { LT_BIG, LT_LITTLE } lt_endian;
typedef struct { char *name,*label; lt_dtype type; unsigned short offset;
  int has_bitpos; unsigned char bit_position; int has_bitcount; unsigned char bit_count;
  lt_dtype bit_storage;           /* backing word type for bits; default U08 */
  char *units; double scale, translate; char *expression; lt_expr *cached; } lt_channel;
```
Type tokens (case-insens, `from_ini_str`): U08/UINT8/BYTE/BU08; S08/INT8/SBYTE/BS08; U16/UINT16/WORD/BU16; S16/INT16/SWORD/BS16; U32/UINT32/DWORD/BU32; S32/INT32/SDWORD/BS32; F32/FLOAT/BF32; F64/DOUBLE/BF64; ASCII/STRING -> String; BITS -> Bits. Sizes: 1,1,2,2,4,4,4,8, String 0, Bits 1. (`B*` big-endian per-field override exists in constants parsing but is NOT applied to output channels.) Block endianness (`def.endianness`) comes from [MegaTune]/protocol section (spec 01); `EcuDefinition` field default in tests = Little set explicitly; type default is Big.
`read_from_bytes(data,len,offset,endian)`: fails (None) if `offset + size > len`; U08/Bits -> byte; S08 -> (signed char); U16/S16/U32/S32 per endian; F32 -> float widened to double; F64; String -> None. Returns double.

### 3.2 Line grammar `name = <spec>`
Key = channel name (as written, case-preserved, map key). Special keys (case-insens) `ochBlockSize` (value up to `;`, parse int, fail->0) and `ochGetCommand` (value up to `;`, `"` stripped) set protocol fields and create no channel.
1. Value trimmed starts with `{`: computed. expression = text between first `{` and FIRST `}` (so expression cannot contain `}`), trimmed. rest after `}` trimmed, leading `,` trimmed, then first comma-field (trim, strip `"`) = units. No `}` -> line ignored. type F32, offset 0, scale 1, translate 0, bit_storage U08. Expression is parsed once (`cache_expression`); a parse failure leaves `cached` NULL and the evaluator reparses on each call (fails every time -> channel never gets a value).
2. Else split on plain `,` (trim each; NOT quote aware) -> parts; need >= 2. First part lower-cased `bits` => bits form; `scalar` => scalar form; either prefix is removed (parts need still >= 2). parts[0] = type token (unknown -> line ignored), parts[1] = offset parsed as u16 (decimal only; fail -> ignored).
   - Bits form: `name = bits, U08, 84, [5:7]`. bit_storage = type; channel.type = Bits. parts[2], if present and starts `[`: strip `[` `]` chars, split `:`; bit_position = trim(first) parse u8; bit_count: if end part exists and parses u8 and end >= start -> end-start+1 (inclusive); if end absent -> 1; else None. If parts[2] does not start `[` it is taken as units (stripped `"`). If no parts[2], no bit_position (then decoded like a plain byte read).
   - Scalar form (also bare `U16, 0, "RPM", 1.0, 0.0`): parts[2] units (`"` stripped), parts[3] scale (parse f64, fail->1.0), parts[4] translate (fail->0.0). Display = raw*scale + translate. Scale/translate given as `{expr}` are NOT supported for channels (become 1.0/0.0).
Channel stored by key; later duplicates replace.
`size_bytes()`: computed -> 0; Bits -> bit_storage size; else type size.

### 3.3 Decoding one raw channel
`parse(data,len,endian)`: computed -> None. Bits with bit_position: `raw = read_from_bytes(bit_storage @ offset)` (None if short); `count = clamp(bit_count or 1, 1, 64)`; `mask = count>=64 ? ~0ULL : (1<<count)-1`; `word = (uint64)(int64)raw` (two's complement keep: S08 0xC5 -> negative -> sign-extended, which is fine since masked); `bit_val = (pos >= 64 ? 0 : word >> pos) & mask`; value = bit_val*scale + translate. Else plain read; value = raw*scale+translate. Bits without bit_position decode as byte (U08 path via Bits=1 byte; no scale special).
Tests (all Little endian): `nSquirts = bits, U08, 84, [5:7]` -> pos 5 count 3; byte 0b0100_0000 -> 2; 0b1110_0000 -> 7. `errorNum = bits,U08,75,[0:1]` & `currentError = bits,U08,75,[2:7]`(count 6): byte (42<<2)|3 -> 3 and 42. `afr_type = bits,S08,753,[0:2]` & `launchMode = bits,S08,753,[6:7]`: byte 0b1100_0101 -> 5 and 3. `hasSync = bits,U08,31,[7:7]`: 0x80 -> 1, 0 -> 0. `crank = bits, U32, 852, [11:11]`: pos 11 count 1 storage U32; moved to offset 0, block {00,08,00,00} little -> 1.0. `flag = bits, U32, 1, [8:8]`: size_bytes 4, parse of 2-byte block -> None. `run = bits,U08,0,[1:1]` block {0x02} -> 1.0. `rpm = U16, 0, "RPM", 1.0, 0.0` -> type U16, offset 0, units RPM. `afr = { ego1 / 10.0 }, "AFR"` -> computed, expression `ego1 / 10.0`, units AFR.

### 3.4 Frame -> values (`Evaluator::process`, realtime/evaluator.rs)
```c
typedef struct { char **computed_names; size_t n; } lt_evaluator;
lt_evaluator *lt_evaluator_new(const lt_ecudef*);             /* names of computed channels, SORTED ascending (byte order) */
lt_map_f64   *lt_evaluator_process(const lt_evaluator*, const unsigned char *raw, size_t len, const lt_ecudef*);
```
1. values = {}. For every NON-computed channel: if `parse` succeeds insert (name -> value). Short frames silently omit channels whose bytes are out of range.
2. Up to 3 passes over computed names in sorted order: for each, evaluate expression with ctx = `values` (no StringContext; function `timeNow`/`table` etc. return defaults): success -> `v = result.as_f64()`; update (insert) if name absent OR `|old - v| > 1e-7`; count changes. Evaluation error (unknown function, etc.) -> skipped (no value). If a pass has 0 changes stop. Note a computed value may be NaN/inf; Rust realtime stream sanitises non-finite to 0 before emit (do the same for display); `fold_computed_output_channels` (used for dynamic scales) uses 3 unconditional passes, rewrites only if finite, includes constants in ctx, evaluates over HashMap order (non-deterministic; ok).
3. Return map. No frame header/CRC handled here (protocol layer already stripped it). Frame length = `ochBlockSize`.
Tests: raw channel rpm U16@0 little, computed `rpm_x2 = rpm * 2`, `rpm_x4 = rpm_x2 * 2`; raw {E8 03 00 00} -> rpm 1000, rpm_x2 2000, rpm_x4 4000 (sorted order rpm_x2 < rpm_x4 but dependency chain also resolved if reverse). Computed `status = flag > 0 ? 100 : 0` with flag U08@0: {01}->100, {00}->0.

## 4. [Datalog]
Section keys are all treated alike regardless of key text (`entry = ...`). `parts = split_ini_line(value)`; empty -> skip. Fields (each: trim, strip `"`, empty -> missing):
```c
typedef struct { char *channel,*label,*data_type,*format,*condition; int enabled; } lt_datalog_entry;
```
channel = parts[0] (unquoted, not empty-filtered); label = [1] or ""; data_type = [2] or "float"; format = [3] or "%.2f"; condition = [4] only if it is `{...}`: inner trimmed (else NULL). enabled = true always. Appended to ordered vector. Format `onOff` etc. are bit-option names, not printf.
Tests: `rpm, "RPM", int, "%d"` -> int,%d,no cond. `time, "Time", float, "%.3f"`. `stagingActive, "Fuel Staging", int, "onOff", { stagingEnabled }` -> format onOff, condition `stagingEnabled`. `boostDuty, "Boost Duty", int, "%d", { bitStringValue(boostLabels, boostType) }` -> condition `bitStringValue(boostLabels, boostType)`. `afr, "AFR"` -> float, %.2f.
[DatalogViews] (`key = "Label", ch1, ch2...`; needs >=2 parts; channels = parts[1..] trimmed) in the same module family: `lt_datalog_view {name,label,channels[]}`.

## 5. [LoggerDefinition]
Two coexisting parses of the same raw lines:
### 5.1 Legacy flat (per line, `key = "Label", sampleRate, ch1, ch2, ... [, {enable}]`)
Only when split_ini_line gives >= 3 parts: name=key, label=parts[0] unquoted, sample_rate = parse f64 else 100.0, channels = parts[2..] trimmed up to first braced part which becomes enable_condition (unbraced) and stops. Struct `lt_logger_def{name,label,sample_rate,channels[],enable_condition}` in map by name. (Real INIs' `loggerDef = tooth, "Tooth Logger", tooth` has 3 parts so this ALSO fires with name "loggerDef" - harmless junk; indented block keys with <3 parts are skipped; `recordField = a,b,c,d,e,f` lines also produce junk entries keyed `recordField` - ignore/skip these in the C port: only use 5.2.)
### 5.2 Diagnostic loggers (block form; authoritative)
```c
typedef struct { char *name,*label; unsigned start_bit, bit_count; double scale; char *units; } lt_logger_field;
typedef struct { char *name,*label,*kind, *start_command,*stop_command,*data_read_command;
  unsigned long data_read_timeout_ms; int continuous_read; char *data_ready_condition;
  size_t header_len, footer_len, record_len; lt_logger_field *fields; size_t nfields; } lt_diag_logger;
size_t lt_parse_logger_definitions(const char **lines, size_t n, lt_diag_logger **out);
int    lt_logger_decode(const lt_diag_logger*, const unsigned char *rec, size_t len, double *vals /*nfields*/, int *present);
int    lt_logger_is_empty_record(const lt_diag_logger*, const unsigned char*rec, size_t len);
size_t lt_logger_record_count(const lt_diag_logger*, size_t payload_len);
```
Input = raw section lines (already stripped of comments with the 0.4 rule i.e. `;` comments after `=`; the parser keeps `line` post-strip). For each line: strip_comment (simple: cut at first `;` - note module's own strip_comment cuts at the FIRST `;` anywhere, quote-unaware), trim, skip empty, split at first `=`, key/value trimmed. Keys case-insens.
- `loggerDef`: value split on plain `,` trimmed: name=[0], label=[1] unquoted, kind=[2] (missing -> ""); new logger with data_read_timeout_ms=5000, everything else zero/empty. Subsequent keys apply to the LAST logger (ignored if none).
- `startCommand`, `stopCommand`, `dataReadCommand`: value with all leading/trailing `"` stripped. Raw: still contains `\xNN` escapes (4 chars) and `$tsCanId` names; resolved later by `substitute_variables` (replace `$name`/`\$name` with the pcVariable byte as a char, keep literal if unknown) then `parse_escape_sequence`: `\xNN`/`\XNN` (needs i+3 < len i.e. two hex digits present AND at least one more char after... exact rule: `'x'|'X' if i+3 < chars.len()`; so `\x01` at the very end of string is NOT decoded as hex - quirk: falls to default arm pushing `\` then literal chars) -> byte; `\0` -> 0; `\n`; `\r`; `\\` -> `\`; any other `\c` -> push `\` (advance 1); other chars pushed as byte.
- `dataReadTimeout`: unsigned parse of unquoted value else unchanged (5000).
- `continuousRead`: case-insens equals "true" (only that word).
- `dataReadyCondition`: unquoted, strip all `{`/`}` chars, trim -> expression text (Some).
- `recordDef = header, footer, record`: split `,`, keep only parts that parse as usize (unparseable dropped), if >= 3 take [0],[1],[2].
- `recordField = name, "Label", startBit, bitCount, scale[, "units"]`: split `,` trimmed; need >= 5; start_bit parse u32 (fail 0), bit_count u32 (fail 0), scale f64 (fail 1.0), units = [5] unquoted or "".
- `dataLength` and others ignored.
`read_bits(rec,len,start_bit,count)`: None if count==0 || count>64 || start+count > len*8. Record is a BIG-ENDIAN integer: bit 0 = LSB of the LAST byte. For i in [0,count): `bit=start+i; byte=rec[len-1-bit/8]; if (byte >> (bit%8)) & 1: out |= 1<<i`.
`decode`: for each field with Some(raw): value = (double)raw * scale (fields that cannot be read are omitted). `is_empty_record` = every decoded value == 0.0 (vacuously true if none decoded). `record_count(payload_len) = record_len==0 ? 0 : (payload_len - header - footer, saturating) / record_len`.
Tests (verbatim INI block in diagnostic_logger.rs tests: tooth + compositeLogger): 2 loggers; tooth start "H", stop "h", read cmd starts with `T`, continuousRead true; commented `;dataReadCommand = "r..."` is ignored; recordDef (0,0,4) / (0,0,5); `record_count(508)==127` tooth, `record_count(635)==127` composite. Tooth `[00 00 04 94]` -> toothTime 1172 (also 0x90->1168, `00 00 09 20`->2336). Composite `[00 01 ED 28 19]` -> priLevel 1, secLevel 0, sync 1 (field bits 0,1,4 of last byte 0x19=0b11001: bit0=1,bit1=0,bit4=1), refTime(bits 8..39, scale 0.001) 126.248; `[00 01 EE F4 10]` -> priLevel 0, sync 1, refTime 126.708. Empty: tooth `00 00 00 00` empty; `45 23 01 00` not; composite `00 00 00 00 00` empty; `00 E8 03 00 00` not (refTime nonzero). `decode(01 02)` for tooth -> no fields.

## 6. [Menu]
```c
typedef enum { LT_MI_DIALOG, LT_MI_TABLE, LT_MI_SUBMENU, LT_MI_STD, LT_MI_HELP, LT_MI_SEPARATOR } lt_mi_type;
typedef struct lt_menu_item { lt_mi_type type; char *label,*target; char *vis_cond,*en_cond; int visible,enabled; struct lt_menu_item *items; size_t nitems; } lt_menu_item;
typedef struct { char *name,*title; lt_menu_item *items; size_t nitems; } lt_menu;
```
Keys (case-insens):
- `menu = "Title"`: parts=split_ini_line; name = title = parts[0] without `"`. Push new menu (ordered). Items below attach to the LAST menu; if no menu exists, items are dropped.
- `subMenu` / `groupChildMenu = target, "Label"[, {A}[, {B}]]` (three accepted layouts):
  1. If parts[0] contains `"`: space-separated `target "Label"`: target = text before first quote trimmed; label = text between first quote and next quote (or rest, `"`-trimmed). Conditions from parts[1..].
  2. Else if len >= 2: target=parts[0] unquoted; label=parts[1] unquoted; Conditions from parts[2..].
  3. Else only target: label = target = parts[0].
  `cond(s)` = strip all `{}` chars, trim; None if empty or exactly "0" or "1". Conditions: an unbraced number first (old index placeholder, `"VE", 0, {cond}`) is skipped; one condition => ENABLE condition; two => enable first, visibility second (TunerStudio ECU definition reference, "Visibility and enablement" of menus; same order as dialog fields). Empty target -> skip entry.
  Type: target == `std_separator` -> Separator; starts with `std_` -> Std; target is a known help topic -> Help; target is a known table (by table `name`) or curve -> Table; else Dialog. (Help/table/curve may be defined later in the file: `post_process_menu_items` re-types every `Dialog` item recursively after the whole file is parsed: if target in help topics -> Help; else if target is table name OR table `map_name` OR curve name -> Table. Note: `visible`/`enabled` reset to true.) Only Dialog items are re-typed.
  Placement: `groupChildMenu` appends to the last item of the current menu iff it is a SubMenu (children go in `items`), else appends to menu itself. `subMenu` always appends to the menu's top-level item list.
- `groupMenu = "Label"[, {A}[, {B}]]`: SubMenu with label=parts[0] unquoted; en=cond(A), vis=cond(B) as above; empty items. Appended to the last menu.
- `std_separator` as a KEY (no value needed, e.g. `std_separator =`): append Separator to current menu.
Runtime: evaluate `visible`/`enabled` per 1.9 with the full context. Special std targets: `std_realtime` (dashboard) etc. are handled by the UI (host-defined), `std_injection`, `std_ms3Rtc` are synthesized from constants (UI-level; skip).
Hierarchy depth: SubMenu children are leaf items only (no nested groupMenu in children via this parser).

## 7. [UserDefined] (alias [UiDialogs])
State: `current_dialog`, `current_indicator_panel`, `current_readout_panel`, `current_help` (each Option<name>). Keys case-insens. Dialogs/panels stored in name-keyed maps (replace on redefinition), components in ordered Vec. Lines with an unknown key are ignored.
```c
typedef struct { char *name,*title,*layout_hint,*topic_help; lt_dlg_comp *comps; size_t ncomps; } lt_dialog;
typedef enum { C_LABEL,C_PANEL,C_FIELD,C_CHECKBOX,C_SLIDER,C_DISPLAYONLY,C_SETTINGSEL,C_RUNTIMEVALUE,C_LIVEGRAPH,C_TABLE,C_INDICATOR,C_CMDBUTTON,C_GAUGE } lt_comp_type;
```
Component fields (shared names): `label`, `name`, `position`, `vis_cond`, `en_cond`, plus per-type extras below.

- `help = name, "Title"`: name = parts[0] (NOT unquoted/trimmed beyond split), title = parts[1] unquoted or name. Insert help topic {name,title,web_url=NULL,text_lines[]}; current_help=name. `webHelp = "url"` sets web_url (trim, strip `"`) on current_help. `text = "line"` appends (trim, strip `"`) to current_help text_lines. (current_help is never cleared.)
- `dialog = name, "Title"[, layoutHint][, {cond}]`: name=parts[0]; title=parts[1] unquoted else name; layout_hint = first part at index >= 2 not starting `{` (trimmed) else NULL (`xAxis` only value acted on: lay out top-level unpositioned panels in a row; `border` captured). Creates dialog; current_dialog=name; clears current indicator/readout panel.
- `topicHelp = "url"`: sets dialog.topic_help (trim, strip `"`) on current_dialog, overwriting (note: "first only" in doc but code overwrites each time).
- `indicatorPanel = name[, columns][, {vis}]`: columns = parts[1] parsed u8 else **2**; vis = first part at index>=2 that is braced -> unbrace WITHOUT inner trim (strip `{}` chars only; leading/trailing spaces inside braces remain). Creates panel; current_indicator_panel=name; clears readout panel AND current_dialog.
- `readoutPanel = name[, columns][, {vis}]`: columns = first part at index>=1 that does not start `{` and parses as u8, else **1**; vis = first braced part at index>=1 (unbraced, trimmed). Clears indicator panel and current_dialog. 
- `readout = channel, "Title", "Units", ..., digits, precision` (needs current readout panel, >= 2 parts): channel=[0] trimmed; title=[1] unquoted; units=[2] unquoted or ""; digits = u8 parse of parts[len-2] else 0; precision = u8 parse of last part else 0. (With only 2 parts, parts.get(len-2)=parts[0] parse fails->0.)
- `indicator = {expr}, "off", "on"[, offFg, offBg, onFg, onBg]` (needs >=3 parts): if current_indicator_panel is set -> push `lt_indicator{expression=unbrace(parts[0]) (no inner trim), label_off,label_on=unquoted (a braced label stays braced -> dynamic text via evaluate_display_string), color_off_fg=[3], color_off_bg=[4], color_on_fg=[5], color_on_bg=[6] (trimmed; NULL if absent)}`. Else if current_dialog set -> push Indicator component {expression,label_off,label_on}. NOTE color order is fg,bg,fg,bg in [UserDefined] (default colors white/black/red/black) but in [FrontPage] it is bg_off, fg_off, bg_on, fg_on (see 9).
- `panel = name[, position][, {A}][, {B}]` (needs current dialog): panel name = parts[0] trim; position = first part at index>=1 not starting `{`; brace parts (index>=1, braced, unbraced+trimmed) collected: 2+ -> enable=bp[0], visibility=bp[1]; 1 -> enable=bp[0], visibility NULL; each trivial ("", "0","1") -> NULL. NOTE: panels, fields and menus all read enable first, visibility second. Single brace on a panel = enable expression; app hides the panel when it is false.
- `liveGraph = name, "Title"[, position]` (needs >=2 parts, current dialog): component LiveGraph{name, title unquoted, position = parts[2] or "South", channels[]}; `graphLine = channel` appends trimmed value to the LAST component only if it is a LiveGraph.
- `runtimeValue = "Label", channel[, {vis}]`: needs >= 2 parts; vis = first braced part from index 2, unbraced+trimmed. Component RuntimeValue.
- `field = "Label", constant[, {A}[, {B}]]`: >=2 parts -> Field: label unquoted, name=parts[1] trimmed; conditions = all braced parts from index 2, each unbraced WITHOUT inner trim; if >= 2: en=c[0], vis=c[1] (TunerStudio order, same as panels; Speeduino's "Maximum lambda" / "Maximum lambda deviation" pair depends on it); if exactly 1: en=c[0] only (hidden never; greyed). parts[1] empty or braced (`field = "!Note", {}, {}, { cond }`) -> Label with those conditions: `{}`/empty holds the constant's place, a non-empty brace there is the first condition. If only ONE part (`field = "Some text"`) -> Label component text=unquoted. Zero parts -> nothing.
- `checkbox = "Label", name[, flipValue][, {A}[, {B}]]`: >= 2 parts; scan parts from index 2: braced -> condition (unbraced + trimmed), bare token equal (ASCII case-insens) `flipvalue` -> flip=true; same en/vis assignment as field. Semantics: unchecked/checked write 0/1; with flip inverted (checked <=> 0).
- `slider = "Label", name[, horizontal|vertical][, {A}[, {B}]]`: orientation = last matching lowercased token; other bare tokens ignored; conditions as checkbox.
- `displayOnlyField = [!|#]"Label"[, name][, {A}[, {B}]]`: label = parts[0] trimmed, `"`-stripped (prefix `!`/`#` kept inside label text only when outside quotes; UI interprets: `#` section header, `!` note). name = parts[1] trimmed unless empty, `blankfield`, or a braced part -> NULL. Conditions: braced parts from index 2 (if name present) else from index 1, unbraced+trimmed, EMPTY ones dropped; 2+ -> en=c0,vis=c1; 1 -> en=c0.
- `table = name` -> Table{name=value trimmed}.
- `settingSelector = "Label"[, {vis}]` -> SettingSelector{label,options[],vis = first part from idx1 starting `{` unbraced (no trim)}; `settingOption = "Label", const=val[, const=val...]` valid ONLY when the dialog's last component is a SettingSelector (else dropped): assignments = parts[1..] split at first `=`, name trimmed, value must parse f64 (else part dropped); push option {label, assignments}.
- `commandButton = "Label", commandName[, {enable}][, clickOnClose|clickOnCloseIfEnabled|clickOnCloseIfDisabled]` (>=2 parts): enable cond unbraced no trim (last braced wins); close behavior token case-insens (last wins). Command name refs [ControllerCommands]; button disabled when enable false; on dialog close sends the command per behavior (IfEnabled: when enable cond true; IfDisabled: when false; ClickOnClose: always).
- `gauge = gaugeName[, position]` -> Gauge{name, position (trimmed, NULL if empty)}.
Components other than the above require a current dialog; if none, the line is dropped. 
Tests: (a) `[UserDefined]` with `dialog = someOtherDialog, "Unrelated"` + `field = "Unrelated field", someConstant`, then `indicatorPanel = fuelClosedLoopIndicatorsPanel` (no columns) + two `indicator =` lines -> panel registered, columns 2, 2 indicators, second label_on "Tuning Detected" (line: `indicator = { isTuningNow }, "No tuning happening", "Tuning Detected", white, black, green, black`), earlier dialog still has exactly 1 component. First indicator there has braced labels `{ Correction using bitStringValue(...) region }`. (b) `dialog = fuelClosedLoopDialog, "Short term fuel trim/Closed loop", xAxis` + `panel = fuelClosedLoopBank1` -> layout_hint "xAxis"; `dialog = plainDialog, "No hint"` -> hint NULL.

## 8. [TableEditor] / [CurveEditor]
Section name match is substring (`contains`) so `[TableEditor]` and variants work. Entries mutate the "current" object set by the last `table =`/`curve =`.
```c
typedef struct { char *name,*map_name,*title,*map,*x_bins,*x_out,*y_bins,*y_out,*help,*x_label,*y_label,*up_label,*down_label;
  int is3d; int page; size_t x_size,y_size; int has_grid_height; float grid_height; int has_orient; float orient[3];
  char *rows_size_const,*cols_size_const; size_t max_elements; int role; } lt_table;
```
- `table = tableName, mapName, "Title"[, page]`: plain `,` split+trim; need >= 3 parts. name=[0], map_name=[1], title=[2] unquoted, page=[3] parse u8 else 0. Defaults: 2D, x_size 0, y_size 1. Registers reverse map mapName->name. current_table=name. (Menus reference by map_name or name.)
- `xBins = constName[, outputChannel]`: x_bins=[0], x_out=[1]. `yBins = constName[, outputChannel]`: y_bins=[0], y_out=[1], table becomes 3D (a table with no yBins is 2D). `zBins = constName` -> `map` (trimmed whole value). `gridHeight = f32`. `topicHelp = "..."` (trim `"`). `xyLabels = "X","Y"` (split_ini_line; `"`-stripped; braced expr kept verbatim -> evaluate_display_string at render). `gridOrient = yaw, pitch, roll` (f32 x3 all must parse). `upDownLabel = "(RICHER)", "(LEANER)"` -> up_label, down_label (split_ini_line).
- Post-process `table_sizes` (after whole file; dynamic shapes first): for each table: x_size_from_bins = shape.x_size() of constant x_bins (0 if unknown); y_size_from_bins = for yBins present: shape.x_size() of y_bins constant else 1; no yBins -> 1. If map constant has dynamic size (`{cols}x{rows}` with defaultValue for both counts > 0): (cols,rows) = defaults. Else if map constant 2D: (cols, rows) of its shape; if 1D: x = x_from_bins if > 0 else its length, y=1; if scalar: (x_from_bins, y_from_bins); no map constant: (x_from_bins, y_from_bins). Assign `x_size` only if currently 0 and final_x > 0; `y_size` only if currently <= 1 and final_y > 1. If map constant has dynamic size, record rows_size_const/cols_size_const (names of the rows/cols count constants) and `max_elements` from `maximumElements`.
- `post_process_dynamic_shapes` (constants with `{count}` or `[{cols}x{rows}]` dims): 2D: def_cols,def_rows = round(defaultValue); max_el = maximumElements[name] else cols*rows; (rows,cols) = (def_rows,def_cols) if both > 0 and product == max_el; else if def_cols>0 && max_el>0 && max_el%def_cols==0 -> (max_el/def_cols, def_cols); else if max_el>0 -> (max_el,1); else (max(def_rows,1), max(def_cols,1)). 1D: allocated = round(max of the count constant) if >0 else round(defaultValue of count) ; at least 1.
- `curve = curveName, "Title"`: name=parts[0], title=parts[1] unquoted or "". current_curve. Per-key: `columnLabel = "X","Y"` (plain split; needs >=2; unquoted). `xAxis = min,max,step` / `yAxis = min,max,step` (3 f32, all must parse). `xBins = constName[, outputChannel]` -> x_bins, x_out. `yBins = constName[, ...]` -> y_bins = parts[0] (split_ini_line trimmed; extra ignored). `size = n` (usize parse). `topicHelp = "..."`. `gauge = GaugeName` (trim). `lineLabel = "text"` appends (repeatable, trim, strip `"`) to line_labels. Page for curves defaults 0 (not parsed from the curve line). Curves live in a map by name; curve `y_bins` may also refer to two lines if overlaid.
Role (`infer_table_roles`, other spec): VE/AFR/ignition/WUE role assignments from [VeAnalyze]/[WueAnalyze]; numbered siblings inherit.
Tests: `new_2d("cranking","crankingTable","crankingBins",10)`: not 3D, cell_count 10; `new_3d("veTable1","veTable1Map","rpmBins","fuelLoadBins",16,16)`: 3D, cell_count 256.

## 9. [FrontPage]
```c
typedef struct { char *expression,*label_off,*label_on,*bg_off,*fg_off,*bg_on,*fg_on; } lt_fp_indicator;
typedef struct { char **gauges; size_t ngauges; lt_fp_indicator *inds; size_t ninds; } lt_frontpage;
```
Created (empty) on first key in the section. `gaugeN = gaugeName` (key case-insens prefix `gauge`; N parsed as usize from the rest of key; 0 or > 64 -> ignored with warning; vector grown to N with empty strings; gauges[N-1] = trimmed value). Non-numeric suffix ignored. `indicator = { expr }, "off", "on"|{ dynamic }, c1, c2, c3, c4`: value must start with `{`; find matching `}` by brace-depth count (nested ok); expression = inner trimmed; remaining after it: skip one `,`, trim; split with `split_frontpage_parts` (like split_ini_line but quote-state only toggles when brace depth == 0 and commas split only at depth 0, not in quotes; the final piece pushed only if non-empty after trim); need >= 6 parts else indicator dropped. Assignments: label_off=p0 (`"` stripped), label_on=p1 (`"` stripped; a braced dynamic label stays braced), **bg_off=p2, fg_off=p3, bg_on=p4, fg_on=p5** (trimmed).
Colors -> CSS/RGB (case-insens): white #ffffff, black #000000, red #ff0000, green #00ff00, blue #0000ff, yellow #ffff00, orange #ffa500, cyan #00ffff, magenta #ff00ff, gray/grey #808080, lightgray/lightgrey #d3d3d3, darkgray/darkgrey #a9a9a9, lime #00ff00, maroon #800000, navy #000080, olive #808000, purple #800080, silver #c0c0c0, teal #008080; strings starting `#` or unknown names returned (lower-cased) as-is. In C the renderer needs a name->RGB table plus `#rrggbb`/`#rgb` parser; unknown -> fall back to white fg / black bg.
Tests: INI with gauge1..gauge8 (tachometer, throttleGauge, mapGauge, cltGauge, afrGauge, advanceGauge, batteryGauge, iatGauge) -> 8 gauges, [0]=tachometer, [7]=iatGauge. `indicator = { running }, "Not Running", "Running", white, black, green, black` -> expr `running`, off "Not Running", on "Running", bg_off white, fg_off black, bg_on green, fg_on black. `indicator = { (tps > tpsflood) && (rpm < crankRPM) }, "FLOOD OFF", "FLOOD CLEAR", white, black, red, black` -> expr `(tps > tpsflood) && (rpm < crankRPM)`, bg_on red. `gauge1 = accelGauge` + `gauge4000000000 = bogusGauge` -> gauges.len <= 64 and [0]=="accelGauge".
Runtime: indicator on when `as_bool(eval(expr))`; if label (on or off) is a `{...}` string use evaluate_display_string.

## 10. Other table-driven per-line helpers touched
- `[ControllerCommands]` `name = "raw"|refName,... [, {enable}]`: parts=split_ini_line; braced part -> enable_condition; quoted -> Raw(unquoted); other non-empty -> Reference; if no parts produced use unquoted parts[0] as Raw. label=name. Raw strings carry `\xNN` escapes (decode as in 5.2). Used by commandButton.
- `[PortEditor]` `name = "Label"[, {enable}]`: label=parts[0] unquoted; enable = first braced part from idx1 (unbraced, trimmed).
- `[PcVariables]`: handled by constants spec; `defaultValue = const, value` ([Defaults]) -> `default_values[const]=value` (f64). `$name` byte substitution uses default, else constant `min`, clamped 0..255, else 0; re-resolved after file end.

## 11. Suggested C API summary
```c
lt_expr *lt_expr_parse(const char*, char*err, size_t); void lt_expr_free(lt_expr*);
int  lt_expr_eval(const lt_expr*, const lt_ctx*, const lt_strctx*, lt_value*, char*err, size_t);
int  lt_eval_bool(const char *cond_or_null, const lt_ctx*, const lt_strctx*);       /* NULL->1, error->1 */
char*lt_display_string(const char *raw, const lt_ctx*, const lt_strctx*);           /* caller frees */
int  lt_gauge_parse(const char *name,const char *value, lt_gauge*);
int  lt_channel_parse_line(const char *name,const char *value, lt_channel*);
int  lt_channel_decode(const lt_channel*, const unsigned char*, size_t, lt_endian, double*);
void lt_realtime_process(const lt_evaluator*, const unsigned char*, size_t, const lt_ecudef*, lt_ctx *out);
```

## 12. Test-porting checklist (all derived from Rust tests)
Expression: `1 + 2 * 3`=7; `true && false || 1 == 1`=true; `rpm > 1000` (1500 true, 500 false); `(flags & 4) == 4` (5 true, 3 false); function coverage table: abs(-3)=3, round(2.7)=3, floor(2.7)=2, ceil(2.1)=3, sqrt(16)=4, min(3,5,1)=1, max(3,5,1)=5, pow(2,10)=1024, recip(4)=0.25, if(1,10,20)=10, int(3.9)=3, trunc(-2.8)=-2, not(0)=true, boolean(1)=true, bool(0)=false, isNaN(sqrt(0-1))=true, isAdvancedMathAvailable()=true, pastValue(time,1)=1.5 (time=1.5), getAuxDigital(0)=0; `time - pastValue(time, 1)` with time=42 -> 0; shifts `1 << 64`, `1 << 200`, `1 << (0 - 1)`, `1 >> 64`, `1 >> (0 - 3)` all 0, `1 << 3` = 8; 5000-deep nested parens -> parse error, 16 nested ok; `1 2`, `1 + 2 3`, `1 @ 2`, `a = 1` all parse errors; numbers `1e3`=1000, `1E3`, `1.5e-2`=0.015, `2e+2`=200, `.5`=0.5; `stringValue(gpPwmNote1)` -> "" without ctx, "Radiator Fan" with; `bitStringValue(pwmAxisLabels, gppwm1_loadAxis)` with idx 1 -> `INVALID[1]` without ctx, "MAP" with options [RPM,MAP,TPS]; `evaluate_display_string("Engine Speed")` returns unchanged. Extra to add: `5/0`=0, `5%0`=0, `-(-3)`, `~0`=-1, `"a"+"b"`="ab", `"a"=="a"`, `x ? 1 : 2` nested, unknown var = 0, unknown fn errors.
Fixtures: demo.ini (`libretune-app/src-tauri/resources/demo.ini`) gauge `gppwmGauge1` title contains `stringValue`; `tests/diagnostic_loggers_real_ini.rs` (see file) for the real-INI logger block.
