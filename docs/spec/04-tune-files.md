# 04 - Tune files (.msq, cache, diff, migration, .table, .tuneView)

Source: `libretune-core/src/tune/{file,cache,diff,migration}.rs`, `table_file.rs`, `tune_view/*`.
Out of scope (one line each): git versioning of tunes, SHA-256 INI hashing (the hash is only stored/compared as an opaque string here; computing it belongs to the INI subsystem), serde-JSON tune format (see 1.9, optional).

Conventions: all C identifiers `lt_` prefixed. Strings are NUL-terminated UTF-8 (or Latin-1 bytes; see 1.2). Numbers are `double`. "Page" is a `uint8_t` index.

---------------------------------------------------------------------
## 0. Minimal XML subset needed (write ONE small parser, used by all 3 formats)

Do NOT port the Rust string-search parsing (it has quirks, listed in 1.10). Write a tolerant pull/DOM parser for this subset and walk it semantically. Needed:

- Prolog: optional `<?xml ... ?>` (skip, ignore encoding attr; see 1.2 for bytes), optional `<!DOCTYPE ...>` (skip to matching `>`; the test files contain `<!DOCTYPE msq PUBLIC "-//rusEfi//DTD msq//EN" "msq.dtd">`, no internal subset `[...]` needed but skip balanced brackets anyway), comments `<!-- ... -->` (skip), processing instructions `<? ... ?>` (skip), CDATA `<![CDATA[ ... ]]>` (text, tune_view only; .msq values never use it).
- Elements: `<name attr="v" attr2='v'>`, `</name>`, `<name attr="v"/>` (self-closing; `<name />` with space also). Names: `[A-Za-z_:][A-Za-z0-9_.:-]*`. Namespace prefixes: for .tuneView strip everything up to and including the last `:` of element AND attribute names (local name). For .msq and .table, `xmlns` attributes are ignored and names are matched literally.
- Attributes: double- or single-quoted values; whitespace around `=` allowed; attribute order arbitrary. Values entity-decoded (below). Attribute value may contain `>` literally (a correct tokenizer must not end the tag at a `>` inside quotes).
- Text: character data between tags, entity-decoded. Whitespace preserved (caller trims where specified).
- Entities (decode in attributes and text): `&amp; &lt; &gt; &quot; &apos;`, `&#DD;` decimal, `&#xHH;` / `&#XHH;` hex numeric char refs (encode code point as UTF-8; see 1.2 on Latin-1). Rust `xml_unescape` rule: a `&` with no `;` within the next 10 chars is kept as a literal `&`; an unknown entity name (`&foo;`) or invalid code point is kept verbatim (`&foo;`). Implement exactly this leniency.
- Attribute value normalization (tune_view uses quick-xml `normalized_value`): `\t \n \r` inside attribute values become space on read (XML spec), and `&#10;`/`&#13;` decoded afterwards stay as real newlines. The tuneView writer relies on this (escapes `\n`->`&#10;`, `\r`->`&#13;` in attrs).
- Not needed: DTD entities, namespaces semantics, encodings other than UTF-8/Latin-1, mixed-content ordering beyond concatenated text.

Suggested API:
```c
typedef struct lt_xml_attr { char *name, *value; } lt_xml_attr;
typedef struct lt_xml_node {
  char *name;                      /* local name (prefix stripped only when asked) */
  lt_xml_attr *attrs; int nattrs;
  struct lt_xml_node **kids; int nkids; /* element children only, document order */
  char *text;                      /* concatenated direct character data (decoded) */
  struct lt_xml_node *parent;
} lt_xml_node;
lt_xml_node *lt_xml_parse(const char *buf, size_t len, int strip_ns, char *err, size_t errsz);
const char *lt_xml_attr_get(const lt_xml_node*, const char *name); /* NULL if absent */
void lt_xml_free(lt_xml_node*);
char *lt_xml_escape(const char*);        /* 1.7 */
char *lt_xml_unescape(const char*);
```
Error handling for the DOM parser: malformed XML (unclosed tag, mismatched close) -> for .table/.tuneView return error; for .msq be lenient: Rust never failed on malformed XML, it only failed with "Not a valid MSQ file" (1.5). Recommended: on mismatched/unterminated structure in .msq, keep what was parsed so far, then apply the validity rule.

---------------------------------------------------------------------
## 1. .msq tune file (`TuneFile`)

### 1.1 Data structures
```c
typedef enum { LT_TV_SCALAR, LT_TV_ARRAY, LT_TV_STRING, LT_TV_BOOL } lt_tv_kind;
typedef struct {
  lt_tv_kind kind;
  double scalar;            /* SCALAR */
  double *arr; size_t n;    /* ARRAY: flat, row-major as in text; n>=2 when parsed from text, may be any when set by API */
  char *str;                /* STRING (never NULL; "" for empty) */
  int boolean;              /* BOOL */
} lt_tune_value;

typedef struct { char *digits,*units,*rows,*cols; } lt_const_attrs; /* each NULL if absent; stored as raw strings, written back verbatim */

typedef struct { char *name,*data_type; uint8_t page; uint16_t offset; double scale, translate; } lt_manifest_entry;
  /* data_type strings: "U08","S08","U16","S16","U32","S32","F32","F64","String","Bits" (Rust Debug names of DataType) */

typedef struct { char *signature,*name,*hash,*spec_version,*saved_at; } lt_ini_metadata;

typedef struct { char *text,*author,*created,*modified; int tag; /* -1 none, 0 Info,1 Warning,2 Critical,3 Success,4 Todo */ } lt_annotation;

typedef struct lt_tune {
  char *version;        /* "1.1" default for new; also "1.1" for parsed (Default) */
  char *signature;      /* "" if unknown */
  char *author, *description, *created, *modified;   /* NULL = None */
  /* maps (string-keyed hash or sorted array; writer sorts keys, see 1.4) */
  map<uint8_t, byte[]> pages;                 /* raw page images */
  map<string, lt_tune_value> constants;
  map<string, uint8_t> constant_pages;        /* name -> page */
  map<string, lt_const_attrs> constant_attrs; /* shared by constants AND pcVariables (same namespace) */
  map<uint8_t, char*> page_sizes;             /* declared size text from <page number size> */
  char **ecu_settings; size_t n_ecu_settings; /* ordered list of <setting name> */
  map<string, lt_tune_value> pc_variables;
  lt_ini_metadata *ini_metadata;              /* NULL = None */
  lt_manifest_entry *manifest; size_t n_manifest; int has_manifest;
  map<string, lt_annotation> annotations;     /* key: "const" or "table:row:col" ; NOT written to .msq (JSON only) */
  char *firmware_info;                        /* NULL = None */
} lt_tune;
```
Timestamps are free-form strings. `lt_tune_new(sig)`: version "1.1", created=modified=now as RFC3339 (e.g. `2026-01-07T12:00:00+00:00`; C89: use `time()/gmtime/strftime("%Y-%m-%dT%H:%M:%S+00:00")`), everything else empty. Parsed tunes start from the *Default* (created/modified NULL, version "1.1").

