# 05 - Dashboards and Gauges (TunerStudio .dash/.gauge, INI [GaugeConfigurations], painters)

Source: `libretune-core/src/dash/{types,parser,writer,validation,templates}.rs`, `ini/gauges.rs`, `libretune-app/src/components/gauges/**` (canvas painters; re-derived here for a C framebuffer).
Out of scope: Tauri commands, React designer UI, asset cache, Lua/LLM/network. Prefix all C names `lt_`.

---
## 1. Data model

### 1.1 Color
```c
typedef struct { unsigned char a, r, g, b; } lt_color;
```
- ARGB int (signed 32-bit, big-endian bytes A,R,G,B): `argb = (int32)((a<<24)|(r<<16)|(g<<8)|b)`. e.g. -16777216 = FF000000 black, -1 = white, -65536 = FF0000 red, -6710887 = 999999.
  `lt_color_from_argb(int32)`: a=(v>>24)&255 (use unsigned cast), etc. `lt_color_to_argb`.
- CSS hex: `#rrggbb` (a=255) or `#rrggbbaa`; any other length (after stripping leading `#`s) = failure. Output: `#rrggbb` if a==255 else `#rrggbbaa`.
- `lt_color_from_xml_attrs`: default struct is a=r=g=b=0 (TsColor::default), attrs `alpha`,`red`,`green`,`blue` parse as u8; **alpha parse failure -> 255, other channel failure -> 0**, absent attr leaves 0 (so an element with no alpha attr has alpha 0!). Element text, if it parses as int32, OVERRIDES the attrs.
- Constants: black 255,0,0,0; white; red; yellow 255,255,0; green 0,255,0; transparent 0,0,0,0.

### 1.2 Enums
`GaugePainter` (default BasicReadout): AnalogGauge, BasicAnalogGauge, CircleAnalogGauge, AsymmetricSweepGauge, BasicReadout, HorizontalBarGauge, VerticalBarGauge, HorizontalLineGauge, HorizontalDashedBar, VerticalDashedBar, AnalogBarGauge, AnalogMovingBarGauge, Histogram, LineGraph, RoundGauge, RoundDashedGauge, FuelMeter, Tachometer, TelemetryStat, MultiChannelTrend.

TS strings written (`to_ts_string`): "Analog Gauge","Basic Analog Gauge","Circle Analog Gauge","Asymetric Sweep Gauge"(sic, one m),"Basic Readout","Horizontal Bar Gauge","Vertical Bar Gauge","Horizontal Line Gauge","Horizontal Dashed Bar Gauge","Vertical Dashed Bar Gauge","Analog Bar Gauge","Analog Moving Bar Gauge","Histogram","Line Graph","Round Analog Gauge","Round Dashed Gauge","Fuel Meter","Tachometer","Telemetry Stat","Multi Channel Trend".

`from_ts_string(s)`: t=trim(s), l=lowercase(t). Evaluate IN THIS ORDER, first match wins:
1. l contains "roundanaloggaugepainter" or t=="Round Analog Gauge" or t=="Round Gauge" -> RoundGauge
2. l contains "rounddashed" or t=="Round Dashed Gauge" -> RoundDashedGauge
3. l contains "fuelmeter" or t=="Fuel Meter" -> FuelMeter
4. l contains "tachometer" or t=="Tachometer" -> Tachometer
5. l contains "horizontaldashedbar" or t=="Horizontal Dashed Bar Gauge" -> HorizontalDashedBar
6. exact (case-sensitive) matches: "Analog Gauge"|"AnalogGaugePainter"->AnalogGauge; "Basic Analog Gauge"->BasicAnalogGauge; "Circle Analog Gauge"|"CircleAnalogGaugePainter"->CircleAnalogGauge; "Asymetric Sweep Gauge"|"AsymetricSweepGaugePainter"->AsymmetricSweepGauge; "Basic Readout"|"BasicReadoutGaugePainter"->BasicReadout; "Horizontal Bar Gauge"|"HorizontalBarPainter"->HorizontalBarGauge; "Vertical Bar Gauge"|"VerticalBarPainter"->VerticalBarGauge; "Horizontal Line Gauge"|"HorizontalLinePainter"->HorizontalLineGauge; "Vertical Dashed Bar Gauge"|"VerticalDashedBarPainter"->VerticalDashedBar; "Analog Bar Gauge"|"AnalogBarPainter"->AnalogBarGauge; "Analog Moving Bar Gauge"|"AnalogMovingBarGaugePainter"->AnalogMovingBarGauge; "Histogram"|"HistogramPainter"; "Line Graph"|"LineGraphPainter"; "Telemetry Stat"; "Multi Channel Trend".
7. substring (lowercased) fallbacks in order: analoggaugepainter->AnalogGauge, basicreadoutgaugepainter->BasicReadout, horizontalbarpainter->HorizontalBar, verticalbarpainter->VerticalBar, horizontallinepainter->HorizontalLine, verticaldashedbarpainter->VerticalDashedBar, analogbarpainter->AnalogBar, analogmovingbargaugepainter->AnalogMovingBar, histogrampainter->Histogram, linegraphpainter->LineGraph.
8. else BasicReadout. (Java fully-qualified names like `com.efiAnalytics.apps.ts.dashboard.renderers.RoundAnalogGaugePainter` are caught by the substring rules.)

`IndicatorPainter` (default BasicRectangleIndicator; Bulb, Led). Write strings "Basic Rectangle Indicator","Bulb Indicator","LED". Parse: LED if eq_ignore_case "LED" or lower contains "ledpainter"/"ledindicator"; Bulb if t=="Bulb Indicator" or contains "bulbindicator"; Rectangle if t=="Basic Rectangle Indicator" or contains "rectangleindicatorpainter"/"basicrectangleindicator"; else Rectangle.

`BackgroundStyle` Tile(default)|Stretch|Center|Fit (case-sensitive strings). `ResourceType` png|gif|ttf (case-insens. parse, default png).

### 1.3 GaugeConfig (field: default)
```c
typedef struct lt_gauge {
  char *id; lt_gaugepainter painter /*BasicReadout*/; char *gauge_style /*"Basic Readout"; provenance only, never used for rendering*/;
  char *output_channel, *title, *units;      /* "" */
  double value /*0, demo*/, min /*0*/, max /*100*/;
  char *min_vp,*max_vp;                      /* NULL; ValueProvider/ECU var names */
  int has_default_min; double default_min; int has_default_max; double default_max;
  int peg_limits /*1*/;
  /* optional thresholds: has_X flag + value */
  double low_warning, high_warning, low_critical, high_critical;   /* NULL/unset */
  char *low_warning_vp,*high_warning_vp,*low_critical_vp,*high_critical_vp;
  lt_color back_color /*black*/, font_color /*white*/, trim_color /*255,102,102,153*/,
           warn_color /*yellow*/, critical_color /*red*/, needle_color /*255,255,102,0*/;
  int value_digits /*0*/, label_digits /*0*/; char *font_family /*""*/; int font_size_adjustment /*0*/; int italic_font /*0*/;
  int sweep_angle /*270*/, start_angle /*135*/, face_angle /*360*/, sweep_begin_degree /*135*/; int counter_clockwise /*0*/;
  double major_ticks /*-1 = auto*/, minor_ticks /*-1*/;
  double relative_x /*0*/, relative_y /*0*/, relative_width /*0.25*/, relative_height /*0.25*/;
  int border_width /*3*/, shortest_size /*50*/, shape_locked_to_aspect /*0*/, antialiasing_on /*1*/;
  char *background_image_file_name, *needle_image_file_name;   /* NULL */
  /* optional doubles: needle_length, needle_pivot_offset_x/y, needle_image_offset_x/y */
  double history_value /*0*/; int history_delay /*15000 ms*/; int needle_smoothing /*1; round-trip only*/;
  char *short_click_action,*long_click_action;  /* NULL */
  int display_value_at_180 /*0*/;
  char *enabled_condition /*NULL; INI expression, hide when false*/;
  int peak_hold /*0; serialized as ShowHistory*/;
  int has_hysteresis; double hysteresis;
  lt_kv *extra_attrs;  /* ordered map string->string, written sorted by key (BTreeMap) */
  /* runtime only: run_demo, must_paint, invalid_state, dirty */
} lt_gauge;
```
### 1.4 IndicatorConfig defaults
id "", painter Rectangle, output_channel "", value 0, on_text "ON", off_text "OFF", on_text_color black, off_text_color (255,51,51,51), on_background_color green(0,255,0), off_background_color transparent, on/off_image_file_name NULL, rel x 0, y 0, w 0.1, h 0.05, font_family "", italic 0, antialiasing_on 1, short/long_click_action NULL, ecu_configuration_name NULL, enabled_condition NULL, extra_attrs {}.

