# 08 — Simulator, Demo, Base Map, Table Ops, Dynamic Tables, Unit Conversion

Source: `libretune-core/src/{simulator/**,demo.rs,basemap/*,table_ops.rs,dynamic_table.rs,unit_conversion.rs}`.
Target: C89, no deps beyond libc/libm. All numbers are `double` unless stated; engine-sim state is `int` (32-bit). Use `lt_` prefix.
OUT OF SCOPE: serde (JSON) derives, `rand::StdRng` entropy seeding, threads/Mutex/Arc (single-threaded in C), `crc32fast` crate (implement CRC-32 IEEE yourself), std::time (use a caller-supplied ms clock).

Conventions: tables are `rows(y) x cols(x)`, `z[y][x]`, stored row-major flat `double z[rows*cols]` in C. A "cell" is `(y,x)` = (row,col). A selection is an array of `lt_cell {size_t y,x;}`.

---------------------------------------------------------------------------
## 1. Unit conversion (`unit_conversion.rs`) — pure functions
Exact constants (copy verbatim):
```
c2f(c)   = c*9.0/5.0 + 32.0          f2c(f) = (f-32.0)*5.0/9.0
kpa2psi  = kpa * 0.14503773773020923 psi2kpa = psi / 0.14503773773020923
bar2psi  = bar * 14.503773773020923  psi2bar = psi / 14.503773773020923
kmh2mph  = kmh * 0.62137119223733    mph2kmh = mph / 0.62137119223733
L2galUS  = L * 0.26417205235815      galUS2L = g / 0.26417205235815
L2galImp = L * 0.21996924829909      galImp2L= g / 0.21996924829909
lbs2kg   = lbs * 0.45359237          kg2lbs  = kg / 0.45359237
liters_to_gallons(L, imperial) / gallons_to_liters(g, imperial): dispatch on flag (nonzero = imperial).
```
Lambda/AFR: `lambda_to_afr(l, fuel) = l * stoich(fuel)`; `afr_to_lambda(a, fuel) = a / stoich(fuel)`.
`fuel` string is lower-cased (ASCII) then matched; unknown -> 14.7:

| name(s) | stoich |
|---|---|
| "gasoline","petrol" | 14.7 |
| "e85" | 9.8 |
| "methanol" | 6.4 |
| "ethanol" | 9.0 |
| "lpg","propane" | 15.5 |
| "cng","natural_gas" | 17.2 |
| "diesel" | 14.5 |
| other | 14.7 |

NOTE: this table differs from the basemap `FuelType` stoich table (section 3) — keep both separately (methanol 6.4 vs 6.5; LPG 15.5 vs 15.7).

C API:
```c
double lt_c_to_f(double); double lt_f_to_c(double);
double lt_kpa_to_psi(double); double lt_psi_to_kpa(double);
double lt_bar_to_psi(double); double lt_psi_to_bar(double);
double lt_lambda_to_afr(double lambda, const char *fuel);
double lt_afr_to_lambda(double afr, const char *fuel);
double lt_kmh_to_mph(double); double lt_mph_to_kmh(double);
double lt_l_to_gal_us(double); double lt_gal_us_to_l(double);
double lt_l_to_gal_imp(double); double lt_gal_imp_to_l(double);
double lt_l_to_gal(double,int imperial); double lt_gal_to_l(double,int imperial);
double lt_lbs_to_kg(double); double lt_kg_to_lbs(double);
```
Tests (tol 0.01 unless noted): c2f(0)=32 exactly; f2c(32)=0 exactly; c2f(100)=212 exactly; f2c(212)=100 exactly; -40C=-40F; kpa2psi(100)=14.504; psi2kpa(14.504)=100; kpa2psi(101.325)=14.696; lambda_to_afr(1,"gasoline")=14.7; afr_to_lambda(14.7,"gasoline")=1; lambda_to_afr(1,"E85")=9.8 (case-insensitive); kmh2mph(100)=62.14; mph2kmh(62.14)=100; L2galUS(3.78541)=1; galUS2L(1)=3.78541; L2galImp(4.54609)=1; lbs2kg(100)=45.36; kg2lbs(45.36)=100; bar2psi/psi2bar roundtrip.

---------------------------------------------------------------------------
## 2. Table operations (`table_ops.rs`)
All ops are pure: return a new grid (C: take `const double*` in, write to out buffer, or mutate a copy). Exceptions: `set_cells_equal` mutates in place. `eps = DBL_EPSILON (2.220446049250313e-16)`.

### 2.1 Helper: get_cell bounds check
`y < rows && x < cols` where `rows=len(z)`, `cols = rows? len(z[0]) : 0` (ragged rows allowed in Rust; in C assume rectangular).

### 2.2 `scale_cells(z, sel, f)`: for each (y,x) in sel that is in-bounds: `z[y][x] *= f`. Out-of-bounds cells silently skipped. Duplicates in sel apply repeatedly (value scaled twice).
### 2.3 `add_offset(z, sel, off)`: same, `+= off`.
### 2.4 `set_cells_equal(z, sel, v)`: in place, in-bounds cells = v.

### 2.5 `interpolate_cells` (2D, "Interpolate" = bilinear blend of selection bbox corners)
1. if `len(sel) < 4` return copy unchanged.
2. bbox: min_x,max_x,min_y,max_y over sel.
3. corners TL=z[min_y][min_x], TR=z[min_y][max_x], BL=z[max_y][min_x], BR=z[max_y][max_x]; if ANY corner out of bounds -> return unchanged copy (no partial).
4. `y_span=max_y-min_y`, `x_span=max_x-min_x` (as double). For EVERY cell in bbox (not just selected cells): `yr = y_span>0 ? (y-min_y)/y_span : 0`; `xr` likewise;
   `z = TL*(1-yr)*(1-xr) + TR*(1-yr)*xr + BL*yr*(1-xr) + BR*yr*xr`.
   Degenerate axis -> ratio 0 (never NaN). Note: when y_span==0, bottom corners are same row so result is linear along x using TL/TR only.
Tests: 3x3 `[[10,0,40],[0,0,0],[20,0,80]]` select all -> corners unchanged, center 37.5. Single row `[10,0,0,40]` sel all 4 -> 10,20,30,40 (1e-9). Single col same. Out-of-bounds sel `[(0,0),(0,5),(9,0),(9,5)]` on 2x2 -> unchanged.