### 1.2 Reading bytes / encoding
Read file as bytes. If bytes are valid UTF-8 use as is; otherwise treat each byte as a Latin-1 code point and transcode to UTF-8 (so internal strings are always UTF-8). Writer declares `encoding="ISO-8859-1"` but writes the UTF-8 bytes unchanged (known inconsistency; for ASCII content identical). Faithful C port: same behavior; optionally transcode non-ASCII to Latin-1 on write only if every code point < 256 (not done by the original).
Format dispatch by lowercase extension: `msq` -> MSQ parse; `json` -> JSON; otherwise try JSON then MSQ. On save: `msq` -> MSQ; `json` or no extension -> JSON; other ext -> error "Unknown format: <ext>". After load, run sanitize (1.6).

### 1.3 .msq reading semantics (implement on top of the DOM)
Root `<msq ...>` (xmlns `http://www.msefi.com/:msq`; any `version`/xmlns ignored). Elements, all optional, any order, matched by exact local name anywhere under root:

| element/attr | effect |
|---|---|
| `versionInfo@signature` | `signature`. Fallback if no `<versionInfo` element exists: first `signature="..."` attribute anywhere in the file (e.g. on `<msq>`). Note: a `<versionInfo>` lacking `signature` (only `firmwareInfo`) leaves signature "" (test files do this). `fileFormat`, `firmwareInfo`, `nPages` are NOT read back (firmware_info stays NULL). |
| `bibliography@author` | author |
| `bibliography@writeDate` | modified (free text, e.g. `Sun May 03 20:46:57 NZST 2026`) |
| `bibliography@created` | created |
| (no `<bibliography`) `msq@timestamp` fallback | modified (raw, not unescaped) |
| `iniMetadata@signature,name,hash,specVersion,savedAt` | ini_metadata (missing attrs -> ""); presence of element -> Some |
| `constantManifest/entry@name,type,page,offset,scale,translate` | manifest entry; page/offset parse failure -> 0; scale failure -> 1.0; translate failure -> 0.0; defaults page 0 offset 0 scale 1.0 translate 0.0; entries with empty name skipped; manifest set only if >=1 entry (else None). `type` is `data_type`. |
| `pageData@page` (uint8) | hex payload -> `pages[page]` |
| `page@number`, `page@size` | page structure / `page_sizes[number]=size-text` if both present and number parses as uint8 |
| `settings/setting@name` | appended to ecu_settings in order |
| `constant` | see below |
| `pcVariable` | see below |

pageData hex: trim; drop all whitespace; consume pairs of hex digits -> byte (`(hi<<4)|lo`); a pair where either char is not hex is skipped (no byte emitted); odd trailing char dropped; if resulting byte vector is empty the page is NOT inserted. Upper/lower case accepted. `page` attr must parse as 0..255 else element ignored. Later duplicates overwrite.
(Tests: `<pageData page="0">0102030405060708</pageData>` -> [1..8]; `page="1">aabbccdd` -> {aa,bb,cc,dd}.)

**Page walk**: iterate `<page>` elements in document order. `current_page` starts 0. For each `<page>`: if `number` attr parses as uint8 (0..255) then `current_page = number`; otherwise (e.g. `"-1"`, missing) `current_page` is left UNCHANGED (carries over from previous page, initially 0). Inside the page, first all `<constant>` children, then all `<pcVariable>` children (relative order between the two kinds is irrelevant).
  - `<constant name=N [digits units rows cols] />` or with text:
    - name = unescaped `name` attr; element skipped if no name attr.
    - attrs: `digits,units,rows,cols` unescaped; stored in `constant_attrs[N]` only if at least one present (an element with none does NOT erase a previous entry).
    - self-closing (`.../>`) -> value = String "" ; else value = `parse_value(trimmed text)`.
    - `constants[N] = value; constant_pages[N] = current_page`. Later duplicates overwrite.
  - `<pcVariable ...>`: same, but goes to `pc_variables[N]` (no page recorded). attrs go into the same `constant_attrs` map.
  - A `<constant>` located on page `-1` (as in pcVariableValues.msq) is therefore stored as a constant with `constant_pages = <previous page or 0>`.
- **Legacy fallback**: if the document has NO `<page>` element at all (or none with a closing tag), scan the whole file for `<constant>` elements (stored in constants with page 0) and, ONLY if no constant was found, `<pcVariable>` elements (-> pc_variables). (Rust: the fallback loop alternates; once a page-structured region is found it breaks.) Implementation: if no page elements -> take all `constant` elements anywhere with page 0; if that yielded 0, take all `pcVariable`s anywhere.
- **Validity**: after parse, if `constants` is empty AND `signature` is "" -> error `"Not a valid MSQ file"`. (A file with only pcVariables and no signature is invalid.)