### 1.5 Cluster / file
- GaugeCluster: anti_aliasing 1, force_aspect 0, force_aspect_width/height 0.0, background_dither_color (optional), cluster_background_color black, cluster_background_image_file_name NULL, cluster_background_image_style Tile, embedded_images[], components[] (ordered union Gauge|Indicator), cluster_layout (NULL), enabled_condition (NULL), extra_attrs.
- EmbeddedImage: file_name, image_id, resource_type, data (base64 text, trimmed).
- DashFile: bibliography{author,company,write_date}, version_info{file_format "3.0", firmware_signature NULL}, gauge_cluster, additional_clusters[] (never populated by parser/writer - only first `<gaugeCluster>` is kept: a second one REPLACES the first's attributes via `dash.gauge_cluster = parse(...)` and components keep appending to the one cluster), extra_attrs (unused).
- Default DashFile bibliography: author "LibreTune", company "LibreTune Project", write_date = now "%Y-%m-%d %H:%M:%S".
- GaugeFile (.gauge): bibliography, version_info (file_format default "1.0"), embedded_images[], one gauge.

---
## 2. XML format

### 2.1 .dash document
```xml
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<dsh xmlns="http://www.EFIAnalytics.com/:dsh">
    <bibliography author=".." company=".." writeDate=".."/>
    <versionInfo fileFormat="3.0" firmwareSignature="speeduino"/>
    <gaugeCluster antiAliasing="true" clusterBackgroundColor="-16777216" ...>
        <imageFile fileName="x.png" imageId="id" type="png">BASE64...</imageFile>
        <dashComp type="Gauge">
            <Title type="String">RPM</Title>
            <Min type="double">0.0</Min>
            <ValueDigits type="int">0</ValueDigits>
            <PegLimits type="boolean">true</PegLimits>
            <BackColor type="java.awt.Color" alpha="255" red="0" green="0" blue="0">-16777216</BackColor>
            ...
        </dashComp>
    </gaugeCluster>
</dsh>
```
Writer: 4-space indent, declaration `<?xml version="1.0" encoding="UTF-8" standalone="no"?>`; root name and namespace not checked on parse (any root works, test fixture uses `<dashboard>` and untyped elements with integer colors). Elements `bibliography`/`versionInfo` are self-closing.

### 2.2 Parser algorithm (event-based, forgiving)
State: in_dash_comp, current_gauge, current_indicator, current_image, current_property, color prop/value, text_buf. Loop over XML events:
- Text/CDATA: APPEND to text_buf (do not overwrite). Entity refs `&amp; &lt; &gt; &quot; &apos;` each resolved to a char and appended (unknown entities dropped). Whitespace-only text is trimmed away by the reader; leading/trailing whitespace of values is trimmed again in `set_*_property`.
- Start `gaugeCluster`: cluster = parse attrs (2.3). Replaces whole cluster struct (see 1.5).
- Start `dashComp`: read attr `type`; if lowercase(type) contains "indicator" -> new Indicator else new Gauge (unknown/missing type = Gauge).
- Start `imageFile`: new image from attrs `fileName`,`imageId`,`type`.
- Any other Start while in_dash_comp: current_property=name; if name is a color property, remember color attrs.
- Empty (self-closing) `bibliography` -> attrs author, company, writeDate (missing -> ""); `versionInfo` -> fileFormat (default "3.0"), firmwareSignature (optional). Empty color element inside dashComp -> color from attrs only.
- End `dashComp`: push current gauge (preferred) else indicator onto components. End `imageFile`: data = trim(text_buf), push to cluster images. End other (in dashComp): if color property: color = attrs, overridden by int32 parse of trimmed text; apply to gauge (BackColor,FontColor,TrimColor,WarnColor,CriticalColor,NeedleColor) or indicator (OnTextColor,OffTextColor,OnBackgroundColor,OffBackgroundColor). Else if name == current_property: apply property with text_buf; clear current_property. text_buf cleared on every Start/End.
- Malformed XML (mismatched/unclosed tags) -> error. Missing root children are fine; empty dashboard parses OK with 0 components.
- `type="..."` attribute on property elements is ignored on read.

### 2.3 gaugeCluster attributes
read: antiAliasing (bool parse else true), forceAspect (else false), forceAspectWidth/Height (f64 else 0), clusterBackgroundColor (int32 ARGB), backgroundDitherColor (nonempty int32), clusterBackgroundImageFileName (nonempty), clusterBackgroundImageStyle (Tile|Stretch|Center|Fit else Tile), clusterLayout (nonempty), enabledCondition (nonempty). Every other attribute (compared case-insensitively against the known list) -> extra_attrs (entity-unescaped).
write order: antiAliasing "true"/"false"; if force_aspect: forceAspect="true" and (only if >0) forceAspectWidth/Height (Rust f64 Display: `16` prints as "16", `16.5` as "16.5" - i.e. shortest round-trip, no trailing ".0"); clusterBackgroundColor (int32 decimal); backgroundDitherColor?; clusterBackgroundImageFileName?; clusterBackgroundImageStyle (always); clusterLayout?; enabledCondition?; extra_attrs sorted.
Note: cluster attribute `forceAspect=false` is not written at all.

### 2.4 Gauge property table (element name -> field, parse, fallback on parse error)
All values trimmed first. bool parse accepts only "true"/"false".
Id, Title, Units, OutputChannel: string. Value f64(0). Min f64(0). Max f64(100). MinVP/MaxVP/LowWarningVP/HighWarningVP/LowCriticalVP/HighCriticalVP: string, empty->None. DefaultMin/DefaultMax/LowWarning/HighWarning/LowCritical/HighCritical: f64, parse failure -> None. PegLimits bool(true). ValueDigits int(0). LabelDigits int(0). FontFamily str. FontSizeAdjustment int(0). ItalicFont bool(false). SweepAngle int(270). StartAngle int(135). FaceAngle int(**360**). SweepBeginDegree int(135). CounterClockwise bool(false). MajorTicks/MinorTicks f64(-1). RelativeX/Y f64(0). RelativeWidth/Height f64(0.25). BorderWidth int(3). ShortestSize int(50). ShapeLockedToAspect bool(false). AntialiasingOn bool(true). BackgroundImageFileName/NeedleImageFileName: "null" or empty -> None. ShowHistory -> peak_hold bool(false). HistoryValue f64(0). HistoryDelay int(15000). NeedleSmoothing int(1). ShortClickAction/LongClickAction: "null"/empty -> None. DisplayValueAt180 bool(false). GaugeStyle str. GaugePainter -> from_ts_string. RunDemo bool. EnabledCondition str (empty->None). Hysteresis f64 (fail->None). **Any other name -> extra_attrs[name]=value** (includes lt_* and unmodeled TS props such as NeedleLength etc.; needle_length/pivot/image-offset fields exist in the struct but are NOT parsed from XML by name - they are only reachable through the app JSON; a C port may parse `NeedleLength`,`NeedlePivotOffsetX/Y`,`NeedleImageOffsetX/Y` as optional doubles at its discretion, but then must write them back too).
Indicator properties: Id, Value f64, OutputChannel, OnText, OffText, OnImageFileName/OffImageFileName ("null"/empty->None), RelativeX/Y(0), RelativeWidth(0.1), RelativeHeight(0.05), FontFamily, ItalicFont, AntialiasingOn(true), ShortClickAction/LongClickAction, EcuConfigurationName (empty->None), Painter -> IndicatorPainter, RunDemo, EnabledCondition, else extra_attrs. Indicators have no Title/Min/Max.

### 2.5 Writer (exact emit order)
Property element forms (all `<Name type="T">text</Name>`):
- string: `type="String"`; text escaped normally, but if value contains any of `< > &` it is wrapped in `<![CDATA[...]]>` (preserves spaces, e.g. `rpm > 3000`).
- double: `type="double"`, Rust f64 `to_string()` (shortest round trip; 0.0 prints "0", 8000.0 -> "8000", 0.45 -> "0.45"; NaN/inf would print "NaN"/"inf"). C port: emit with `%.17g` then shortest-roundtrip search (try %.1g..%.17g until strtod round-trips); do not append ".0".
- int: `type="int"`; boolean: `type="boolean"` "true"/"false".
- color: `<Name type="java.awt.Color" alpha="A" red="R" green="G" blue="B">ARGBINT</Name>`.
Gauge `<dashComp type="Gauge">` order: Id, Title, Units, Value, OutputChannel, Min, Max, [MinVP], [MaxVP], [DefaultMin], [DefaultMax], PegLimits, [LowWarning], [HighWarning], [LowCritical], [HighCritical], [LowWarningVP], [HighWarningVP], [LowCriticalVP], [HighCriticalVP], ValueDigits, LabelDigits, FontFamily, FontSizeAdjustment, ItalicFont, SweepAngle, StartAngle, FaceAngle, SweepBeginDegree, CounterClockwise, MajorTicks, MinorTicks, BackColor, FontColor, TrimColor, WarnColor, CriticalColor, NeedleColor, RelativeX, RelativeY, RelativeWidth, RelativeHeight, BorderWidth, ShortestSize, ShapeLockedToAspect, AntialiasingOn, BackgroundImageFileName (String, "null" when absent), NeedleImageFileName ("null"), ShowHistory (from peak_hold), HistoryValue, HistoryDelay, NeedleSmoothing, ShortClickAction ("null"), LongClickAction ("null"), DisplayValueAt180, GaugeStyle, GaugePainter (TS string), RunDemo, [EnabledCondition], [Hysteresis], then extra_attrs as String props sorted by key. [..] = only if set. `Value` and runtime flags: Value written; RunDemo written from the (non-persisted) flag.
Indicator `<dashComp type="Indicator">`: Id, Value, OutputChannel, OnText, OffText, OnImageFileName ("null"), OffImageFileName ("null"), OnTextColor, OffTextColor, OnBackgroundColor, OffBackgroundColor, RelativeX/Y/Width/Height, FontFamily, ItalicFont, AntialiasingOn, ShortClickAction ("null"), LongClickAction ("null"), [EcuConfigurationName], Painter, RunDemo, [EnabledCondition], extra_attrs.
Cluster body order: embedded `<imageFile fileName imageId type="png|gif|ttf">base64</imageFile>` entries, then components in order. Only `gauge_cluster` is written (additional_clusters dropped).
.gauge file: root `<gauge xmlns="http://www.EFIAnalytics.com/:gauge">`, bibliography, versionInfo, imageFile*, then nested `<gauge>` containing one `<dashComp type="Gauge">...`. Parse: gauge_depth counter on `gauge` tags; first dashComp inside depth>0 becomes the gauge; same property rules (gauge only, no indicators). A gauge file with no dashComp yields the default gauge.

### 2.6 Public API
```c
int  lt_dash_parse(const char *xml, size_t len, lt_dashfile *out, char *err, size_t errsz);
int  lt_dash_write(const lt_dashfile *d, lt_buf *out);
int  lt_gaugefile_parse(const char*, size_t, lt_gaugefile*, char*err,size_t);
int  lt_gaugefile_write(const lt_gaugefile*, lt_buf*);
lt_dashfile* lt_dash_default(void); void lt_dash_free(lt_dashfile*);
lt_gaugepainter lt_painter_from_ts(const char*); const char* lt_painter_to_ts(lt_gaugepainter);
lt_indpainter lt_indpainter_from_ts(const char*); const char* lt_indpainter_to_ts(lt_indpainter);
lt_color lt_color_from_argb(int32_t); int32_t lt_color_to_argb(lt_color);
int lt_color_from_css(const char*, lt_color*); void lt_color_to_css(lt_color, char out[10]);
```
C port needs a tiny non-validating XML pull parser: tags, attributes (single or double quoted), text, CDATA, comments `<!-- -->`, `<?...?>`, `<!DOCTYPE ...>` skip, entities incl. numeric `&#N;`/`&#xH;` (superset). Must report error on mismatched end tag/EOF inside element.

---
## 3. Validation (`lt_dash_validate(dash, ecu_channels_or_NULL, &report)`)
Only the first cluster. Errors (fatal):
- EmptyDashboard: components empty.
- Per gauge: non-empty output_channel is added to unique set; if channel set provided and it is absent (exact, case-sensitive) -> UnknownOutputChannel(gauge_id, channel). `min >= max` -> InvalidRange. background_image_file_name or needle_image_file_name set but no embedded image with that `file_name` -> MissingEmbeddedImage.
- Per indicator: unknown channel -> UnknownIndicatorChannel.
- UnsupportedPainter enum exists but is never emitted.
Warnings: TinyGauge if rel width<0.05 or height<0.05; OutOfBounds if x+w>1.0 or y+h>1.0; OverlappingGauges for every gauge pair i<j (indicators excluded) where `!(a.x+a.w < b.x || b.x+b.w < a.x || a.y+a.h < b.y || b.y+b.h < a.y)` (note: touching edges count as overlap); PerformanceWarning when components.len() > 50.
Stats: gauge_count, indicator_count, unique_channels, embedded_image_count, has_embedded_fonts (any resource_type==ttf). `is_valid = errors empty`.
(Overlap warnings can be O(n^2); fine.)

---
## 4. INI `[GaugeConfigurations]` (ini/gauges.rs)
```c
typedef struct { char *name,*channel,*title,*units; double low_warning,low_danger,high_warning,high_danger,lo,hi; unsigned char digits;
                 char *lo_expr,*hi_expr,*low_danger_expr,*low_warning_expr,*high_warning_expr,*high_danger_expr; } lt_ini_gauge;
```
Defaults: all strings "", low_warning 0, low_danger 0, high_warning 100, high_danger 100, lo 0, hi 100, digits 0, exprs NULL.
Line `name = channel, "title", "units", lo, hi, loD, loW, hiW, hiD, digits` split with the INI field splitter (`split_ini_line`: comma-separated, honors quotes and `{...}` braces so a braced title may contain commas; see spec 01). Field order: [0] channel (required, non-empty else NULL return; strip `"`), [1] title, [2] units (strip surrounding `"` via trim_matches - strips all leading/trailing quote chars), [3] lo, [4] hi, [5] low_danger, [6] low_warning, [7] high_warning, [8] high_danger, [9] digits (u8, parse failure 0). Extra fields ignored. Missing fields keep defaults.
Numeric field: if parses as f64 -> value, expr NULL. Else if trimmed text is `{ inner }` -> value=fallback (lo 0, hi 100, loD 0, loW 0, hiW 100, hiD 100) and expr = trim(inner). Else fallback, no expr. Callers resolve exprs later against tune/default values.
Zone predicates: `is_danger(v) = v<=low_danger || v>=high_danger`; `is_warning(v) = (v<=low_warning && v>low_danger) || (v>=high_warning && v<high_danger)`; `is_normal(v) = v>low_warning && v<high_warning`.
Tests: line `rpm, "Engine Speed", "RPM", 0, 8000, 300, 600, 6500, 7000, 0` -> channel rpm, title "Engine Speed", hi 8000. Speeduino tach `rpm, "Engine Speed", "RPM", 0, {rpmhigh}, 300, 600, {rpmwarn}, {rpmdang}, 0, 0` -> hi_expr "rpmhigh", high_warning_expr "rpmwarn", high_danger_expr "rpmdang", lo_expr/low_danger_expr NULL, lo 0, low_danger 300. Line `gppwmGauge1 = gppwmOutput1, { bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }, "%", 0, 100, 0, 0, 100, 100, 1` -> title `{ bitStringValue(pwmAxisLabels, gppwm1_loadAxis) }`, units "%", hi 100, digits 1. Zones with low_danger 500, low_warning 800, high_warning 6500, high_danger 7000: danger(400), danger(7500), warning(600), warning(6800), normal(3000).

---
## 5. Runtime behavior shared by all painters

### 5.1 Layout
Component pixel rect = cluster_pixel_size * (relative_x, relative_y, relative_width, relative_height). `shortest_size` (px, default 50) is a minimum-size floor applied by the host layout (templates set 0 for percent-driven tiles). `force_aspect` with force_aspect_width/height (e.g. 16:10) letterboxes the cluster to that aspect ratio. Cluster background: cluster_background_color fill, optional image per style (Tile/Stretch/Center/Fit). enabled_condition false -> skip component.

### 5.2 Value smoothing, peak hold, spikes
- Display value EMA: `alpha = 1 - exp(-dt/tau)`, tau=120 ms; dt<=0 returns prev; dt clamped to 100 ms. `v += (target-v)*alpha`. Redraw cap default 30 Hz (allowed 10,15,20,25,30); each gauge phase-offset by `slot*interval/16`, slot = djb2(channel) mod 16 (h=5381; h=h*33+c, int32 wrap, abs) (no channel: incrementing counter mod 16).
- Spike: raw vs smoothed `|raw-smoothed| > 0.15*(max-min)` (range<=0 never) -> flash value text in critical_color for 400 ms after last spike.
- Peak hold (only if `peak_hold`): peak seeded = clamp(history_value, min, max) (else current value). Per frame: first frame starts decay clock; if target>peak -> peak=target, clock=now; else if history_delay>0 and now-clock>=history_delay -> peak=target, clock=now; history_delay<=0 holds forever. Marker drawn only by analog dial: when peak>min.
- Value color `get_value_color(v)` with hysteresis h (default 0) and remembered previous zone prev in {normal,warn,critical}: critical if (high_critical set and v >= high_critical - (prev==critical?h:0)) or (low_critical set and v <= low_critical + (prev==critical?h:0)); else warn if (high_warning and v >= high_warning - (prev in {warn,critical}?h:0)) or (low_warning and v <= low_warning + (same)); zone saved as prev; returns critical_color / warn_color / font_color. Uses display (smoothed) value.
- Thresholds with VP names (`*_VP`, `MinVP`,`MaxVP`) are re-resolved from ECU/INI data by host (`useGaugeRangeSync`); not part of the file semantic beyond being stored.

### 5.3 Helpers (exact)
- `lighten(hex,p)`: amt=round(2.55*p); each channel min(255, c+amt). `darken`: max(0,c-amt). Negative p works as the opposite (`lighten(c,-60)` = darken by 153 = dimmed segment). Alpha dropped (opaque).
- Metallic gradient (radial, center offset (-0.3*r2,-0.3*r2) of the gauge center, inner r1, outer r2) with stops: 0:lighten60, .3:lighten30, .5:base, .7:darken20, 1:darken40, base = trim_color. Without gradient support in C: approximate as concentric rings or flat shade by interpolating these 5 stops by normalized distance from the offset center.
- `steady_value_text(v,digits,min,max)`: pad-left with spaces to max(len(fmt(min)),len(fmt(max))) of `%.{digits}f` (only if user setting gauge_right_align_values).
- roundRect uses quadratic corners; `toFixed(n)` == `%.nf` (note JS rounds half away on decimal repr; C printf uses exact binary - minor).
- Fonts: UI has family (config.font_family or default sans stack "Arial, Helvetica, sans-serif"), `bold`, `monospace` options. C port: map to a built-in bitmap/vector font; sizes below are pixels (CSS px).
- Canvas arc angle convention: radians, 0 = +x (right), +PI/2 = down (screen y down), increasing clockwise. Point = (cx+cos a*r, cy+sin a*r). `ccw` flag reverses sweep direction.
- Shadows (shadowBlur) are cosmetic; a C renderer may skip them or draw a darker offset copy.

### 5.4 Gauge arc geometry `resolve_gauge_arc(cfg)` (used by analog dial)
```
startDeg = sweep_begin_degree  (parser always yields a value; fallback chain: sweep_begin_degree ?? start_angle ?? 225)
requestedSweep = isfinite(sweep_angle) ? sweep_angle : 270
face = (absent|NaN|<45) ? 360 : min(face_angle, 360)
sweepDeg = max(1, min(requestedSweep, face))
ccw = counter_clockwise
sweepMid = ccw ? startDeg - sweepDeg/2 : startDeg + sweepDeg/2
faceStartDeg = sweepMid - face/2
isFullCircle = face >= 360
```
Face path: full circle, or pie wedge (moveTo center, arc faceStart..faceStart+face, close). `faceOppositeDirection = (-cos(mid), -sin(mid))` with mid = faceStart + face/2 (radians) - where sector-face text is anchored.
Tests (gaugeGeometry.test.ts shapes): FaceAngle 360 full circle; FaceAngle 180 with SweepAngle absent (270 default) -> sweep clamped to 180; face 182 w/ sweep 180 -> faceStart = mid-91 (1 deg margin each side); face <45 -> treated 360; face >360 -> 360.

---
## 6. Painters (canvas size W x H in pixels; `v`=display value; pct(v)=(v-min)/(max-min); `clamp01`)
Notation: P = padding, TS(c)=trim_color, FC=font_color, NC=needle_color, BC=back_color; gradients "vertical g(a,b)" = top a -> bottom b. If range max==min, guard pct=0 (original divides -> NaN/Inf; C must guard).
Registration/dispatch: map painter enum -> fn; unknown -> BasicReadout. AnalogGauge, BasicAnalogGauge, CircleAnalogGauge share the analog dial painter.

### 6.1 BasicReadout (digital LCD)
P=6, innerW=W-12, innerH=H-12, corner=min(8, W*.05), minDim=min(W,H), fontScale=1+font_size_adjustment*0.1. If legacy-mode bg image: draw it stretched. Else: frame roundRect(0,0,W,H,corner) gradient diagonal #555 -> #333 (.5) -> #222; inset panel roundRect(P-2,P-2,innerW+4,innerH+4,corner-2) filled BC with shadow; then LCD panel roundRect(P,P,innerW,innerH,corner-2) vertical gradient lighten(BC,5)->darken(BC,10). Title: size max(9,minDim*.12*fs), color TS, centered top at y=P+2. Value: size max(14,minDim*.35*fs) bold monospace, color = spike? critical : valueColor (glow if != font_color), centered at (W/2, H/2+titleSize*.3), `%.{value_digits}f`; if rightAlign: right edge x=W-P-6. Units: size max(8,minDim*.10*fs), TS, centered, bottom at H-P-2.

### 6.2 HorizontalBarGauge
P=6; barH=H*.35; barY=(H-barH)/2+H*.08; barW=W-12; r=min(4,barH*.3). Background fillRect gradient lighten(BC,10)->darken(BC,15). Title (left x=P, top y=3) size max(9,H*.14) TS. Bar track roundRect gradient #252525,.5:#404040,#303030. Fill width = barW*clamp01(pct): roundRect gradient (valueColor): 0 lighten30, .3 lighten10, .7 base, 1 darken20; highlight rect rgba(255,255,255,.15) at (P+2,barY+2,fillW-4,barH*.3). Track border 1px TS. Value text `"%.nf units"` right-aligned at (W-P, 3) size max(11,H*.18) bold mono, color FC.

### 6.3 VerticalBarGauge
P=6; labelH=H*.12; barW=W*.45; barH=H-2*labelH-3P; barX=(W-barW)/2; barY=labelH+1.5P; r=min(4,barW*.15). Horizontal bg gradient lighten5/BC/darken10. Title centered top y=3, size max(9,labelH*.75) bold, TS. Track horizontal gradient #202020,.3 #383838,.7 #383838,#282828. Fill from bottom: height barH*clamp01(pct); horizontal gradient darken15,.3 lighten15,.7 lighten10,1 darken10 of valueColor; highlight rgba(255,255,255,.12) rect (barX+3, top+2, barW*.35, fillH-4). Border 1px TS. Ticks: 6 ticks (i=0..5) at barY+barH-barH*i/5, from x=barX+barW+2 to +6, TS. Value at bottom (y=H-2, baseline bottom) size max(11,labelH*.9) bold mono, FC, centered (or right x=W-4); no units.

### 6.4 HorizontalLineGauge
P=8; lineY=H*.55; lineW=W-16. bg vertical lighten8->darken8. Title left (P,3) size max(9,H*.16) bold TS. Track: stroke width 6 round caps, gradient #252525/#404040/#353535 from (P,lineY) to (P+lineW,lineY). Fill: stroke width 4 from P to P+lineW*clamp01(pct), gradient darken10->lighten10 of valueColor. Indicator dot at x=P+fillLen: radius 7, radial gradient (offset -2,-2; r 0..8): lighten40, .5 base, darken20; 1px stroke rgba(255,255,255,.3); glow = valueColor. Value text right (W-P,3) "%.nf units" size max(11,H*.22) bold mono FC. Min/max labels (`%.0f`) at y=lineY+8, left x=P / right x=W-P, size max(8,H*.12) TS.

### 6.5 HorizontalDashedBar (14 segments) / VerticalDashedBar (12 segments)
Horizontal: P=6; titleH=H*.15; valueH=H*.15; barH=H*.35; barW=W-12; barY=titleH+(H-titleH-valueH-barH)/2; segW=barW/14; gap=3. bg vertical gradient darken5/lighten5/darken5. Title centered top y=2 size max(9,titleH*.6) bold TS.
Vertical: P=6; labelH=H*.1; barW=W*.5; barH=H-2P-2*labelH; barX=(W-barW)/2; barY=labelH+P; segH=barH/12; gap 3; segment i is drawn at y=barY+(11-i)*segH (i=0 at bottom). Title centered top y=2, size max(9,labelH*.8).
Both: filled = ceil(pct*N) (can exceed N or be negative: compare i<filled). Segment i: segPct=i/N; segVal=min+segPct*range. Color: if high_critical set and segVal>=high_critical -> filled? critical : #401010; elif high_warning set and segVal>=high_warning -> filled? warn : #403010; else filled? NC : #303030. Filled segments: gradient across segment (horizontal for H-bar: darken15, .3 lighten20, .7 lighten15, 1 darken10); unfilled flat. roundRect(segX+gap/2, barY, segW-gap, barH, 2) (vertical: roundRect(barX, segY+gap/2, barW, segH-gap, 2)). Last filled segment (H-bar) gets glow. Value at bottom (y=H-2) "%.nf" no units, size max(11,valueH*.6 (H) | labelH*.9 (V)) bold mono FC.

### 6.6 Histogram (decorative bell curve; no real histogram data)
P=8; titleH=H*.12; valueH=H*.1; graphW=W-16; graphH=H-titleH-valueH-16; graphY=titleH+8; N=20 bars; barW=(graphW-19*2)/20 (gap 2). bg vertical lighten5->darken10. Title left (P,3) size max(9,titleH*.8) bold TS. Graph panel roundRect(P-2,graphY-2,graphW+4,graphH+4,4) fill #1a1a1a. Grid lines i=1..3 at graphY+graphH*i/4, rgba(80,80,80,.3). centerBar=floor(pct*20); bar i height = graphH*max(0.05, exp(-(i-centerBar)^2/20)); bar x=P+i*(barW+2); color by barPct=i/20: >= pct(high_critical) -> critical; >= pct(high_warning) -> warn; else NC; vertical gradient lighten25/base/darken15; roundRect r=2. Marker: dashed line (dash 4 on, 2 off) width 2 color NC at x=P+pct*graphW from graphY to graphY+graphH. Value right (W-P,3) "%.nf units" size max(10,valueH*.9) bold mono FC.

### 6.7 LineGraph (scrolling time series)
P=8; titleH=H*.12; graphW=W-16; graphH=H-titleH-16; graphY=titleH+8. bg vertical lighten5->darken10. Title left size max(9,titleH*.8) bold TS; value text right "%.nf units" size max(10,titleH*.9) bold mono FC. Panel/grid as histogram (#1a1a1a, grid rgba(80,80,80,.3)).
Data: host keeps per-channel history ring buffer of raw samples (one per ~100 ms stream tick; length chosen by host, e.g. 300-600). If history non-empty: EMA-smooth with tau=250ms, dt=100ms (first sample = raw); sample i x=P+graphW*i/(n-1), y=graphY+graphH-clamp01((s-min)/range)*graphH. Spike dots: for i>0, if |raw_i - ema_i| > 0.15*range, draw circle r=2.5 in critical_color at RAW position. If no history: 50 points flat at clamp01(pct(value)). Draw filled area under line (polygon baseline graphY+graphH) with vertical gradient valueColor alpha 0x60 -> 0x10; line width 2 round joins color valueColor; dot r=4 at last point lighten30 with glow; min/max labels `%.0f` at (P+2, graphY+2 top) = max and (P+2, graphY+graphH-2 bottom)=min, size max(7,graphH*.08) TS. If points empty return. (n==1 history: division by zero in original; guard.)
Tests (lineGraph.test.ts, 286 lines): flat-line when no history, spike dot placement, EMA of history; port by constructing a fake framebuffer and checking points.

### 6.8 MultiChannelTrend (LibreTune-native)
Series: primary = output_channel, label = extra_attrs.lt_series1_label || output_channel || title || "Series 1", color = font_color, min/max = gauge min/max. Extra series for n=2..12: needs `lt_seriesN_channel` non-empty; label `lt_seriesN_label`||channel; color `lt_seriesN_color` (CSS hex) default #94a3b8; min `lt_seriesN_min` (default 0), max `lt_seriesN_max` (default 100) via strtod.
Layout: P=6; titleH=max(14,H*.08); rowH=max(11,H*.045); cols=max(2,floor((W-12)/110)); rows=max(1,ceil(nseries/cols)); legendH=rows*rowH+4; graphW=W-12; graphY=titleH+legendH+3; graphH=H-graphY-6. bg vertical lighten4->darken8. Title UPPERCASE left (P,2) size max(9,titleH*.85) bold TS. Legend cell i: col=i%cols,row=i/cols, x=P+col*(W-12)/cols, y=titleH+row*rowH+rowH/2+2; dot radius max(2,fs*.22) (fs=max(7,rowH*.65)); text `"label %.1f"` (live value; series 0 uses gauge value; others latest realtime channel value case-insensitive lookup else series min) in series color, bold mono. Panel fill #141414 roundRect r4; grid rgba(255,255,255,.08) 3 lines. Plot series from last to first (primary on top): points normalized per-series ((v-min)/(max-min||1) clamp01) across history; no history/<=1 sample -> 50 flat points at live value. line width: n>6:1.1, n>3:1.4, else primary 2 / other 1.6; if primary and n<=3 width 2.25; alpha 1 primary, .85 others; primary glow. Primary gets a dot r=3.5 lighten30 at its last point.

### 6.9 TelemetryStat (LibreTune-native flat tile)
compact = H<42 || H < W*.45. corner = compact?2:min(6,W*.04,H*.08). accentW = compact?0:max(3,W*.025). P = compact? max(4,W*.04) : max(8,W*.06). Panel roundRect gradient BC -> darken(BC,6). Non-compact: left accent stripe width accentW color NC (clipped to round rect). contentX=accentW+P*(compact?.4:.6). minDim=min(W,H); fs=1+font_size_adjustment*.1; valueText=`%.{value_digits}f`.
Compact: label = UPPERCASE(title) at (contentX,H/2) left/middle size max(7,minDim*.22*fs) bold TS; value right-aligned at (W-P,H/2) text `value units` (units omitted if empty) size max(9,minDim*.34*fs) bold mono color valueColor (glow if != font_color); return.
Full: label uppercase, letter-spaced (per-char advance + max(1,size*.12)), left top at (contentX, P*.55), size max(9,minDim*.13*fs) bold TS. Value baseline y=H*.68 at x=contentX, size max(16,minDim*.40*fs) bold mono, valueColor (glow blur 10 if != font_color). Units (if any) at x=contentX+valueWidth+unitsSize*.4 baseline same, size max(8,minDim*.12*fs) bold TS. Range bar: barH=max(3,H*.06), barY=H-P*.5-barH, barX=contentX, barW=W-contentX-P*.5; if barW>4: track rgba(255,255,255,.08) pill; fill pill width max(barH,barW*pct) valueColor if pct>0 (pct=0 if range==0); tick marks 1px wide, height barH+4 starting barY-2 at pct of low_warning/high_warning (warn_color) and low_critical/high_critical (critical_color) when set.

### 6.10 AnalogGauge dial (also BasicAnalog, CircleAnalog) - the main dial
size=min(W,H); cx=W/2; cy=H/2; radius=size/2-8; arc=resolve_gauge_arc. Background: if bg image -> draw size x size centered (clipped to face wedge if sector) else fill BC over face path. Then shadowed base fill #333 over face path. Bezel: bezelW = border_width>0 ? min(radius*.3, border_width) : max(6, radius*.08); metallic gradient (trim_color); full circle -> ring between radius and radius-bezelW; sector -> stroke arc at radius-bezelW/2 width bezelW along face arc. Inner highlight arc at radius-bezelW+1, 1px rgba(255,255,255,.15). faceR=radius-bezelW-2; face filled with radial gradient (center offset -0.3*faceR, r 0..faceR): 0 lighten(BC,15), .7 BC, 1 darken(BC,10).
angleAt(p) = startRad + p*sweepRad (minus if ccw); start=arc.startDeg, sweep=arc.sweepDeg in radians.
Zones (behind ticks, at zoneR=faceR-4, width max(4,faceR*.06), butt): if high_warning set: arc from angleAt(pct(high_warning)) to end of sweep in warn_color; then if high_critical set same in critical_color. (Low zones are NOT drawn by the dial.)
Ticks: tickR=faceR-max(10,faceR*.12); major = major_ticks>0 ? major_ticks : (max-min)/10; nMajor=floor((max-min)/major)+1; minorPer = minor_ticks>0?minor_ticks:0. Minor ticks (color darken(TS,30), 1px): total=(nMajor-1)*minorPer; for i in 0..total-1 skipping i%minorPer==0: p=i/total; line from tickR-faceR*.05 to tickR. Major ticks (TS, 2px): value=min+i*major; line from tickR-faceR*.10 to tickR; label `%.{label_digits}f` color FC size max(8,faceR*.14) at radius tickR-faceR*.22; if radius<70 only first and last labels. 
Needle: pct=(v-min)/(max-min) (NOT clamped here; the display value is already limited by PegLimits semantics in host: clamp to [min,max] when peg_limits). length: needle_length in (0,1.5] -> faceR*len; >1.5 -> min(faceR,len px); else faceR*.35. width=max(3,faceR*.04). Triangle in rotated frame (rotate by needle angle about center): (-len*.08,-w),(len,0),(-len*.08,+w); gradient across y: lighten30/NC/darken20. Custom needle image: scale=len/imgW, drawn centered with offsets needle_image_offset_x/y. Center cap radius max(6,faceR*.1) radial gradient #aaa/#666/#444, 1px #333 stroke.
Peak marker (peak_hold && peak>min): clamp peak; wedge in rotated frame points (0.78*faceR,0),(0.94*faceR,-0.94*faceR*.04),(0.94*faceR,+...); color critical if beyond high/low_critical, warn if beyond warning, else TS.
Text: full circle: title centered top at y=cy+faceR*.25, size max(9,faceR*.13) bold FC; value box: text `"%.nf units"` size max(11,faceR*.16) bold mono at y=cy+faceR*.55, with backing roundRect rgba(0,0,0,.3) (padding 4 x, 2 y, r 3). Sector face: anchored along faceOppositeDirection d: title at center + d*faceR*.1; value at center + d*faceR*.32.

### 6.11 AsymmetricSweepGauge (sweep arc with glowing tip)
cx=W/2+(needle_pivot_offset_x||0); cy=H*.58+(needle_pivot_offset_y||0); radius=min(W,H*1.15)/2-8; arcW=max(16,radius*.18). bg vertical lighten8->darken12. Angles: startDeg=sweep_begin_degree (fallback start_angle, 210), sweepDeg=sweep_angle (fallback 120) - NOTE does NOT clamp by face_angle. Track: round-cap arc width arcW gradient #252525/#404040/#303030. Zones: high_warning arc to end (warn color, width arcW-4, butt), then high_critical (critical). Fill (if pct clamp01 >0.01): arc start->valueAngle width arcW-4 round cap, gradient from start point to tip: darken10, .5 lighten15, 1 valueColor; tip circle r=arcW/4 lighten20 at valueAngle on the arc. Ticks: major=major_ticks>0?major_ticks:(max-min)/5; n=floor(range/major)+1; tick i at pct i/(n-1) from radius+arcW/2 to +4 (TS, 2px); label `%.{label_digits}f` at +10 beyond (size max(8,radius*.1), FC) if radius>60 or first/last. Minor ticks like dial (per major, 1px TS, inside tick span +2/-2). Center text: value size max(18,radius*.28) bold mono FC at cy (cy+radius*.25 if display_value_at_180); title size max(10,radius*.11) bold at cy+radius*.35; units size max(9,radius*.09) TS at cy+radius*.5.

### 6.12 RoundGauge (segment ring) / RoundDashedGauge
Both: P=min(W,H)*.08; cx,cy=center; radius=min(W,H)/2-P.
RoundGauge: ring between radius and radius*0.88 (ringW=radius*.12) metallic(trim); inner disc BC radius-ringW. 60 annular segments from innerR=radius*.55 to outerR=radius*.85; segment i spans angle [i/60*2pi - pi/2, (i+1)/60*2pi - pi/2 - 1deg]; segVal=min+(i/60)*range; color: TS, but segVal>=max-0.1*range -> critical, elif >=max-0.25*range -> warn (hard-coded 10%/25% zones, NOT thresholds); segVal>v -> lighten(color,-60). Center value size max(12,radius*.25) bold mono valueColor (steady if rightAlign); units (if any) size*.5 FC at cy+size*.6; title (if any) size*.4 FC top at y=4.
RoundDashedGauge: shadowed metallic disc radius; inner disc radius*.9 BC. 30 radial dashes (round caps, width radius*.08) from innerR=radius*.65 to outerR=radius*.85 at angle 135deg + i/29*270deg, same 10/25% zone colors; lit if segVal<=v (glow) else lighten(c,-60). Value size max(12,radius*.28); units size*.45 at cy+size*.7; title size*.35 top y=4.

### 6.13 Tachometer
P=min(W,H)*.08; radius=min(W,H)/2-P. Disc radius metallic(trim), inner disc radius*.93 #1a1a1a. Sweep: start 135 deg (0.75pi), total 270 deg (fixed; ignores config angles). Majors: n=ceil(max/1000); for i in 0..n: tickVal=i*1000; skip if >max; angle=start+270deg*(tickVal-min)/(max-min); line radius*.72 -> radius*.85 width 2; color critical if tickVal >= 0.85*max else white; label = String(i) (thousands) at radius*.58 size radius*.12 bold same color. Minor ticks: count=n*5; for i<count: tickVal=min+i/count*range; skip if tickVal%1000==0; line from radius*.72+(.13r*.5) to radius*.85, #666 1px. Redline arc radius*.88 from 0.85*max to end, width radius*.05 critical color. Needle: length radius*.65, half-width radius*.03, polygon (-0.15L,0),(L,0),(-0.15L,-w),(-0.15L,w) rotated, gradient #ff4444/#ff0000/#aa0000 (ignores needle_color). Hub radius radius*.08 radial #888/#444/#222. Digital value `%.0f` size max(10,radius*.16) bold mono valueColor at cy+radius*.35; caption "RPM x 1000" (U+00D7) size*.6 #888 at cy+radius*.52; title (if any) size*.5 FC top y=4.

### 6.14 AnalogBarGauge (semicircular bar)
cx=W/2; cy=H*.85; radius=min(W,H)*.75; barW=radius*.15; 180 deg arc from pi (left) to 0 (right) (counter-clockwise in screen terms: angle = pi - p*pi). Bezel: annulus between radius+barW/2+4 and radius-barW/2-4, metallic(trim). Track: stroke arc width barW color darken(BC,30). Value arc (if pct>0, clamp01): stroke width barW-4 round cap from pi to value angle, gradient darken20/valueColor/lighten20. Ticks: 11 (i=0..10), angle pi-i/10*pi; inner=radius-barW/2-8; outer=radius-barW/2-15; major (even i) from outer to inner, minor from inner+3 to inner; TS 1px. Value text size max(12,radius*.2) bold mono valueColor at cy-radius*.3; units size*.5 FC at cy-radius*.1; title size*.5 top y=4.

### 6.15 AnalogMovingBarGauge (140 deg arc, needle + zones)
cx=W/2; cy=H*.9; radius=min(W,H)*.8; barW=radius*.08; start=pi+0.2pi, end=-0.2pi, total=start-end (=1.4pi... i.e. 252 deg; comment says 140). angle(p)=start-p*total. Bezel stroke at radius+6 width 12 metallic. Dark band annulus radius-barW .. radius-3barW fill darken(BC,40). Zones on radius-2barW width barW butt: warnStart = high_warning ?? min+0.7*range; dangerStart = high_critical ?? min+0.9*range; green (NC-based normal) from min to warnStart, warn_color warnStart..dangerStart, critical dangerStart..max (the file draws these three via helper `drawZone(startVal,endVal,color)` clamping percents). Value trail arc radius-barW*.5 width barW*.8 round cap with gradient (valueColor) from start to value angle. Needle: length radius*.85, half-width 4, polygon (L,0),(0,-4),(-0.1L,0),(0,4) rotated by value angle, gradient #ff4444/#ff0000/#aa0000. Center cap r=10 radial #666/#444/#222. Value size max(14,radius*.18) bold mono valueColor at cy-radius*.35; units size*.6 FC at cy-radius*.18; title size*.5 top y=4.

### 6.16 FuelMeter
P=min(W,H)*.1; radius=min(W,H)/2-P. Metallic disc; inner disc radius*.88 #0a0a0a. Arc radius*.7, start 0.8pi, sweep 1.4pi; background stroke #333 width radius*.12 round; filled stroke to start+sweep*pct (pct UNCLAMPED in original; clamp in C) with horizontal gradient (cx-arcR -> cx+arcR): 0 critical, .25 warn, .5 trim, 1 trim. "E" at angle start+0.05pi, "F" at start+sweep-0.05pi, radius*.55, white bold size radius*.18. Pump glyph (U+26FD) at cy-radius*.15 (C port: draw small rectangle+hose primitive instead). Value `"%.nf" + (units||"%")` size max(10,radius*.2) bold mono valueColor at cy+radius*.35. Title size*.5 FC top y=4.

### 6.17 Indicators (TsIndicator)
value on = value != 0 (host sets from channel). Colors: bg = on? on_background_color : off_background_color; text = on? on_text_color : off_text_color; text string on_text/off_text. If the matching image (on/off_image_file_name) is an embedded image -> draw it contained (aspect-fit) instead. BulbIndicator: circle 80% of min dimension centered, radial gradient highlight at 30%,30% from rgba(255,255,255,.7) to bulbColor (on: bg color; off: rgba(20,20,20,.9)); on => outer glow 12px; off => inner dark shadow; 1px rgba(255,255,255,.2) border; no text. Rectangle (and LED fallback): fill bg, 1px rgba(100,100,100,.5) border, radius 2, centered text clamp(7px, 1.8vmin, 12px) weight 500 nowrap/ellipsis. Italic via italic_font; font via font_family (web-safe mapping e.g. Courier New -> monospace; unknown -> `"name", Arial`). Indicator's LED painter has no distinct rendering in the source (renders as rectangle).

---
## 7. Default dashboard templates (exact; positions are relative fractions)
Common colors (R,G,B): DARKER_BG 12,14,20; GAUGE_BG 28,32,40; ACCENT_BLUE 74,158,248; TEAL 56,189,248; AMBER 251,191,36; GREEN 34,197,94; RED 239,68,68; TEXT_PRIMARY 255,255,255; TEXT_SECONDARY 148,163,184; WARN 234,179,8; CRITICAL 239,68,68. All gauges in the standard templates: back_color GAUGE_BG, trim TEXT_SECONDARY, warn WARN, critical CRITICAL, other fields default. Cluster: antiAliasing true, forceAspect false, bg DARKER_BG, image style Stretch, file_format "3.0", author "LibreTune"/"LibreTune Project", write_date `%Y-%m-%d`.
Notation: id/channel title units min..max painter | x,y,w,h | font/needle | thresholds | digits | extras.

### 7.1 `lt_dash_template_basic()` (create_basic_dashboard) 10 gauges
1. rpm/rpm "ENGINE RPM" "" 0..8000 Tachometer | .02,.10,.45,.80 | font WHITE, needle AMBER | hiW 6500 hiC 7200 | digits 0 | start 135 sweep 270 major 8 minor 4, fontSizeAdj 2
2. afr/afr "AIR/FUEL RATIO" ":1" 10..20 AnalogGauge | .52,.10,.46,.80 | font GREEN needle GREEN | loW 11.5 loC 10.5 hiW 16 | digits 1 | start 225 sweep 270 major 10 minor 5, adj 1
3. coolant/coolant "COOLANT" "°C" -40..120 HorizontalBar | .02,.02,.23,.06 | font+needle BLUE | hiW 100 hiC 110 | d0
4. map/map "MAP" kPa 0..250 HBar | .27,.02,.23,.06 | TEAL | hiW 200
5. tps/tps "THROTTLE" % 0..100 HBar | .52,.02,.23,.06 | AMBER
6. battery/battery "BATT" V 10..16 BasicReadout | .77,.02,.21,.06 | font WHITE needle GREEN | loW 11.5 loC 11 | d1 | adj -1
7. iat/iat "INTAKE TEMP" °C -40..80 BasicReadout | .02,.92,.23,.06 | WHITE/AMBER | hiW 50 | adj -1
8. advance/advance "TIMING" ° -10..50 BasicReadout | .27,.92,.23,.06 | WHITE/TEAL | d1 | adj -1
9. ve/ve "VE" % 0..150 BasicReadout | .52,.92,.23,.06 | WHITE/GREEN | adj -1
10. pw/pulseWidth "PULSE" ms 0..25 BasicReadout | .77,.92,.21,.06 | WHITE/BLUE | d2 | adj -1

### 7.2 `lt_dash_template_tuning()` (create_tuning_dashboard) 15 gauges, bg DARKER_BG
rpm/rpm "RPM" 0..8000 AsymmetricSweepGauge .02,.02,.30,.30 (start 180 sweep 180, hiW 6500 hiC 7200, font WHITE needle TEAL);
afr/afr "AFR" ":1" 10..20 RoundGauge .34,.02,.30,.30 (loW 11.5 loC 10.5 hiW 16, d2, GREEN, start 135 sweep 270 major 10 minor 5);
map "MAP" kPa 0..250 HBar .66,.02,.32,.12 (hiW 200, TEAL); tps "TPS" % 0..100 HBar .66,.16,.32,.12 (AMBER);
coolant "COOLANT" °C -40..120 VerticalBar .02,.35,.14,.38 (hiW 100 hiC 110, BLUE); iat "IAT" °C -40..80 VerticalBar .18,.35,.14,.38 (hiW 50, AMBER);
lambda_hist/lambda "LAMBDA TREND" "λ" 0.7..1.3 LineGraph .34,.35,.64,.38 (loW .75 hiW 1.1, d3, GREEN);
Readouts (all BasicReadout, font WHITE unless noted, adj -1): ve "VE" % 0..150 .02,.76,.14,.11 (needle GREEN); pw/pulseWidth "PULSE" ms 0..25 .18,.76,.14,.11 (hiW 20,d2,BLUE); duty/dutyCycle "DUTY" % 0..100 .34,.76,.14,.11 (hiW 85 hiC 95, AMBER); advance "TIMING" ° -10..50 .50,.76,.14,.11 (d1,TEAL); battery "BATT" V 10..16 .66,.76,.14,.11 (loW 11.5 loC 11,d1,GREEN); egt "EGT" °C 0..1000 .82,.76,.16,.11 (hiW 850 hiC 950, RED); afrtarget/afrTarget "AFR TARGET" ":1" 10..20 .02,.89,.14,.09 (d1, font TEXT_SECONDARY); corr/correction "CORRECTION" % 0..200 .18,.89,.14,.09 (font+needle+trim TEXT_SECONDARY).

### 7.3 `lt_dash_template_telemetry_live()` (also exposed as `create_f1_telemetry_dashboard` alias) - dense Grafana-style
Cluster: forceAspect true 16:10, bg LOG_BG (15,17,20), Stretch. Palette: PANEL_BG 26,28,33; WHITE 230,230,235; GRAY 140,145,155; BLUE 87,148,242 (#5794f2); GREEN 115,191,105; ORANGE 255,152,48; RED 242,73,92; PURPLE 184,119,217; YELLOW 250,222,42; CYAN 51,178,255.
All tiles: back_color PANEL_BG, font_color=needle_color=<tile color>, trim GRAY, warn WARN, critical CRITICAL, border_width 1, **shortest_size 0** (required by test), font_size_adjustment -1 for stat tiles only (TelemetryStat), LineGraph sparklines default adj.
Top KPI strip (TelemetryStat, y .005, h .048, w .095 except last .090): tl_rpm RPM/rpm 0..9000 d0 x.005 BLUE; tl_map MAP/map kPa 0..250 x.105 CYAN; tl_tps TPS/tps % 0..100 .205 ORANGE; tl_afr AFR/afr ":1" 10..20 d1 .305 GREEN; tl_lam LAM/lambda "λ" .7..1.3 d3 .405 GREEN; tl_clt CLT/coolant °C -20..120 .505 BLUE; tl_iat IAT/iat °C -20..80 .605 ORANGE; tl_spd SPD/speed km/h 0..260 .705 WHITE; tl_bat BATT/battery V 10..16 d1 .805 YELLOW; tl_duty DUTY/dutyCycle % 0..100 .905 PURPLE (w .090).
Left column (TelemetryStat, x .005, w .118, h .034, y = .060 + .037*i): VE/ve % 0..150 GREEN; PW/pulseWidth ms 0..25 d2 BLUE; ADV/advance ° -10..50 d1 CYAN; BOOST/boost kPa 0..300 ORANGE; BARO/baro kPa 70..110 GRAY; OIL P/oilPressure kPa 0..700 YELLOW; OIL T/oilTemp °C 0..150 ORANGE; EGT/egt °C 0..1000 RED; CORR/correction % 50..150 GRAY; SYNC/sync "" 0..1 GREEN; FUEL/fuelLevel % 0..100 YELLOW; AFR TGT/afrTarget ":1" 10..20 d1 GRAY (ids tl_ve,tl_pw,tl_adv,tl_bst,tl_baro,tl_oilp,tl_oilt,tl_egt,tl_corr,tl_sync,tl_fuel,tl_tgt).
Trend panels (MultiChannelTrend, d1; primary color = css of primary entry; series spec channel/label/color/min/max; extra series fill lt_seriesN_* for N=2.., primary label via lt_series1_label):
- tl_trend_engine "ENGINE DYNAMICS" rect .130,.060,.430,.210: rpm RPM #5794f2 0..9000 (primary); map MAP #33b5ff 0..250; tps TPS #ff9830 0..100; speed SPD #e6e6eb 0..260; dutyCycle DUTY #b877d9 0..100; advance ADV (color/range per INI: -10..50); boost BOOST 0..300; ve VE 0..150 (colors for last three not captured above; use CYAN #33b2ff? -> pick distinct: ADV #33b2ff, BOOST #ff9830? - port may choose any distinct palette; they do not affect tests).
- tl_trend_fuel "FUEL & MIXTURE" (title approximate) .570,.060,.425,.210: afr AFR #73bf69 10..20 (primary); lambda LAM #56a64b .7..1.3; pulseWidth PW #5794f2 0..25; ve VE #fade2a 0..150; correction CORR #8e8e93 50..150; afrTarget TGT 10..20.
- tl_trend_temp "TEMPERATURES" .130,.278,.430,.223: coolant CLT #5794f2 -20..120 (primary); iat IAT #ff9830 -20..80; egt EGT #f2495c 0..1000; oilTemp OILT #fade2a 0..150.
- tl_trend_pressure "PRESSURES" (title approximate) .570,.278,.425,.223: map MAP #33b5ff 0..250 (primary); baro BARO #8e8e93 70..110; oilPressure OILP #fade2a 0..700; boost BOOST #f2495c 0..300; battery BATT #73bf69 10..16.
Sparkline wall (LineGraph, 4 columns x 4 rows; spark_w .242, spark_h .115, y0 .510; x = .005 + (spark_w+.006)*col; y = y0 + (spark_h+.008)*row; ids tl_sp_*; title/channel/units/min..max/color/digits):
row0: RPM rpm 0..9000 BLUE d0; MAP map kPa 0..250 CYAN; TPS tps % 0..100 ORANGE; SPEED speed km/h 0..260 WHITE
row1: AFR afr :1 10..20 GREEN d1; LAMBDA lambda λ .7..1.3 GREEN d3; VE ve % 0..150 YELLOW; PULSE pulseWidth ms 0..25 BLUE d2
row2: COOLANT coolant °C -20..120 BLUE; IAT iat °C -20..80 ORANGE; TIMING advance ° -10..50 CYAN d1; DUTY dutyCycle % 0..100 PURPLE
row3: BOOST boost kPa 0..300 RED; OIL P oilPressure kPa 0..700 YELLOW; EGT egt °C 0..1000 RED; BATTERY battery V 10..16 GREEN d1
(units for each sparkline from list in the stat tiles; digits default 0 unless listed.) Tests only require shortest_size==0 for every gauge; exact trend titles/color details of items marked approximate are not asserted anywhere.

---
## 8. Tests worth porting
1. `parse_simple_dash` (inline in parser.rs): XML with `<dsh xmlns=...>`, bibliography author "Test", versionInfo 3.0, one gauge RPM 0..8000 painter "Analog Gauge" -> author "Test", file_format "3.0", 1 component, painter AnalogGauge.
2. `dash_parsing.rs` with fixture `tests/fixtures/dashboards/basic.dash` (root `<dashboard>`, un-typed elements, integer colors; props `MinorTick`,`MajorTick`,`WarnThreshold`,`CritThreshold` land in extra_attrs): expects 4 gauges + 2 indicators; RPM gauge title "Engine Speed" 0..8000 units "rpm" Tachometer peg_limits true; coolant "Coolant Temp" high_warning 100 high_critical 110, "Vertical Bar Gauge"; afr "Air/Fuel Ratio" value_digits 2; "Manifold Pressure"; indicators on_text "Battery OK" and "Check Engine"; painters include Tachometer, VerticalBar, BasicReadout, AsymmetricSweep; needle_color.red != 0 (-65536) and font_color.red != 0 (-1); malformed XML -> Err; empty `<gaugeCluster/>` dashboard -> OK with 0 components; minimal gauge with Title/OutputChannel and no painter -> BasicReadout; `.gauge` file parse (title RPM, channel rpm).
3. Parser inline: `show_history_drives_peak_hold` (ShowHistory true -> peak_hold), `peak_hold_round_trips_as_show_history`, `face_angle_parses_and_defaults` (absent -> 360; value kept; garbage -> 360), `face_angle_round_trips`, `parse_color` (attrs alpha/red/green/blue; text -16777216 -> a255,r0,g0,b0; -1 -> 255s).
4. Writer inline: `write_simple_dash`, `roundtrip` (parse(write(d)) preserves fields), `cluster_image_attrs_roundtrip` (BackgroundImageFileName + style), `cluster_lossless_extras_roundtrip` (clusterLayout, enabledCondition, unknown attrs), enabled_condition `rpm > 3000` round-trips verbatim via CDATA.
5. Corpus round trip (`stock_ts_dash_corpus.rs`): for every stock TS `.dash`, parse -> write -> parse and compare (skips if corpus absent): implement as structural equality over all fields incl. extra_attrs.
6. Templates: telemetry live: every gauge has shortest_size==0; each template must pass `lt_dash_validate` with no `InvalidRange`/`EmptyDashboard`.
7. gauges.rs unit tests (section 4) and gaugeGeometry/ema/peakTracking tests: `emaStep` (dt<=0 -> prev; dt clamp 100; tau 120), `isSpike` (range<=0 false; >15% range), `nextPeakState` (rising ratchets, decay after history_delay, delay<=0 holds, first frame starts clock), `seedPeak` (clamped to min/max; not peak_hold -> fallback), `resolveGaugeArc` shapes above, `staggerSlotForChannel` (djb2 mod 16 stable).

## 9. Gotchas
- Parsing: color element text OVERRIDES attrs; empty color elements with no alpha attr give alpha 0; element names are case-sensitive; unknown props must survive round trip via sorted extra_attrs; strings with `<>&` must be CDATA on write.
- Defaults differ between "absent" and "garbage" for some fields (FaceAngle 360 both; Max garbage -> 100; RelativeWidth garbage -> 0.25 for gauge, 0.1 for indicator).
- Only one cluster is read/written. `needle_smoothing`, `gauge_style` are never applied at render.
- Painter zone logic differs: dial/sweep/dashed/histogram use high_warning/high_critical only; round/roundDashed use hard-coded 10%/25% of range; value color uses all four thresholds + hysteresis.
- Guard divisions by (max-min)==0 and n-1==0 (original yields NaN/Inf and nothing is drawn).
- Gradient/shadow/alpha are cosmetic; a framebuffer renderer should implement: filled/stroked arcs, round rects, thick lines with caps, alpha blend, linear/radial gradient (or flat), bitmap/vector text with metrics, clipping.