### 2.6 `interpolate_linear(z, sel, axis)` axis in {ROW, COL}
1. empty sel -> copy. bbox as above. If `max_y>=rows || max_x>=cols` -> unchanged (whole op rejected).
2. ROW (horizontal): for y in min_y..max_y: `start=z[y][min_x]`, `end=z[y][max_x]` (read from the result being built — same row not yet modified at that time), `span=max_x-min_x`; if span>0, for x in min_x..max_x: if `(y,x)` is in sel (linear membership test): `z[y][x] = start + (end-start)*(x-min_x)/span`.
3. COL (vertical): for x in min_x..max_x: start=z[min_y][x], end=z[max_y][x], span=max_y-min_y; for y: if (y,x) in sel: `z = start+(end-start)*(y-min_y)/span`.
Anchors are the EDGES of the bbox of the selection (not the neighbors outside it). Unselected cells in bbox are untouched but their values may serve as anchors.
Tests: `[[10,0,0,50],[20,0,0,80]]` sel row0 ROW -> 10, 23.333.., 36.666.., 50; row 1 unchanged. COL case `[[10,20],[0,0],[0,0],[40,80]]` sel col1 -> 20,40,60,80. Partial sel `[(0,1),(0,2)]` of `[10,0,0,40]` -> index1 stays 0.0 (anchors are 1 and 2). OOB sel -> unchanged.

### 2.7 `fill_region(z, sel, dir)` dir in {RIGHT, DOWN}
Empty sel -> copy; bbox; if `max_y>=rows||max_x>=cols` -> unchanged (whole op, even if only one cell stale).
RIGHT: for y in min_y..max_y: `src=z[y][min_x]`; for x in min_x..max_x: if (y,x) in sel: `z[y][x]=src`.
DOWN: for x: `src=z[min_y][x]`; for y: if (y,x) in sel: `z[y][x]=src`.
Tests: 2x3 `[[10,0,0],[20,0,0]]` all selected RIGHT -> rows become 10,10,10 / 20,20,20. DOWN 3x3 row0 [10,20,30] -> columns filled. Partial OOB `[(0,0),(0,1),(0,2),(0,99)]` -> unchanged.

### 2.8 `smooth_table(z, sel, factor)` — 3x3 Gaussian weighted average
1. copy z. `factor <= 0` -> return copy. If ANY sel cell has `y>=rows || x>=cols` -> return copy unchanged (whole op rejected, even partially stale).
2. `two_sigma_sq = 2*factor*factor`. For each (y,x) in sel (in order): `sum=0, wsum=0`; for dy,dx in -1..1: neighbor (ny,nx) in bounds: `val = ORIGINAL z[ny][nx]` (reads from the INPUT grid, NOT the partially smoothed result, so order of cells is irrelevant); `w = exp(-(dy*dy+dx*dx)/two_sigma_sq)`; sum += val*w; wsum += w.
   `if wsum>0: result[y][x] = sum/wsum`.
Weights at sigma=1: center 1, edge-neighbor e^-0.5=0.6065, diagonal e^-1=0.3679. Edge/corner cells just have fewer neighbors (renormalized).
Tests: 3x3 of 10s with center 50, sel (1,1), factor 1 -> 10<result<50. Corner (0,0)=50 and edge (0,1)=50 likewise strictly between. factor 0 -> exactly 50. factor 2.0 result < factor 0.5 result. Sel (3,3) on 3x3 -> unchanged; `[(0,1),(2,2)]` on 2x2 -> unchanged.
Numeric example (center 50, 8 nbrs 10, sigma 1): w sum = 1+4*0.60653+4*0.36788=4.8981; weighted = 50 + 10*(3.8981)=88.98... => 88.98/4.8981=... compute in code: (50*1 + 10*3.89815)/4.89815 = 18.17 (approx; do not hardcode, assert range).

### 2.9 `rebin_table(old_x, old_y, old_z, new_x, new_y, interpolate_z)` -> {x_bins,y_bins,z, table_name=""}
Output z is `len(new_y) x len(new_x)` zero-initialised.
- `interpolate_z` true: for each (y,x): `z[y][x] = bilinear(new_x[x], new_y[y], old_x, old_y, old_z)`.
- false: `copy_z_by_index`: if old_z has 0 rows -> leave zeros. else for each new (y,x): `old_row = old_z[min(y, old_rows-1)]`; if old_row empty skip; `z[y][x] = old_row[min(x, len(old_row)-1)]` (clamp index; grow replicates last row/col; shrink keeps top-left block).
Bilinear `interpolate_value(tx,ty,xb,yb,z)`: `(x0,x1,fx)=find_surrounding(tx,xb)`, `(y0,y1,fy)=find_surrounding(ty,yb)`; `v00=z[y0][x0] v10=z[y0][x1] v01=z[y1][x0] v11=z[y1][x1]` (any OOB -> 0.0); `top=v00+(v10-v00)*fx; bot=v01+(v11-v01)*fx; return top+(bot-top)*fy`.
`find_surrounding(v,bins)`: empty -> (0,0,0); `v<=bins[0]` -> (0,0,0); `v>=bins[last]` -> (last,last,0); else linear scan i=0..: first i with `bins[i]<=v<=bins[i+1]`: `span=b[i+1]-b[i]`; `ratio = fabs(span)<DBL_EPSILON ? 0 : (v-b[i])/span`; return (i,i+1,ratio). Fallback (last,last,0). Bins assumed ascending; clamps (never extrapolates).
Tests: same bins/interp -> values equal (0.01). 4x4 -> `[500,3000]x[20,80]` gives 2x2. z=[[0,10],[100,110]] bins [0,10]x[0,10], target (5,5) -> 55.0. Clamp: new_x=[-5,0,15], new_y=[-2,0,12]: z[0][0]=0, z[1][1]=0, z[2][2]=110. No-interp same size -> identical. Grow 3x3->4x4 no-interp: z[3][0]=old[2][0], z[0][3]=old[0][2], z[3][3]=old[2][2], top-left 3x3 preserved. Shrink 4x4->2x2 no-interp: `[[1,2],[5,6]]`. Empty old grid no-interp -> 2x2 zeros, no crash.