**parse_value(text)** (text already trimmed of ASCII whitespace by the caller; this is the XML-decoded-NOT yet text - see note):
1. `trimmed` = trim(text). `was_quoted` = len>=2 and first char `"` and last char `"`. `unquoted` = trimmed without outer quotes if quoted else trimmed.
2. If quoted: if unquoted == "true"/"false" -> Bool; else -> String(xml_unescape(unquoted)). (Quoted numeric `"1"`, `"1.0"` MUST stay String: they are `bits` option labels.)
3. Unquoted:
   a. starts with `[` and ends with `]`: strip all leading `[` and trailing `]`; split on whitespace or `,` dropping empties; parse each as f64, DROPPING tokens that fail; >1 values -> Array; ==1 -> Scalar; 0 -> String(unquoted incl brackets).
   b. else contains a space or `\n`: tokens = split on whitespace/`,` (empties dropped); if ALL tokens parse as f64 and count>1 -> Array; if exactly 1 token and it parses -> Scalar; otherwise String(unquoted) (e.g. `Speed Density` unquoted -> String).
   c. else if parses as f64 (Rust `f64::from_str`: optional sign, digits, optional `.`, exponent, also `inf`, `infinity`, `nan` case-insensitive; in C use strtod but REJECT hex floats `0x..`, and require the whole token consumed) -> Scalar.
   d. else "true"/"false" -> Bool; else String(unquoted).
   Note a tab-separated array in an element with no space/newline (e.g. `1\t2`) falls to (c) fail -> String; real files use spaces/newlines. Cheap improvement allowed: treat `\t \r` like space in test (b).
   Note on entity handling: the Rust reader does NOT unescape text for unquoted values (only quoted-string payloads and attributes). With a DOM parser that decodes text up front, apply decoding only inside quoted strings: i.e. have the DOM return RAW (undecoded) text for `constant`/`pcVariable` elements, or decode first then skip step-2 unescape. Equivalent for all files the writer emits (writer only emits entities in quoted strings). Also quoted payload `&quot;` decodes to `"`.

Numbers in arrays are written by TunerStudio one row per line (rows x cols, row-major, whitespace-separated); LibreTune reads them flat and does not use rows/cols to reshape; consumers use the INI's shape.

### 1.4 .msq writing (exact layout; deterministic)
Output is built in this order, `\n` line endings, no trailing spaces:
```
<?xml version="1.0" encoding="ISO-8859-1"?>
[<!-- DESCRIPTION -->]                      only if description != NULL; text via xml_comment_escape (1.7)
<msq xmlns="http://www.msefi.com/:msq">
  <versionInfo signature="SIG" firmwareInfo="FW" nPages="N"/>
  [  <bibliography author="A" created="C" writeDate="M"/>]   only if any of author/created/modified set; attrs in that order, each only if set
  [  <iniMetadata signature="S" name="N" hash="H" specVersion="V" savedAt="T"/>]   if ini_metadata
  [  <constantManifest>
    <entry name="N" type="T" page="P" offset="O" scale="S" translate="X"/>   (4-space indent, manifest order)
  </constantManifest>]       (2-space indent on the wrapper lines)  only if manifest present (even if empty vector -> wrapper with no entries)
  [  <settings Comment="These setting are only used if this msq is opened without a project.">
    <setting name="CELSIUS"/>
  </settings>]               only if n_ecu_settings>0, order preserved
  <page number="P" size="SZ">          size attr only if page_sizes has P; else <page number="P">
    <constant ATTRS>VALUE</constant>   4-space indent; constants of that page sorted by name (byte-wise string order)
  </page>
  ... pages ascending by number; only pages that have >=1 constant (pages with only pageData are not emitted here)
  <pageData page="P">HEX</pageData>    ascending page; lowercase hex, 2 digits/byte, no whitespace/newlines; written even for empty page (empty element body `<pageData page="3"></pageData>`)
  [  <page number="-1">
    <pcVariable ATTRS>VALUE</pcVariable>   sorted by name
  </page>]                       only if pc_variables nonempty
</msq>
```
All lines indented with 2 spaces for children of `<msq>` (as shown) EXCEPT where noted. Every line ends `\n`, including final `</msq>\n`.
- `FW` = firmware_info or else signature. `N` (nPages) = max(count(pages), max(constant_pages values)+1 or 0) printed as decimal integer. Signature/FW/attrs XML-escaped (1.7).
- Constant page for a name lacking `constant_pages` entry = 0.
- `ATTRS` = ` name="NAME"` plus any of cols,digits,rows,units that are set, ALL SORTED ALPHABETICALLY by attribute key (so order is `cols digits name rows units`); format ` key="escaped"` each. Example: `<constant cols="1" digits="0" name="wueRates" rows="10" units="%">150 136 ...</constant>`. pcVariables use identical ATTRS from the same `constant_attrs` map.
- VALUE by kind: Scalar -> format_f64; Array -> format_f64 of each joined by single `" "` (ONE line, no row breaks); String -> `"` + xml_escape(s) + `"`; Bool -> `true`/`false` unquoted (NOTE: a Bool read from `"true"` re-saves unquoted; a quirk).
- **format_f64**: non-finite -> `0`; zero (incl -0.0) -> `0`; else the SHORTEST decimal string that round-trips to the same double (Rust `to_string`: never uses exponent notation, e.g. 1e21 prints `1000000000000000000000`, 1e-7 prints `0.0000001`; integers print without `.0`: `14.7`, `1`, `-0.3`, `12`). C port: try `%.1g`...`%.17g` ascending precision until `strtod` gives back identical double, then reformat to plain decimal (expand exponent form manually, strip trailing zeros / trailing dot). Tests: 14.7->`14.7`, 0.1->`0.1`, 0.0->`0`, array [14.7,1.0,-0.3] -> `14.7 1 -0.3`.
- scale/translate in manifest entries use plain Rust `{}` of f64 (same shortest form BUT without the zero/nonfinite special case: 1.0 -> `1`, 0.0 -> `0`, -0.0 -> `-0`, NaN -> `NaN`, inf -> `inf`); parse back with fallbacks above.
- Sorting is by Rust `String` Ord = bytewise UTF-8 (`strcmp`). Determinism requirement: saving twice yields identical bytes (test with 12 pages of `vec![page;32]`: output pageData blocks in ascending order).
- Not written to .msq: `version`, `annotations`, `author` when absent, `description` goes only as comment (and is NOT read back, no round-trip).
- File write: single `fwrite`; error -> propagate I/O error.