C API:
```c
typedef struct { size_t y, x; } lt_cell;
void lt_tbl_scale(double *z,size_t rows,size_t cols,const lt_cell*sel,size_t n,double f);   /* in place; caller copies if undo needed */
void lt_tbl_offset(...,double off);
void lt_tbl_set_equal(...,double v);
void lt_tbl_interp2d(double*z,rows,cols,sel,n);
void lt_tbl_interp_linear(double*z,rows,cols,sel,n,int axis /*0=row,1=col*/);
void lt_tbl_fill(double*z,rows,cols,sel,n,int dir /*0=right,1=down*/);
void lt_tbl_smooth(double*z,rows,cols,sel,n,double factor);   /* needs temp copy of input */
int  lt_tbl_rebin(const double*xb,size_t nx,const double*yb,size_t ny,const double*z,
                  const double*nxb,size_t nnx,const double*nyb,size_t nny,int interp,double*out_z /*nny*nnx*/);
double lt_tbl_bilinear(double tx,double ty,const double*xb,size_t nx,const double*yb,size_t ny,const double*z);
```

### 2.10 Copy/paste semantics
There is NO copy/paste code in these files (UI-layer, out of this subsystem). Selection coordinates are always (row=y, col=x); paste is not defined here.

### 2.11 2D/3D lookup (VE-model lookup, `simulator/ve_model.rs`)
Same as bilinear above except via `segment(bins,v)` returning (lower index, fraction):
```
last = n-1; if last==0 -> (0,0.0)
v<=bins[0] -> (0,0.0); v>=bins[last] -> (last-1,1.0)
scan i in 0..last-1: if bins[i]<=v<=bins[i+1]: span=hi-lo; frac = span>0 ? (v-lo)/span : 0; return (i,frac)
fallback (last-1,1.0)
```
`current_ve(rpm,load)`: requires nx,ny>0 and `len(ve)==nx*ny` else NONE. `x0,fx=segment(rpm_bins,rpm)`, `y0,fy=segment(load_bins,load)`, `x1=min(x0+1,nx-1)`, `y1=min(y0+1,ny-1)`; `cell(y,x)=ve[y*nx+x]` (row = load bin, col = rpm bin); `top=c(y0,x0)*(1-fx)+c(y0,x1)*fx; bot=c(y1,x0)*(1-fx)+c(y1,x1)*fx; return top*(1-fy)+bot*fy`.
Tests: bins rpm [1000,2000], load [40,80], ve [50,60,70,80]: (1500,60)=65; (1000,60)=60; (1500,40)=55; (0,0)=50; (9000,300)=80; ve with len 3 -> NONE.

---------------------------------------------------------------------------
## 3. Dynamic (resizable) tables (`dynamic_table.rs`)
Allocated footprint is fixed; active size comes from two scalar constants (`cols_const`, `rows_const`). Z packing: row-major with stride = ACTIVE cols; unused tail preserved.
```c
typedef struct { char cols_const[64], rows_const[64];
  size_t min_cols,max_cols,min_rows,max_rows,max_elements,active_cols,active_rows; } lt_table_size;
```
- `allows(cols,rows)`: errors (text): `"Columns %d out of range %d..%d"` (cols<min||>max); `"Rows %d out of range %d..%d"`; `"%dx%d exceeds cell budget %d"` (rows, cols, max_elements; test `cols*rows > max_elements`, saturating multiply). Order: cols, rows, budget.
- `clamp_to_budget`: `while ac*ar>max_el && ac>min_cols: ac--;` then `while ac*ar>max_el && ar>min_rows: ar--;`.
- `resolve_axis_count(raw?, min, max, default?)`: `fallback = clamp(default? round(default) : min, min, max)`; if raw present: `n=(long)round(raw)`; `n<min ? fallback : clamp(n,min,max)`; absent -> fallback. (Below-min incl. 0 uses default, NOT clamp-to-min.)
- `table_size_info(def, table, get_value)`: needs table's `map` constant with `dynamic_size{cols_const?, rows_const}`; both size constants must exist. `min/max_cols/rows = max(1, round(const.min/max))`. `max_elements = def.maximum_elements[table.map]` else element_count(map.shape), `.max(1)`. `active_* = resolve_axis_count(get_value(name), min,max, def.default_values[name])`; then `clamp_to_budget`. Returns none if any lookup fails. (Depends on INI structures — spec 02.)
- `size_const_names(def)`: every constant with dynamic_size: collect cols_const (if any) and rows_const; sort; dedup.
- `patch_invalid_size_scalars(def, pages)`: for each size const name: constant must exist, `data_type.size_bytes()==1`, page exists, `offset < page_len`; `min=max(1,round(c.min)) as u8`; `max=max(round(c.max),min) as u8`; `default = clamp(default_values[name]? round : min, min,max)`; if `page[off]<min||>max` -> `page[off]=default`, count++. Returns count (mark tune dirty if >0).
- `tables_sharing_size_consts(def, cols, rows)`: names of tables whose map has dynamic_size with identical cols_const and rows_const.
- `slice_bins(bins,active)`: empty -> `max(active,1)` zeros; `len>=active` -> first `active`; else pad with last value to `active`.
- `unpack_z(flat,cols,rows)`: `z[y][x]=flat[y*cols+x]` or 0.0 if past end.
- `pack_z_into(alloc, z)`: `cols=len(z[0])`; `alloc[y*cols+x]=z[y][x]` for in-range slots; remaining tail untouched.
- `linspace_bins(first,last,count)`: 0->empty; 1->[first]; else `step=(last-first)/(count-1)`; `first+step*i`.
- `is_dynamic_array(c)`: dynamic_size != null. `allocated_elements`: Scalar 1; Array1D(n) max(n,1); Array2D rows*cols (saturating) max 1.
Tests: pack 2x3 `[[1,2,3],[4,5,6]]` into 16 x 9.0 -> first 6 are 1..6, flat[6]==9.0, unpack(flat,3,2)==z. linspace(0,100,5)=[0,25,50,75,100]. resolve_axis_count(Some(0),8,64,Some(32))=32; (Some(1)..)=32; (None)=32; (Some(24))=24; (Some(100))=64.

---------------------------------------------------------------------------
## 4. Base map generator (`basemap/`)
### 4.1 Enums
```
FuelType: Gasoline=0,E85,E100,Methanol,LPG
Aspiration: NA, Turbo, Supercharged
StrokeType: FourStroke, TwoStroke
InjectionMode: Simultaneous, Batch, Sequential, ThrottleBody
IgnitionMode: WastedSpark, CoilOnPlug, Distributor
CombustionChamber: OpenChamber, QuenchTwoValve(default), SwirlMultiValve
```
| Fuel | stoich | density g/cc | safe WOT AFR | eff. octane default | crank factor | WUE factor | prime base ms | IAC cold % |
|---|---|---|---|---|---|---|---|---|
| Gasoline | 14.7 | 0.75 | 12.5 | 93 | 1.0 | 1.0 | 3.0 | 70 |
| E85 | 9.8 | 0.79 | 8.5 | 100 | 1.4 | 1.2 | 4.5 | 80 |
| E100 | 9.0 | 0.789 | 7.8 | 105 | 1.6 | 1.3 | 5.0 | 80 |
| Methanol | 6.5 | 0.792 | 5.5 | 105 | 1.8 | 1.4 | 6.0 | 80 |
| LPG | 15.7 | 0.51 | 13.5 | 105 | 0.8 | 0.9 | 2.0 | 70 |