### 1.5 Signature-only / minimal files
Valid: `<msq><versionInfo signature="speeduino 202501"/><page number="0"><constant name="reqFuel">12.6</constant></page></msq>`. Output of this when saved contains `<constant name="reqFuel">12.6</constant>` and no `<settings`.

### 1.6 sanitize_corrupted_strings (post-load migration)
For every String value in `constants` and `pc_variables`: if it starts with `<` AND contains substring `constant` or `pcVariable` -> replace with empty String "" (log to stderr `[MIGRATION] ...`). Count fixed; log total.

### 1.7 Escaping
- `xml_escape` (attributes and quoted values; both write paths): `& -> &amp;  < -> &lt;  > -> &gt;  " -> &quot;  ' -> &apos;`.
- `xml_comment_escape`: replace every `--` with `- -` (single left-to-right pass, non-overlapping: `---` -> `- --`... Rust `replace` is non-overlapping left to right: "---" -> "- --"); if result ends with `-` append a space.
- `xml_unescape`: see section 0.

### 1.8 Operations (C signatures)
```c
lt_tune *lt_tune_new(const char *signature);
void     lt_tune_free(lt_tune*);
int      lt_tune_load(lt_tune **out, const char *path, char *err, size_t errsz);  /* 0 ok */
int      lt_tune_parse_msq(lt_tune **out, const char *utf8, size_t len, char *err, size_t errsz);
int      lt_tune_save(const lt_tune*, const char *path, char *err, size_t errsz);
char    *lt_tune_to_msq(const lt_tune*);                       /* malloc'd string */
void     lt_tune_touch(lt_tune*);                              /* modified = now */
void     lt_tune_set_page(lt_tune*, uint8_t page, const uint8_t*, size_t);
const uint8_t *lt_tune_get_page(const lt_tune*, uint8_t page, size_t *len);
void     lt_tune_patch_page_bytes(lt_tune*, uint8_t page, uint16_t off, const uint8_t*, size_t n); /* no-op if page absent or off+n > len; never grows */
void     lt_tune_set_constant(lt_tune*, const char*, lt_tune_value);      /* takes ownership */
void     lt_tune_set_constant_with_page(lt_tune*, const char*, lt_tune_value, uint8_t page);
const lt_tune_value *lt_tune_get_constant(const lt_tune*, const char*);
void     lt_tune_set_pc_variable(...); const lt_tune_value *lt_tune_get_pc_variable(...);
const lt_tune_value *lt_tune_get_value(const lt_tune*, const char*);   /* constants first, then pc_variables */
void     lt_tune_set_annotation(lt_tune*, const char *key, const lt_annotation*); /* touch() */
const lt_annotation *lt_tune_get_annotation(const lt_tune*, const char*);
int      lt_tune_delete_annotation(lt_tune*, const char*);   /* 1 if removed, touches */
/* table annotations: keys equal to table name OR starting with "table:" */
size_t   lt_tune_table_annotations(const lt_tune*, const char *table, const char **keys_out, size_t max);
int      lt_tune_save_pc_variables(const lt_tune*, const char *path, const char *signature);
int      lt_tune_load_pc_variables(lt_tune*, const char *path); /* load file, merge its constants AND pc_variables into this->pc_variables (overwrite) */
```
**pcVariableValues.msq** (save_pc_variables) exact layout (no indentation, names/signature NOT escaped in the original - escape them in the port):
```
<?xml version="1.0" encoding="ISO-8859-1"?>
<msq xmlns="http://www.msefi.com/:msq">
<bibliography author="LibreTune" writeDate="Sun May 03 20:46:57 2026"/>     (strftime "%a %b %d %H:%M:%S %Y", UTC)
<versionInfo fileFormat="4.0" signature="SIG"/>
<page number="-1">                                  (only if pc_variables nonempty)
<constant name="NAME">VALUE</constant>        (sorted by name; NOTE: <constant>, not <pcVariable>; no digits/units attrs)
</page>
</msq>
```
Loading it: its constants are on page -1 -> parsed as constants; merged into pc_variables (loaded.constants then loaded.pc_variables). Note load() requires constants non-empty OR signature non-empty (here signature is present so empty-pcvar files load).

### 1.9 JSON format (optional; serde default)
`.json` tunes are serde output of the struct: pretty JSON object with the field names above (snake_case: version, signature, author, description, created, modified, pages{"0":[bytes...]}, constants, constant_pages, constant_attrs, page_sizes, ecu_settings, pc_variables, ini_metadata, constant_manifest, annotations, firmware_info); TuneValue is externally tagged: `{"Scalar":1.5}`, `{"Array":[..]}`, `{"String":"x"}`, `{"Bool":true}`; AnnotationTag as string `"Info"|"Warning"|"Critical"|"Success"|"Todo"`. Fields with `#[serde(default)]` may be missing. Recommend: implement only if a JSON parser exists elsewhere in the project; .msq is the interchange format. Annotations are persisted ONLY here.

### 1.10 Rust parser quirks that a port may drop (document for fidelity)
- `<page` substring search also matches `<pageData`; quirk-only, harmless with a DOM parser.
- Rust finds attributes with substring search within the whole remainder of the file, so e.g. `iniMetadata` attribute `name=` could in a malformed file be picked from a later element; DOM parser fixes this. Also `signature=` in `<iniMetadata>` is looked up after `<iniMetadata`, independent of `<versionInfo`.
- A `<constant>` with no closing tag and not self-closing is skipped silently.
- Page numbers outside 0..255 (incl `-1`) leave current_page unchanged (documented in 1.3).
- `page_sizes` are collected from every `<page ...>` open tag with both `number` (uint8) and `size`; `-1` pages ignored.