### 4.2 EngineSpec
```c
typedef struct { uint8 cylinder_count; double displacement_cc, injector_size_cc; int fuel,aspiration,stroke,inj_mode,ign_mode;
  unsigned idle_rpm, redline_rpm;
  int has_boost; double boost_target_kpa; int has_wot_afr; double target_wot_afr;
  int has_octane; double octane; int has_cr; double compression_ratio; int has_chamber; int chamber; } lt_engine_spec;
```
Default: cyl 4, disp 2000, inj 440, Gasoline, NA, FourStroke, Sequential, WastedSpark, idle 800, redline 6500, all optionals unset.
- `compute_req_fuel`: `dpc=disp/cyl; divider = Simultaneous:1, ThrottleBody:1, Batch:max(cyl/2.0,1.0), Sequential:cyl; req=(dpc*stoich*10.0)/(inj*divider); clamp(req, 0.1, 25.5)`. (Default 2L seq computes 41.76 -> clamped 25.5.)
- `safe_wot_afr`: target_wot_afr if set else table.
- `max_load_kpa`: NA 105.0; boosted: `max(boost_target_kpa or 200.0, 120.0)`.
- `effective_octane`: octane if set else table.
- `max_spark_advance`: `adv=34; adv += clamp((oct-93)*0.6,-8,8); cr=CR or 10.0; adv += clamp((10-cr)*1.5,-8,6); chamber: Open +2, Quench 0, Swirl -2; TwoStroke: -6; return clamp(adv,18,42)`.

### 4.3 Axes
`generate_rpm_bins(idle,redline,count=16)`: count<=1 -> [idle]. For i: `t=i/(count-1); bt=pow(t,1.3); rpm=idle+(redline-idle)*bt; r=round(rpm/100)*100; bin=max(r,idle)`. Then `bins[0]=round(idle/100)*100; bins[last]=round(redline/100)*100`. Dedup ADJACENT equal values (Vec::dedup); while len<count: append `last+100`; truncate to count. (`round` = half away from zero, C99 `round`; in C89 implement `floor(x+0.5)` for x>=0.)
`generate_load_bins(max_load,count=16)`: count<=1 -> [max_load]. `min=15.0`; `kpa=round(15+(max-15)*t)`; `bins[0]=15; bins[last]=round(max_load)`; dedup adjacent; fill with `last+5` until count; truncate.
Tests (idle 800, redline 6500): 16 bins, bins[0]>=800, |bins[15]-6500|<100, non-decreasing. load(105,16): first 15, last 105, increasing. load(250): last 250.

### 4.4 VE table (rows = load bins, cols = rpm bins)
```
rpm_norm = clamp((rpm-idle)/(redline-idle),0,1);  load_norm = clamp((load-15)/(max_load-15),0,1)
NA:      rf = 1-(rpm_norm-0.6)^2*0.3;  ve = 30 + 55*load_norm*rf
Boosted: rf = 1-(rpm_norm-0.55)^2*0.25; bf = load>101 ? 1+max(load_norm-0.5,0)*0.3 : 1;  ve = 28 + 62*load_norm*rf*bf
cell = round(clamp(ve,15,120))
```
Tests (default spec): 16x16; ve[0][0]<50; ve[15][8]>50.

### 4.5 Ignition table
```
peak = max_spark_advance(); idle_adv=14.0; wot_pull = clamp(peak-12, 6, 16);
boost_retard_rate = clamp(20-(eff_octane-90)*0.4, 8, 22)
per cell: ramp=min(rpm_norm/0.55,1); base=14+(peak-14)*ramp; adv = base - wot_pull*load_norm;
 if boosted && load>101 && max_load>101: adv -= (load-101)/(max_load-101) * boost_retard_rate
 if rpm < idle*1.2 && load < 50: adv = clamp(adv,10,20)
 cell = round(clamp(adv,0,45))
```
Tests: all in [0,45]; ign[8][15] > ign[8][0].

### 4.6 AFR table
`stoich=fuel stoich; wot=safe_wot_afr`. per cell, in order:
`load_norm>0.85` -> wot; else `load_norm<0.35 && rpm_norm<0.3` -> `stoich*0.97`; else `load_norm<0.5` -> `stoich*1.02`; else `blend=clamp((load_norm-0.5)/0.35,0,1); stoich + (wot-stoich)*blend`. Cell = `round(afr*10)/10`.
Tests: afr[15][8] < afr[8][3] (default); E85 afr[15][8] < 10.

### 4.7 Curves (8 points each, (coolant_C, value)); temps = -40,-20,0,20,40,60,80,100
- Cranking enrichment % : 400,350,280,200,150,120,100,90 each * crank factor.
- Warmup enrichment % : 180,165,150,135,120,110 each * WUE factor for temps -40..60; then (80,100.0),(100,100.0) UNSCALED.
Tests: first > last; WUE first>100, last==100.
- `prime_pulse_ms = clamp(base * (440/max(inj,100)), 0.5, 15.0)`.
- AccelEnrich default: tps_threshold 10.0 (%/s), enrichment_pct 30.0, duration_cycles 3, taper_pct 50.0.
- IacConfig: cold_start_pct (table), warm_idle_pct 20.0, warm_threshold_c 75.0.

### 4.8 `generate_base_map(spec)` (table_size=16)
Outputs: rpm_bins, load_bins, ve/ign/afr [16][16], curves, accel, iac, prime_pulse_ms, req_fuel, and scalars map (name -> displayed value):
`reqFuel=req_fuel; injType: Simultaneous 0, Sequential 1, Batch 2, ThrottleBody 3; sparkMode: Wasted 0, COP 1, Distributor 2; nCylinders=cyl; stoich=fuel stoich`.
Tests: complete map has 16 each, nonempty curves, req_fuel>0, prime>0, has keys reqFuel & nCylinders. Turbo boost 200 -> last load bin > 101. Other engine_spec tests: stoich(Gas)=14.7, E85 9.8, LPG 15.7; max_load NA 105, Turbo(250)=250; safe_wot Gas 12.5, E85 8.5; req_fuel of (4cyl,2000cc,440 seq) in (0,25.5]; simultaneous 1600cc/1000cc in (0,25.5].

---------------------------------------------------------------------------
## 5. Demo (cosmetic) simulator (`demo.rs`) — `lt_demo`
Generates a name->double map of fake channels. State: `start_time_ms(0), last_update_ms(0), next_blip_at_ms (random 8000..15000), blip_state{Idle,RampUp(start),Hold(start),RampDown(start)}, current_rpm=850, blip_target_rpm=0`. RNG: any PRNG (Rust seeds from entropy; a seedable xorshift is fine). Uniform ranges: first/next blip interval `[8000,15000)` ms, blip target `[2000,4000)` rpm.
`update(elapsed_ms)`:
1. `if start_time_ms==0: start_time_ms=elapsed_ms`. `sim_time=elapsed-start`. `delta=last_update>0 ? elapsed-last_update (saturating) : 0`; `last_update=elapsed`.
2. State machine (RAMP_UP 300ms, HOLD 200, RAMP_DOWN 800): Idle: if `sim_time>=next_blip_at` -> pick target, RampUp(sim_time). RampUp: `sim_time>=start+300` -> Hold(sim_time). Hold: `>=start+200` -> RampDown. RampDown: `>=start+800` -> Idle, `next_blip_at = sim_time + rand[8000,15000)`.
3. target_rpm: Idle 850; RampUp `850+(blip-850)*min((sim-start)/300,1)`; Hold blip; RampDown `blip+(850-blip)*min((sim-start)/800,1)`.
4. slew: `rate = target>current ? 8000 : 3000` rpm/s; `maxchg=rate*delta/1000`; `current += clamp(target-current,-maxchg,maxchg)`.
5. `t=sim_time/1000.0`; idle wobble only in Idle: `20*sin(t*2.5)+10*sin(t*7.3)`; `rpm=max(current+wobble,0)`.
6. Channels (aliases all get same value):
 - rpm, RPM = rpm
 - tps, TPS, throttle = clamp(x,0,100), x: Idle `1.5+1.0*|sin(t*0.3)|`; RampUp `p=(cur-850)/(blip-850); 2+p*45`; Hold `45+5*sin(t*3)`; RampDown `2+p*40` (same p).
 - map, MAP, fuelLoad = clamp(m,20,105): `rpm<1000 ? 35+10*(1-rpm/1000) : 25+rpm/100`.
 - afr = `14.7+0.3*sin(t*1.5)+0.1*sin(t*4.7)` -> keys afr, AFR, AFRValue; lambda = afr/14.7; afrTarget = `rpm<1500 ? 14.7 : 13.5`
 - coolant, CLT, coolantTemperature = `20+70*(1-exp(-t/120))`
 - iat, IAT = `25+5*sin(t*0.1)`
 - batteryVoltage, vBatt = `13.8+0.2*sin(t*0.5)`
 - advance, timing = clamp(`10+rpm/200+3*sin(t*0.7)`,-10,45)
 - pulseWidth, actualLastInjection = `2.5+(rpm/1000)*1.5+(tps/50)*2` (tps = UNclamped x)
 - VE1, veCurr = clamp(`75+rpm/100+10*(tps/50)`,0,150)
 - egoCorrection = `100+5*sin(t*2)`; hasSync=running=engine=1.0; dwell=`3+0.5*(rpm/2000)`; baro, baroPressure = 101.0
Tests: update(0) has rpm,map,afr,coolant. Idle 0..3000ms step 100: 700<rpm<1000. Coolant: update(1000) then update(61000): later>start and <90.