### 1.11 Tests to port
1. Determinism: tune "speeduino 202501", pages 0..11 each `32 x byte(page)`, constants reqFuel=12.5, stoich=14.7; save 3 times -> identical bytes; `<pageData page="` occurrences ascending.
2. new("speeduino 202310") + set reqFuel 10.5 -> signature, get ok.
3. Self-closing: msq with `<!DOCTYPE msq PUBLIC ...>`, `<versionInfo firmwareInfo="rusEFI test"/>`, `<page number="0"><constant name="vinNumber"/><constant name="engineType">25</constant><constant name="tuneHidingKey"/><constant name="someValue">42.5</constant></page>`: vinNumber = String "", engineType = Scalar 25, someValue = Scalar 42.5 (signature empty but constants nonempty -> valid).
4. pageData test (above) + `<constant name="testValue">100</constant>` -> Scalar 100; pages[0] = 8 bytes, pages[1] = aa bb cc dd.
5. Quoted bits: `"1"`->String "1"; `"1.0"`->String "1.0"; `"Speed Density"`->String; `12.6`->Scalar; `"true"`->Bool true; saved output contains `"1.0"` with quotes.
6. Metadata round trip using SAMPLE:
```
<?xml version="1.0" encoding="ISO-8859-1"?>
<msq xmlns="http://www.msefi.com/:msq">
<bibliography author="LibreTune" writeDate="Sun May 03 20:46:57 NZST 2026"/>
<versionInfo fileFormat="5.0" firmwareInfo="Speeduino+2025.01.4" nPages="2" signature="speeduino 202501"/>
<settings Comment="These setting are only used if this msq is opened without a project.">
<setting name="CELSIUS"/>
<setting name="AFR"/>
</settings>
<page number="1" size="288">
<constant cols="1" digits="0" name="wueRates" rows="10" units="%">150 136 131 126 120 115 110 105 100 100</constant>
<constant digits="1" name="reqFuel" units="ms">12.6</constant>
</page>
<page number="-1">
<pcVariable digits="0" name="rpmhigh" units="rpm">7000</pcVariable>
</page>
</msq>
```
 -> constant_attrs.len==3; ecu_settings == [CELSIUS, AFR]; page_sizes[1]=="288"; saved text contains `<constant cols="1" digits="0" name="wueRates" rows="10" units="%">`, `<pcVariable digits="0" name="rpmhigh" units="rpm">`, `<setting name="CELSIUS"/>`, `<page number="1" size="288">`; wueRates parsed as Array of 10; second round trip yields identical constants/attrs/settings/page_sizes.
7. No-metadata file saves with `<constant name="reqFuel">` and no `<settings`.
8. Escaping: author `R&D "quoted" <lab>`, string const `R&D "quoted" <x> 'y'` -> file contains `&amp;`,`&lt;`,`&quot;`, never raw `R&D` or `<lab>`; reload equal to originals.
9. Number format (1.4): `>14.7<`, `>0.1<`, `>0<`, `>14.7 1 -0.3<`; pcVariable 14.7 also `>14.7<` (>=2 occurrences); reload stoich ~ 14.7.
10. Sanitize: String constant `"<constant name=x>"` loads as "".

---------------------------------------------------------------------
## 2. TuneCache (local ECU memory buffer with dirty tracking)

```c
typedef enum { LT_PAGE_NOT_LOADED, LT_PAGE_LOADING, LT_PAGE_CLEAN, LT_PAGE_DIRTY, LT_PAGE_PENDING, LT_PAGE_ERROR } lt_page_state;
typedef struct lt_tune_cache lt_tune_cache;
struct lt_tune_cache {
  uint8_t *pages[256]; size_t page_len[256];    /* raw page images (NULL = none) */
  uint16_t *page_sizes; size_t n_sizes;          /* from INI */
  uint8_t n_pages;
  lt_page_state state[256];                      /* default NOT_LOADED for unknown page */
  /* shadow: set of dirty (page,offset): use per-page bitmap sized page_len or 8KB(65536 bit) bitmap */
  uint8_t *dirty_bits[256];
  size_t dirty_count; int has_pending_burn;
  /* local_values: map<string,double> PC variable values (public) */
};
```
Operations (semantics exact):
- `lt_cache_from_definition(def)`: copy page_sizes and n_pages; for i in 0..len(page_sizes)-1 set state NOT_LOADED; no pages allocated (never invent zero buffers); no pending burn.
- `page_count()` = n_pages. `page_size(p)` = page_sizes[p] or "none". `page_state(p)` default NOT_LOADED.
- `is_fully_loaded()`: for p in 0..n_pages-1 state in {CLEAN, DIRTY, PENDING}. `is_loading()`: any state==LOADING. `pages_to_load()`: p in 0..n_pages-1 with state==NOT_LOADED (ERROR/LOADING excluded).
- `mark_loading(p)`, `mark_error(p)` set state. `load_page(p, data)`: replace image, state=CLEAN (does not touch dirty bits).
- `read_bytes(p, off, len)`: NULL unless state in {CLEAN,DIRTY,PENDING}, page image exists and `off+len <= image_len` (compute in size_t).
- `write_bytes(p, off, data, n)`: returns false if state not loaded-ish, no image, or `off+n > image_len` (never grows); else memcpy, mark bytes (p, off..off+n-1) dirty (n cast to u16 in Rust; use n<=65535), state=DIRTY, return true.
- `get_page(p)`: image only if state in {CLEAN,DIRTY,PENDING}.
- `has_dirty_data()` = dirty_count>0; `has_pending_burn()`; `dirty_byte_count()`; `dirty_pages()` = ascending distinct pages with dirty bytes.
- `mark_pending()`: every page with dirty bytes -> state PENDING; clear all dirty bits; has_pending_burn=true.
- `mark_burned()`: every PENDING -> CLEAN; has_pending_burn=false.
- `revert()`: clear all dirty bits; every page in state DIRTY -> NOT_LOADED (must reload; PENDING unchanged; image bytes remain but unreadable); does not touch has_pending_burn.
- `dirty_ranges(p)`: scan offset 0..page_size(p)-1 (page_size from INI sizes, 0 if unknown; NOT image length) collecting maximal contiguous runs of dirty offsets as (start,len) ascending; trailing run flushed.
Signatures: `lt_cache_*` mirroring the above; `int lt_cache_write_bytes(lt_tune_cache*, uint8_t p, uint16_t off, const uint8_t*, uint16_t n)`; `size_t lt_cache_dirty_ranges(const lt_tune_cache*, uint8_t p, lt_range *out, size_t max)`.