---------------------------------------------------------------------------
## 6. Virtual ECU engine model (`simulator/engine/`) — `lt_simengine`
Integer physics at 50 ms steps (20 Hz). Temps in deci-degC. Constants:
```
RPM_MIN 0, RPM_IDLE_MIN 700, RPM_IDLE_MAX 900, RPM_CRUISE 2500, RPM_HIGH_START 5000, RPM_MAX 7000, RPM_REDLINE 6800
TEMP_AMBIENT 200, TEMP_ENGINE_WARM 800, TEMP_ENGINE_HOT 950   (deci-C)
MAP_ATMOSPHERIC 100, MAP_IDLE 35, MAP_WOT 95 (kPa); VOLTAGE_NORMAL 140 (dV); AFR_STOICH 147 (x10)
TPS_IDLE 2, TPS_CRUISE 20, TPS_HALF 50, TPS_WOT 100; TIMING_IDLE 15, TIMING_MAX 35
UPDATE_INTERVAL_MS 50; STATE_TRANSITION_MS 5000; STEPS_PER_SECOND 20
```
Modes: Startup, WarmupIdle, Idle, LightLoad, Acceleration, HighRpm, Deceleration, Wot.
**RNG**: xorshift32, seed 0x4F545531. `next(): x^=x<<13; x^=x>>17; x^=x<<5` (uint32 wrap); `range(lo,hi) = lo + (int)(next() % (uint32)(hi-lo))` (hi exclusive). Call ORDER matters for determinism; follow exactly as below (C evaluation order of function args: evaluate RNG draws in the listed sequence, never inside one expression with unspecified order).
State (ints): mode, time_ms(u64), acc_ms, state_start_ms, steps, secl(u8), target_rpm, rpm, rpm_accel, target_tps, tps, tps_reading, coolant_dc, intake_dc, map_kpa, battery_dv, advance_deg.
Init: mode Startup, all times 0, target_rpm=0, rpm=0, rpm_accel=0, target_tps=tps=tps_reading=2, coolant_dc=intake_dc=200, map_kpa=100, battery_dv=140, advance_deg=0; then `transition(Startup)`; then encode.
**tick(dt_ms)**: `acc+=dt; while acc>=50: acc-=50; step();` then encode (even with 0 steps).
**step()**: `time+=50; steps++; if steps%20==0: secl=(secl+1)&0xFF; state_machine(); simulate_rpm(); simulate_thermal(); simulate_throttle(); simulate_map(); simulate_ignition(); simulate_voltage();`
**state_machine** (`in_state = time - state_start`):
- Startup: `in_state>1000 && rpm>350` -> WarmupIdle.
- WarmupIdle: `coolant_dc>600` -> Idle.
- Idle: if `in_state>5000`: `r=range(0,100)`; r<30 -> LightLoad; else r<35 -> Acceleration.
- LightLoad: if `in_state>5000`: r=range(0,100): r<40 Acceleration; r<70 Deceleration; else Idle.
- Acceleration: `rpm>5000` -> HighRpm; else if `in_state>3000 && range(0,100)<30` (draw only if in_state>3000, short-circuit) -> LightLoad.
- HighRpm: `in_state>2000` -> Deceleration.
- Deceleration: `rpm < 1100` (RPM_IDLE_MAX+200) -> Idle.
- Wot: `in_state>3000 || rpm>6800` -> HighRpm.
**transition(mode)**: `mode=m; state_start=time;` (target_rpm, target_tps, accel) with draws in listed order (rpm draw first, then tps draw):
- Startup: (900, 7, 500)
- WarmupIdle: (850, 5, 100)
- Idle: (700+range(-50,50), 2, 50)
- LightLoad: (2500+range(-300,300), 20+range(-5,10), 200)
- Acceleration: (5000+range(-500,500), 50+range(10,40), 1000)
- HighRpm: (6800-range(100,500), 100-range(0,20), 500)
- Deceleration: (900+range(0,150), 2, -800)   (must stay < 1100 exit threshold)
- Wot: (6800, 100, 1500)
**simulate_rpm**: `step=abs(rpm_accel*50/1000)` (integer division; e.g. 500->25, 100->5, 50->2, 1000->50, 1500->75, 800->40); if rpm<target: `rpm=min(rpm+step,target)`; else if rpm>target: `rpm=max(rpm-step,target)`; `rpm=clamp(rpm,0,7000)`; if mode in {Idle,WarmupIdle}: `rpm=clamp(rpm+range(-10,10),0,7000)`.
**simulate_thermal**: `target = (Wot|HighRpm)?950 : (Idle|WarmupIdle)?750 (=800-50) : 800; coolant=slew(coolant,target,5)`; `intake_target=200+(coolant-200)/4` (C int division truncates toward zero, same as Rust); `if rpm>2500: intake_target -= (rpm-2500)/50`; `intake=slew(intake,intake_target,10)`.
**slew(cur,tgt,rate)**: `delta=tgt-cur; step=delta*rate/100; if step==0&&delta!=0: step=sign(delta); return cur+step`.
**simulate_throttle**: `tps=slew(tps,target_tps,20); tps_reading=clamp(tps+range(-1,2),0,100)`.
**simulate_map**: `if tps<10: base=35+(rpm-700)/20; else if tps>80: base=95-(7000-rpm)/100; else base=map_value(tps,10,80,45,90)`; `if rpm>5000: base+=(rpm-5000)/100`; `map=clamp(base+range(-2,3),0,100)`. `map_value(x,imin,imax,omin,omax)=(x-imin)*(omax-omin)/(imax-imin)+omin` (int arithmetic; (rpm-700)/20 truncates toward zero for rpm<700).
**simulate_ignition**: `load=map*100/100; adv=15; if rpm>1000: adv+=(rpm-1000)/200; if load>80: adv-=(load-80)/4; else if load<40: adv+=(40-load)/8; advance=clamp(adv,5,35)`.
**simulate_voltage**: `base = mode==Startup ? 100 : 140; battery_dv=base+range(-3,4)`.
**set_mode(m)** = transition(m) (Wot only reachable this way). reset_secl: secl=0 + re-encode. advance_secl(d): secl=(secl+d)&0xFF + re-encode (250+10 -> 4).
**snapshot (physical values)**: `secl, rpm, map, baro=100, coolant_c=coolant_dc/10, iat_c=intake_dc/10, tps_percent=tps_reading, battery_dv, advance_deg, afr_target=14.7, ego_correction=100.0, running = rpm>0 && mode!=Startup, cranking = mode==Startup`. `afr`: if a VE context is set: `current=max(1.0, ctx.current_ve(rpm,map) or 1.0); wanted=true_ve(rpm,map); afr=14.7*wanted/current`; else `afr=14.7`.
Determinism: same dt sequence -> identical och bytes. Tests: WOT: set_mode(Wot); tick 1000 -> rpm>=1000; tick 3000 more -> mode != Wot. Coolant after tick 60s > initial (initial = 200/10=20C). secl>0 after 5s; reset_secl -> 0 immediately. Block len == och_block_size. Deceleration: over 2000 draws of transition(Decel) target_rpm<1100 always. Idle from above: set Idle, rpm=target+400, calling simulate_rpm repeatedly converges to <= target+20 within 400 calls. Deceleration leaves within 600 ticks of 50ms.

### 6.1 True VE surface and seeding (`ve_model.rs`)
`true_ve(rpm,load) = clamp(40 + 25*(load/100) + 15*(rpm/6000), 20, 110)`.
`VeContext{rpm_bins[],load_bins[],ve[] (row=load, col=rpm)}`; `is_usable`: both axes len>=2 and strictly ascending, `len(ve)==nx*ny`, any ve>0.
`ve_context(def, memory)`: requires INI `[VeAnalyze]` ve_table_name -> table (lookup `tables[name]`, fallback `table_map_to_name[name]`); decode x_bins (rpm), y_bins (load; if absent -> none), map constants from page memory: for each element `physical = raw*scale + translate` with the constant's endianness override else def endianness; count = shape element count (Scalar1/1D n/2D rows*cols); byte total must fit u16; return only if `is_usable`. Any failure -> none.
`seed_ve_table(def, mem)`: needs ve_analyze+table+y_bins; `rpm = spread(count(x_bins),500,7000)`, `load = spread(count(y_bins),20,100)` (spread(n,lo,hi): n<2 -> empty; `lo + (hi-lo)/(n-1)*i`); need both len>=2; cells (row-major, load outer, rpm inner) `= true_ve(r,l)*0.85`; encode each constant: `raw=(phys-translate)/scale` (scale==0 -> 0), clamp to data-type raw range, write with endianness; element count must equal `len(values)`. Returns true if all three written.
Tests: see 2.11 plus: true_ve(3000,80)>true_ve(3000,40); true_ve(5000,60)>true_ve(1000,60); true_ve(-1e9,-1e9)=20; (1e9,1e9)=110. Decode test: U08 rpmBins scale 100 raw [10,20] -> [1000,2000]; load [40,80]; cells [50,60,70,80]. Blank page -> ve_context none; after seed -> usable, `current_ve(3000,60)` in (10,110) and < true_ve(3000,60). All-zero or flat-axis ctx: `is_usable` false.