Tests: 2 pages (256, 512) all Clean zero-filled: write (0,10,[1,2,3,4]) -> dirty, state Dirty, read back [1,2,3,4]. Writes (0,10,[1,2,3]) and (0,20,[4,5]) -> dirty_ranges(0) = [(10,3),(20,2)]. Loading: both NotLoaded -> not fully loaded, pages_to_load [0,1]; load_page(0) -> [1]; load_page(1) -> fully loaded, []. from_definition(n_pages=1, sizes=[256]): get_page(0) None, write_bytes fails, after load_page(0, 256x9) write [1,2,3] ok and get_page starts [1,2,3]. Pending: write -> dirty && !pending; mark_pending -> !dirty, pending, state Pending; mark_burned -> !pending, Clean.

---------------------------------------------------------------------
## 3. Tune diff / merge

```c
typedef struct { char *name; lt_tune_value *value_a, *value_b; /* NULL = absent */
  int has_numeric; double numeric_diff; int has_percent; double percent_diff; } lt_tune_difference;
typedef struct { lt_tune_difference *diffs; size_t n; int signature_match; char *sig_a,*sig_b; size_t total_a,total_b; } lt_tune_diff;
lt_tune_diff *lt_tune_diff_compare(const lt_tune *a, const lt_tune *b);
int    lt_tune_diff_is_identical(const lt_tune_diff*);  /* n==0 && signature_match */
size_t lt_tune_diff_count(const lt_tune_diff*);
size_t lt_tune_merge_selected(lt_tune *target, const lt_tune *source, const char **names, size_t n);
```
compare: only `constants` (pc_variables, pages, attrs ignored). Union of names, sorted bytewise, deduped. Per name: present in one only -> different; both -> different iff `!values_equal`. Equality: same kind required (else unequal); Scalar |x-y|<1e-9; Bool ==; String strcmp; Array equal length and every |x-y|<1e-9. For different pairs where BOTH are Scalar: numeric_diff = b-a; percent_diff = (b-a)/a*100 only if |a|>1e-9 (else absent). `total_*` = constants count. `signature_match` = strcmp of signatures == 0.
merge_selected: for each requested name present in source.constants: target.set_constant(name, clone); if source has constant_pages[name], copy it to target; count++. If count>0 target.touch(). Names absent in source are skipped (not counted). Does not copy attrs. Returns count.
Tests: two new("test") tunes -> identical. reqFuel 10 vs 12 -> 1 diff, numeric 2.0, percent 20.0. Merge test: target{reqFuel 10, advance 15}, source{reqFuel 12, advance 20, newConst 5}; merge [reqFuel,newConst] -> returns 2; reqFuel 12, advance 15, newConst present.

---------------------------------------------------------------------
## 4. INI-version migration detection