### 6.2 Och codec (`och_codec.rs`) — writes engine values into the realtime block
`block_size(def)`: `och_block_size` if >0 else max over non-computed (expression==null) channels of `offset + width(kind)` where kind = bit_storage for Bits channels else data_type; empty -> 0. width: U08/S08/Bits 1; U16/S16 2; U32/S32/F32 4; F64 8; String 0.
`encode_channels(channels, endian, values, block)`: for each channel (order irrelevant):
- skip if it has an `expression` (computed channel).
- Bits channel: needs bit_position(lo) and bit_count (>0) else skip; `v = bits_value(name)`: "running"->running, "crank"|"cranking"->cranking, else skip. `write_bits(block,offset,bit_storage,endian,lo,count,v)`.
- else `physical = scalar(name)` (skip unknown names -> bytes stay zero); `raw = scale!=0 ? (physical-translate)/scale : 0`; `write_scalar(block,offset,data_type,endian,raw)`.
Scalar name table (name -> physical):
```
secl -> secl; rpm|RPMValue -> rpm; map|MAPValue -> map_kpa; baro|baroPressure -> 100
coolantRaw -> coolant_c+40; iatRaw -> iat_c+40; coolant -> coolant_c; iat|intake -> iat_c
tps|throttle|TPSValue|throttlePedalPosition -> tps_percent
batteryVoltage|VBatt -> battery_dv/10.0
advance|ignitionAdvance -> advance_deg
afrTarget -> afr_target; afr -> afr
lambdaValue -> afr/14.7; targetLambda|lambdaTarget -> afr_target/14.7
egoCorrection -> 100.0
engine -> (running?1:0) | (cranking?2:0)
```
`write_scalar`: non-finite raw -> 0; for non-float types `raw=round(raw)` (half away from zero); clamp to type range then cast: U08/Bits [0,255]; S08; U16; S16; U32; S32; F32 clamp to +/-FLT_MAX then (float) (floats keep fraction); F64 as is; String no-op. Byte order per `endian` (Big = MSB first). If `offset+len > block_len` skip the write entirely (no partial).
`write_bits`: word width = width(storage) (min 1) bytes; if `count==0 || lo+count > width*8` skip; if offset+width > block_len skip. Read word in `endian` order into uint64 (little: bytes reversed), `mask=(1<<count)-1` (count>=64 -> all ones), `new=(cur & ~(mask<<lo)) | ((value&mask)<<lo)`, write back in `endian` order. Preserves neighbor bits. Example: bit 11 of U32 at offset 0: byte 1 (value 0x08) little-endian, byte 2 big-endian.
Tests: U16 scale 0.5 translate 100 offset 2, rpm 3000 LE -> decodes back to 3000. Bits channel bit3 on 0b10100101 -> 0b10101101. F32 afr 14.35 round trips within 1e-4. F32 scale 1e-30, afr 1 -> finite. U16 at offset 7 in 8-byte block -> nothing written. U08 300 -> 255. BE U16 4660 -> 12 34; LE -> 34 12. Computed channel not written.

---------------------------------------------------------------------------
## 7. Virtual ECU wire protocol (`simulator/ecu.rs`) — `lt_ecusim`
A loop-back byte pipe: `write()` appends to `cmd_buf` and immediately processes; responses accumulate in `rsp_buf`; `read()` pops (empty buffer -> "would block", NOT EOF). `dropped` flag -> both read/write fail with not-connected.
### 7.1 Constants and codes
Return codes (msEnvelope, `ResponseCode`): OK=0x00, OK_BURN=0x02, CRC_ERR=0x82, UNKNOWN_CMD=0x83, RANGE_ERR=0x84 (others: 0x01 table, 0x03 refused, 0x80 underrun, 0x81 overrun, 0x85 busy, 0x86 flash locked, 0x94 generic error w/ msg; high bit 0x80 = error). `FRAME_TIMEOUT=400ms` idle timeout; `SUBCMD_OUTPUT_CHANNELS=0x30`. Defaults: signature `"speeduino 202504-dev"`, version `"Speeduino 2025.04-dev"`.
### 7.2 Memory
`PageMemory{ram, flash}` each = page_count pages of `page_sizes[i]` zero bytes. `read(page,off,count)`: bytes from RAM, zero-padded to `count` if range exceeds page (never fails). `write` returns bool (false if out of range -> not applied). `burn(page)`: flash[page]=copy of ram[page]. `reboot()`: ram = copy of flash for all pages. `seeded(def)`: after seeding VE table in RAM (6.1), if seed succeeded copy every RAM page into flash.
### 7.3 State / construction from definition
`signature = def.signature` (nonempty) else default; `version = def.version_info` (nonempty) else default; `query_cmd = first byte of protocol.query_command` else 'Q'; `cmd_le = def.endianness==Little` (byte order of %2i/%2o/%2c command parameters; Speeduino/MS big, rusEFI little); `framing = Enveloped if protocol.uses_modern_protocol() (messageEnvelopeFormat names msEnvelope) else Plain` (no definition -> Plain); `envelope` starting hint = LittleEndian if def little else BigEndian; `order_latched=false`; `page_ids` = per page: identifier bytes -> len0: index; len1: byte; else `u16 little-endian(bytes[0],bytes[1])`; `och_block` initial = engine block (or empty); `auto_tick=true`; `first_och_done=false`; `secl=0`.
`resolve_page(id)`: if id is in page_ids -> its index; else if page_ids empty -> id as u8 (fail if >255); else none (-> RANGE_ERR).
### 7.4 Framing
Envelope frame (msEnvelope): `len(2) || payload(len) || crc32(4)`; len/crc byte order = `order` (Big or Little); CRC = standard CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF) over payload ONLY (length not included on wire). Max payload `MAX_PACKET_SIZE` (see protocol spec). Response payload = `rc(1) || data`.
Plain framing: bare command bytes, no status byte; refusals are silence.
`take_frame()` (called repeatedly while cmd_buf nonempty):
1. If framing==Enveloped: orders to try = `[latched]` if order_latched else `[hint, other]` (hint first). For each: try decode a frame from the buffer start (need >=6 bytes, `len<=MAX`, buffer >= 2+len+4, CRC match). Reject frames with EMPTY payload (six zero bytes must not latch). On success remove `2+len+4` bytes, return Enveloped{payload,order}.
2. If framing==Plain and first byte is a command byte (`Q S A p M b r R C B k O`): compute `plain_command_len`; if buffered >= len -> consume and return Plain(cmd); else NeedMore.
   plain_command_len(first byte): `p r R k` -> 7; `O` -> 5; `b B` -> 3; `M C` -> if <7 bytes buffered None else `7 + u16(buf[5..7], cmd_le)`; else (`Q S A`, unknown) -> 1.
3. If Enveloped and order latched: if >=2 bytes, `total=2+len(read_u16)+4`; if buffered>=total: consume and return BadCrc(order) (corrupt complete frame is consumed and answered with `[0x82]` framed; memory untouched).
4. Else NeedMore.
In Plain mode, a first byte that is not a command byte falls to step 4 -> NeedMore and sits until idle timeout. In Enveloped mode bare bytes never dispatch.
`process()`: if dropped return; loop while cmd_buf nonempty: Enveloped -> set `envelope=order; order_latched=true`, `resp=respond(payload, enveloped=true)`, append `frame(resp, order)`; BadCrc -> append `frame([0x82], order)`; Plain -> append `respond(cmd,false)` raw; NeedMore -> if partial_since unset set it=now; return. After each handled frame `partial_since=None`.
Write path: before appending, if `partial_since` is set and elapsed >= 400 ms: clear cmd_buf & partial_since. If written buf is nonempty: `partial_since=None` (every write restarts idle clock). Then append and process.
### 7.5 `respond(cmd, enveloped)`
`ok(body) = enveloped ? [0x00]+body : body`; `status(code) = enveloped ? [code] : []`.
Order of dispatch: if `cmd[0]==query_cmd` -> `ok(signature)` FIRST (so rusEFI 'S' returns signature). Then:
- `Q` -> ok(signature); `S` -> ok(version).
- `A` -> on_och_request(); auto_tick(); `ok(och_block)` or if block empty `ok([secl])`.
- `p`|`R` (read page): params at bytes 1..3 id, 3..5 offset, 5..7 count (each u16 per cmd_le). Resolve id; ok -> `ok(memory.read(page,off,count))` (zero-padded); unresolvable/short -> status(0x84).
- `M`|`C` (write): same parse; data = `cmd[7 .. 7+count]` must exist fully (else fail); `wrote = memory.write(...)`. Enveloped reply `[wrote?0x00:0x84]` (NO data); plain: no reply.
- `b`|`B` (burn): id at bytes 1..3; resolve; burn. Enveloped reply `[0x02]` ok / `[0x84]`; plain: none.
- `k` (page CRC): parse as read; `crc32(memory.read(page,off,count))` returned as 4 bytes BIG-endian via ok(); bad -> status(0x84).
- `O` (och get, `O%2o%2c`): off@1..3 len@3..5 -> on_och_request(); auto_tick(); `ok(window(block,off,len))`; parse failure -> ok([]).
- `r` (`['r',canId,0x30,off(2),len(2)]`): byte2 must == 0x30 else `ok([])` (plain: empty reply); off@3..5 len@5..7 (CAN id byte 1 ignored). Same handling as 'O'.
- anything else -> status(0x83).
`window(block,off,len)`: `start=min(off,blen); end=min(start+len,blen)`; copy then zero-pad to exactly `len`.
`on_och_request()`: first call only: `first_och_done=1; secl=0; engine.reset_secl(); och_block=engine block`.
`auto_tick()`: if enabled: `engine.set_ve_context(ve_context(def, ram))`; `dt = now - last_auto_tick (0 first time)`; `engine.tick(dt)`; copy block; `last_auto_tick=now`. In C use a monotonic ms clock (clock()/time() fallback) supplied via a function pointer.
### 7.6 Public control API
```c
lt_ecusim *lt_ecusim_new(void);                          /* no def: handshake-only, Plain, default sig */
lt_ecusim *lt_ecusim_from_def(const lt_ecu_def *def);
size_t lt_ecusim_write(lt_ecusim*, const uint8_t*, size_t);   /* feeds + processes; returns n or -1 if dropped */
long   lt_ecusim_read(lt_ecusim*, uint8_t*, size_t max);      /* >=0 bytes; -2 = would-block(empty); -1 = dropped */
size_t lt_ecusim_bytes_to_read(lt_ecusim*);
void   lt_ecusim_clear_input(lt_ecusim*);   /* clears rsp_buf */
void   lt_ecusim_clear_output(lt_ecusim*);  /* clears cmd_buf */
void   lt_ecusim_tick_engine(lt_ecusim*, unsigned dt_ms);  /* also permanently disables auto_tick */
void   lt_ecusim_set_mode(lt_ecusim*, int mode);
void   lt_ecusim_set_link_dropped(lt_ecusim*, int);
void   lt_ecusim_advance_secl(lt_ecusim*, uint8_t);  /* wraps 255; updates engine+block */
void   lt_ecusim_reboot(lt_ecusim*);  /* RAM<-flash, secl=0, first_och_done=0, last_auto_tick=none */
```
tick_engine: set `auto_tick=false`, refresh ve ctx, tick, copy block. `advance_secl`: `secl+=d` and engine.advance_secl(d) and refresh och_block.
### 7.7 Tests worth porting
- Plain Q -> default signature; def-backed sim answers def's signature. `[r,0,0x30,0,0,20,0]` on 16-byte block returns 20 bytes, last 4 zero. `[r,0,0x99,0,0,4,0]` -> empty. Enveloped Q (LE-default def): payload[0]==0, rest == signature. Enveloped 'M' with 300 bytes (payload >255) acked `[0x00]`. rusEFI spellings: `C 1 0 0 0 1 0 0x5A` then `R 1 0 0 0 1 0` returns `5A` (page id 1 resolves to index 1 when no identifiers declared; little-endian params). Write then read same offset roundtrips; reboot loses unburned writes, keeps burned. Corrupt-CRC frame after latch -> `[0x82]` framed, memory unchanged. Six zero bytes do not latch byte order (then BE Q request on LE definition still answered BE). Partial plain command (e.g., 'p' + 3 bytes) waits, later bytes complete it. Abandoned partial frame is dropped after 400 ms idle (test hook: back-date `partial_since`). Unknown command in enveloped mode -> `[0x83]`. Write outside page -> `[0x84]` (enveloped) and memory unchanged. Dropped link -> read/write error until restored. Cloned channels share state (in C: one object). Enveloped definition never dispatches bare 'A' fragment; Plain definition never treats a frame as envelope.
- Integration (needs `demo.ini` + INI parser): handshake signature == def.signature; realtime block len == och_block_size using `O%2o%2c`; every page readable/writable; och block animates over time; WOT mode raises rpm; LE-framed request answered in LE; frame whose length byte looks like command ('A' = 0x41) not split; lambda from sim within AutoTune range.