```c
typedef struct { char *name; char *old_type; uint8_t old_page; uint16_t old_offset; double old_scale, old_translate;
                 char *new_type; uint8_t new_page; uint16_t new_offset; double new_scale, new_translate; char *change_description; } lt_constant_change;
typedef struct {
  char **missing_in_tune; size_t n_missing_in_tune;   /* in INI, not in manifest */
  char **missing_in_ini;  size_t n_missing_in_ini;    /* in manifest, not in INI */
  lt_constant_change *type_changed; size_t n_type;
  lt_constant_change *scale_changed; size_t n_scale;
  lt_constant_change *location_changed; size_t n_loc;
  int can_auto_migrate, requires_user_review;
  char severity[8];   /* "none","low","medium","high" */
} lt_migration_report;
void lt_migration_empty(lt_migration_report*);   /* can_auto_migrate=1, review=0, severity "none" */
int  lt_migration_has_changes(const lt_migration_report*);  /* any of the 5 lists nonempty */
char *lt_migration_summary(const lt_migration_report*);
void lt_migration_compare(lt_migration_report *out, const lt_manifest_entry *saved, size_t n, const lt_ecu_definition *ini);
```
compare_manifests algorithm:
1. Build name->entry map of saved manifest (later duplicates win).
2. For each INI constant (iteration order of the INI's constant map; sort by name for determinism) whose name is not in the saved map: skip if `is_pc_variable`; else append to missing_in_tune.
3. For each saved entry in manifest order: if INI has constant of that name: `cur_type` = data type name string ("U08","S08","U16","S16","U32","S32","F32","F64","String","Bits" = exact Rust Debug of DataType enum); then classify (first match wins):
   - `entry.data_type != cur_type` -> type_changed, description `Type changed from {old} to {new}`.
   - else `|entry.scale-cur.scale|>1e-9 || |entry.translate-cur.translate|>1e-9` -> scale_changed, description `Scale changed from {os}*x+{ot} to {ns}*x+{nt}` (floats printed with Rust `{}` shortest form: `1`, `0.1`, `-40`).
   - else `entry.page != cur.page || entry.offset != cur.offset` -> location_changed, description `Moved from page {op} offset {oo} to page {np} offset {no}`.
   Every change record carries all old (manifest) and new (INI) fields. If INI lacks the name -> missing_in_ini.
   (Note: the manifest type for array/table constants is whatever the writer stored; comparison is plain string equality with the data type name only, so manifests writing "array[16x16]" would always report type_changed - the writer side should record the same Debug-name strings.)
4. Severity: if type_changed or missing_in_ini nonempty -> review=1, auto=0, "high". elif scale_changed -> review=1, auto=1, "medium". elif missing_in_tune or location_changed -> auto=1, review=0, "low". else defaults ("none", auto=1, review=0).
summary: parts joined by `, ` in this order and wording: `{n} new constants` (missing_in_tune), `{n} removed constants` (missing_in_ini), `{n} type changes`, `{n} scale changes`, `{n} location changes`; none -> `No changes detected`.
Manifest generation (who fills it) is in the app layer: for each non-PC INI constant write {name, type name, page, offset, scale, translate}.
Tests: empty manifest + default (empty) definition -> no changes, severity "none". Summary with 2 missing_in_tune + 1 missing_in_ini contains `2 new constants` and `1 removed constants`. Manifest {egoCorrection U08 p3 o50 scale1 trans0} vs INI constant {page5 offset100 default type/scale} -> location_changed=1, type/scale empty, severity low, auto=1, review=0, old/new page/offset 3/50 -> 5/100 (default Constant must have type U08, scale 1, translate 0). Manifest rpmLimit U08 p3 o50 vs INI U16 p5 o100 -> type_changed=1, location_changed=0, severity high.

---------------------------------------------------------------------
## 5. TunerStudio .table file (single-table import/export)

### 5.1 Structure
```c
typedef struct { size_t cols, rows; double *x_bins; /*cols*/ double *y_bins; /*rows*/ double *z; /*rows*cols row-major: z[r*cols+c]*/ } lt_table_file;
int   lt_table_file_parse(const char *xml, lt_table_file *out, char *err, size_t errsz);
char *lt_table_file_write(const char *x_name, const char *y_name, const double *x, size_t nx, const double *y, size_t ny, const double *z /* ny rows x nx cols */);
```
### 5.2 Exact write layout (4-space indent per depth by quick-xml indenting writer; text nodes are written inline with their own newlines)
```
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<tableData xmlns="http://www.EFIAnalytics.com/:table">
    <bibliography author="LibreTune" company="LibreTune, open source" writeDate="epoch 1735000000 UTC"/>
    <versionInfo fileFormat="1.0"/>
    <table cols="NX" rows="NY">
        <xAxis cols="NX" name="XNAME">V1
V2
...Vn
</xAxis>
        <yAxis name="YNAME" rows="NY">V1
...
</yAxis>
        <zValues cols="NX" rows="NY">a b c
d e f
</zValues>
    </table>
</tableData>
```
Whitespace is not significant to the reader, so any consistent layout (e.g. TunerStudio's own: values each on its own line indented 9 spaces, z rows space separated) is acceptable. Rules: cols = len(x_bins), rows = len(y_bins); every axis value written followed by `\n`; each z row = values joined by single space + `\n`. Floats via shortest round-trip `{}` form (like 1.4 format_f64 but zero not special-cased except sign: `6`, `206.8`, `0.6`). `writeDate` = `epoch <unix secs> UTC` (informational; any string, TunerStudio uses `Fri Aug 14 21:39:32 COT 2026`). Attribute values XML-escaped (name attrs). NOTE the namespace host here is `EFIAnalytics.com`; real files seen with `usEasyDocs.com` too -> never validate it.

### 5.3 Parse algorithm (tolerant; match local element names only)
Stream through elements; set `saw_table_data` on `<tableData>`; on `<table>` read `cols`,`rows` attrs as unsigned ints (parse failure -> treated as missing); `<xAxis>` / `<yAxis>` / `<zValues>` open/close toggles in-flags; text in xAxis -> append all whitespace-separated tokens that parse as f64 (non-numeric tokens silently dropped) to x_bins; yAxis likewise to y_bins; zValues: split text into lines; each line -> whitespace-separated floats; non-empty rows appended as a z row (row boundaries = line breaks, NOT cols).
Errors (in this check order): no `<tableData>` -> `not a TunerStudio table file (missing <tableData>)`; cols missing -> `malformed table file: missing <table cols=...>`; rows missing -> `... missing <table rows=...>`; `len(x_bins)!=cols` -> `xAxis has N values, table declares M cols`; `len(y_bins)!=rows` -> `yAxis has ...`; `len(z rows)!=rows` or any row length != cols -> `zValues is not a {cols}x{rows} grid`. XML syntax errors -> error. Attribute order irrelevant. The `<table>` attribute search uses only the `<table>` element (not xAxis `cols` / zValues `cols`).
Note the Rust trims text nodes (`trim_text(true)`) which does not matter since tokens are split on whitespace; but note z rows inside ONE text node are split on `\n` (if the DOM merges text it is identical).

### 5.4 Tests
- REAL_EXPORT (verbatim above in source): `<table cols="8" rows="2">`, xAxis name="VBatt" values 6.00 8.00 10.00 11.00 12.00 13.00 14.00 15.00 (one per line), yAxis name="pressureCorrectionReference" rows="2": 206.80 413.70, zValues rows `4.0 2.4 1.65 1.25 1.05 0.85 0.79 0.70` / `3.08 1.65 1.15 1.19 0.99 0.76 0.64 0.6` with bibliography `author="EFI Analytics - philip.tobin@yahoo.com" company="EFI Analytics, copyright 2010, All Rights Reserved." writeDate="Fri Aug 14 22:08:01 COT 2026"` and namespace `http://www.EFIAnalytics.com/:table`. -> cols 8, rows 2, x=[6,8,10,11,12,13,14,15], y=[206.8,413.7], z[0][0]=4.0, z[1][7]=0.6.
- Round trip: x [600,1200,3000], y [10,50], z [[1,2,3],[4,5,6]] -> write then parse equal.
- `<notATable/>` -> NotATable error. REAL_EXPORT with `cols="8"` replaced by `cols="9"` everywhere (both table and xAxis/zValues) -> error.

---------------------------------------------------------------------
## 6. TunerStudio .tuneView file

### 6.1 Structures
```c
#define LT_TUNEVIEW_NS "http://www.EFIAnalytics.com/:tuneView"
typedef struct { char *k,*v; } lt_kv;                  /* kept SORTED by key (BTreeMap; bytewise) */
typedef struct { lt_kv *a; size_t n; } lt_kvmap;       /* insert replaces existing key */
typedef struct { char *name; char *ts_type; /* NULL if no type attr */ lt_kvmap attrs; /* all other attrs */ char *text; /* decoded; "" if none */ } lt_tune_prop;
typedef struct { char *comp_type; /* "" if no type attr */ lt_tune_prop *props; size_t n; } lt_tune_comp;
typedef struct { lt_kvmap bibliography, version_info, tuning_view_attrs; char *preview_image; /* NULL=None */ lt_tune_comp *comps; size_t ncomps; } lt_tune_view;
int   lt_tune_view_parse(const char *xml, lt_tune_view *out, char *err, size_t errsz);
char *lt_tune_view_write(const lt_tune_view*);
```
### 6.2 Parse semantics
Namespace prefixes stripped from element and attribute names (`tv:tuneView` -> `tuneView`; `xmlns:tv` attribute key becomes `tv`, harmless). Attribute values normalized (section 0). State machine over events (text NOT trimmed):
- Start `tuneView` -> in_tune_view. Start `tuningView` (inside tuneView) -> in_tuning_view, `tuning_view_attrs` = all its attrs. Start `previewImage` (inside tuneView) -> in_preview, clear text buffer.
- Start `tuneComp` (inside tuningView) -> new comp, `comp_type` = attr `type` or "". 
- Start of any other element while a comp is open -> new current_prop `{name, ts_type=attr "type" removed from attrs, attrs=rest}`; clear text buffer. (Only ONE current_prop at a time: nested elements inside a property overwrite it and mismatched closes are ignored; stock files have flat properties.)
- Empty (self-closing) elements: `bibliography` -> bibliography = all attrs; `versionInfo` -> version_info = all attrs; (both must be inside tuneView; Rust only handles them as Empty events, so a non-self-closing `<bibliography ...></bibliography>` is ignored - port may accept both); `tuneComp` (self-closing, in tuningView) -> comp with no props pushed; other self-closing element while a comp is open -> prop with empty text pushed immediately.
- Text and CDATA events append to the text buffer (everywhere; buffer cleared at starts of previewImage/props and at ends).
- End `tuneView` -> in_tune_view=false; End `tuningView` -> false; End `previewImage` -> if in_preview: `preview_image = text with ALL whitespace removed`; End `tuneComp` -> push current comp (if any); End of other element: if current_prop and comp: if `prop.name == endname` -> prop.text = buffer (raw, untrimmed, decoded), push to comp; else keep prop pending. Buffer cleared in all "other end" cases.
- Whitespace text between properties accumulates in buffer but is cleared at the next start/end, so is never attached.
- Validity: error `Invalid tuneView document: no recognized tuneView content` if bibliography empty AND version_info empty AND no comps AND preview_image None. XML syntax errors -> error.
### 6.3 Write layout (deterministic; `\n`; no indentation)
```
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<tuneView xmlns="http://www.EFIAnalytics.com/:tuneView">
<bibliography k="v" .../>          only if non-empty; attrs in sorted key order
<versionInfo k="v" .../>           only if non-empty
<previewImage>BASE64</previewImage>   only if Some (written raw, unescaped)
<tuningView k="v" ...>             always; attrs sorted (no attrs -> `<tuningView>`)
<tuneComp type="TYPE">
<PropName type="T" other="x" ...>TEXT</PropName>      type attr first (if Some), then remaining attrs sorted
<PropName type="T"/>                                   when text is empty
</tuneComp>
</tuningView>
</tuneView>
```
Escaping: attributes: `& < > "` -> entities, `\r` -> `&#13;`, `\n` -> `&#10;`. Text: `& < >` -> entities, `\r` -> `&#13;` (quotes and `\n` NOT escaped). Property names are written unescaped. Round-trip guarantee: parse -> write -> parse yields equal structure (compare all fields; sorted maps make order-independent).
### 6.4 Tests
SAMPLE:
```
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<tuneView xmlns="http://www.EFIAnalytics.com/:tuneView">
<bibliography author="Test" company="LibreTune" viewName="Demo" writeDate="2026-04-29"/>
<versionInfo enabledCondition="" fileFormat="1.0" firmwareSignature="MS3 Format 0545.02 "/>
<previewImage>QUJD</previewImage>
<tuningView Id="42" ShieldedDuringEdit="false">
<tuneComp type="TuneSettingsPanel">
<Dirty type="boolean">true</Dirty>
<SettingPanelName type="String">antilag</SettingPanelName>
<Id type="String"/>
<RelativeHeight type="double">1.0</RelativeHeight>
<WarnColor alpha="255" blue="0" green="242" red="242" type="Color">-85552</WarnColor>
</tuneComp>
<tuneComp type="TuneSlectableTable">
<SelectedTableName type="String">als_timing_tbl</SelectedTableName>
</tuneComp>
</tuningView>
</tuneView>
```
-> bibliography.author "Test"; version_info.firmwareSignature `MS3 Format 0545.02 ` (trailing space kept); preview "QUJD"; 2 comps; comp0 type TuneSettingsPanel with 5 props; `Id` ts_type "String", text ""; `WarnColor` ts_type "Color", attrs alpha "255", text "-85552". Round trip equal. `<?xml version="1.0"?><other/>` -> InvalidFormat. Prefix test: `<tv:tuneView xmlns:tv="..."><tv:bibliography author="x"/><tv:tuningView><tv:tuneComp type="X"><tv:Foo type="String">bar</tv:Foo></tv:tuneComp></tv:tuningView></tv:tuneView>` -> 1 comp, props[0].text "bar". Stock corpus test (`reference/TunerStudioMS/TuneView/*.tuneView`) round-trips losslessly if the directory exists; read file as UTF-8 else lossy (replace invalid with U+FFFD) - skip when absent.

---------------------------------------------------------------------
## 7. Misc gotchas summary
- Quoted vs unquoted values decide type (String label vs Scalar). Never coerce quoted numbers.
- Page `-1` => pcVariables (writer) but constants parsed from a `-1` page get the carried-over page number.
- `constant_attrs` is shared between constants and pcVariables (same name namespace).
- Attribute order in written `<constant>`: strictly alphabetical, including `name`.
- Arrays are written on ONE line, space separated; readers must also accept newline-separated rows and `[a, b]` bracket form.
- Float writer: shortest round-trip, no exponent notation, `0` for zero/NaN/inf.
- Everything sorted bytewise for deterministic output; pageData after all `<page>` blocks, pcVariables page last.
