# Feature gap list: LibreTune (Rust/Tauri/React) vs LibreTune-C

Scope: everything the original at `ref/LibreTune/` (refreshed to upstream, 2026-11-16 entries in its
CHANGELOG) offers that this rewrite does not have, or has only in part. Method: the original's
README, CHANGELOG, `docs/src/**`, every Tauri command module
(`crates/libretune-app/src-tauri/src/commands/*.rs`, 89 files), the React components, menus
(`src/menus/buildMenuItems.ts`), shortcuts (`components/HotkeyManager.ts`, `docs/src/reference/shortcuts.md`),
the settings struct (`commands/app_settings.rs`) and `crates/libretune-core/src/**` were inventoried; each
feature was then checked against `src/` by reading the code and by grepping for callers (a core function
that exists in `src/<module>/` but is never called from `src/app/` counts as a UI gap, not as done).

Key finding: the C core libraries are far ahead of the app layer. These public functions exist and are
tested, but have **no caller in `src/app/` or `src/main.c`**: `lt_snap_*` (version history),
`lt_tune_diff_compare` / `lt_tune_merge_selected`, `lt_migration_*`, `lt_math_*`, `lt_dyno_*`,
`lt_therm_*` / `lt_afr_corr_*` / `lt_linear_wb_*` / `lt_cal_encode_*` (sensor calibration),
`lt_pin_detect`, `lt_proto_logger_*` (tooth/composite), `lt_table_file_*` (.table files),
`lt_table_size_*` / `lt_dyn_*` (resize), `lt_ini_repo_scan_dir/import/find_signature`,
`lt_proto_signature_mismatch`, `lt_proto_stats_get`, `lt_anomaly_detect`, `lt_predict_cells`,
`lt_detect_delay` / `lt_fit_flow_delay`, `lt_ae_*`, `lt_dash_validate`, `lt_action_*`, `lt_restore_prune`,
unit conversions (`lt_c_to_f`, `lt_kpa_to_psi`, `lt_afr_to_lambda`). Most P0/P1 items below are therefore
"write a screen", not "write an algorithm".

Legend. Status: MISSING (nothing usable), PARTIAL (works but incomplete; the gap is stated).
Priority: P0 = core tuning workflow, users hit it immediately; P1 = expected by anyone migrating;
P2 = nice to have / niche. Effort: S (<= 2 days), M (3-7 days), L (1-3 weeks), XL (> 3 weeks), for one
agent, including tests. "Ours" paths are suggestions; new screens should be one `.c` per owner so batches stay disjoint.

Orig = path under `ref/LibreTune/crates/`. `app/` = `libretune-app/src/components`, `cmd/` = `libretune-app/src-tauri/src/commands`,
`core/` = `libretune-core/src`.

---------------------------------------------------------------------------------------------------

## 1. Connection / Protocol

### G-01 Signature-mismatch dialog and matching-INI offer
- Status: PARTIAL | Priority: P0 | Effort: M
- Have: `dialogs_conn.c` (`sig_mismatch`, lines ~121 and ~147) only raises a warning toast.
- Missing: modal on connect/scan when ECU signature != loaded INI: list repository INIs whose signature
  matches (`lt_ini_repo_find_signature`, unused), "switch this project to that INI", "continue anyway",
  "search online" (G-17), "import INI file". `lt_proto_signature_mismatch` is never called.
- Orig: `app/dialogs/SignatureMismatchDialog.tsx`, `cmd/find_inis.rs`, `cmd/signature_helpers.rs`, `cmd/connect_to_ecu.rs`.
- Ours: new `src/app/dlg_sync.c`; uses `project/repo.c`.

### G-02 Project tune vs ECU tune choice on connect (sync)
- Status: MISSING | Priority: P0 | Effort: M
- Have: `connect_io()` in `app.c` reads all pages and calls `lt_tune_from_mem(app->tune, app->mem)`: the
  ECU data silently overwrites the project tune in memory (README "Known limitations"). Offline edits are
  lost on connect.
- Missing: after reading the ECU, compare CRC / diff against project tune; dialog "Use ECU tune / Use project
  tune (write to ECU) / Compare (G-13)"; "write project tune to ECU" path; tune-mismatch view listing
  differing constants; `sync_ecu_data` equivalent when pages change on the ECU.
- Orig: `app/dialogs/TuneMismatchDialog.tsx`, `cmd/tune_mismatch_view.rs`, `cmd/tune_misc.rs`
  (`use_project_tune`, `use_ecu_tune`), `cmd/project_tune_sync.rs` (`write_project_tune_to_ecu`), `cmd/sync_ecu_data.rs`.
- Ours: `src/app/dlg_sync.c` + hook in `connect_io`; `lt_tune_diff_compare`, `lt_proto_write_memory`.

### G-03 "Connect ECU / New Project" wizard
- Status: PARTIAL | Priority: P1 | Effort: S
- Have: Connect dialog with port scan (`lt_disc_run`) and a separate New Project dialog with a repository list.
- Missing: one flow: scan -> signature -> pick best INI from repository (or offer online/import) -> name project -> connect.
- Orig: `app/dialogs/ConnectEcuWizard.tsx` (698 lines), `utils/connectEcuWizard.ts`, `utils/connectionWorkflow.ts`.
- Ours: `dlg_sync.c` (shares G-01 code).

### G-04 Reconnect handling
- Status: PARTIAL | Priority: P1 | Effort: M
- Have: auto-connect when a project opens; on lost link `lt_app_service` disconnects with a toast.
- Missing: automatic reconnect after link drop, after controller commands that reboot the ECU, and after
  firmware update (settings `auto_reconnect_after_controller_command`, `auto_reconnect_after_firmware`);
  resync of pages after reconnect; remembering last good port per app (`last_serial_port`).
- Orig: `src/hooks/useReconnectHandler.ts`, `useAutoConnect.ts`, `useEcuEventListeners.ts`.
- Ours: `src/app/app.c` service loop; new `src/app/reconnect.c`.

### G-05 Per-project connection settings
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: global `settings.port/baud/auto_connect`.
- Missing: port, baud, auto-connect stored in the project (`project.json`/properties) and restored per project.
- Orig: `cmd/project_mgmt.rs` (`update_project_connection`, `update_project_auto_connect`).
- Ours: `src/project/props.c` + settings dialog.

### G-06 Protocol tuning settings and connection metrics
- Status: MISSING | Priority: P2 | Effort: S
- Missing: UI for runtime packet mode override (Auto/ForceBurst/ForceOCH/Disabled), adaptive timing
  enable/stats (core has adaptive timing in `proto.c` but no UI), FOME fast-comms toggle, live connection
  metrics (`lt_proto_stats_get` unused: frames ok/bad, latency, CRC errors).
- Orig: `cmd/adaptive_timing.rs`, `app/layout/ConnectionMetrics.tsx`, settings `runtime_packet_mode`, `fome_fast_comms_enabled`.
- Ours: section in Connection settings page (G-92) + status-bar popover.

### G-07 ECU console parity and controller commands
- Status: PARTIAL | Priority: P1 | Effort: S
- Have: `dialogs_misc.c` console (send, scrollback 200 lines).
- Missing: no entry in the Tools menu (only reachable through an INI target); command history (Up/Down),
  persisted history, clear button, output colouring, FOME fast comms, "Controller Command" confirmation dialog for
  INI `[ControllerCommands]` run from menus (e.g. Enter DFU, reset), Tools items shown only when the INI
  declares support.
- Orig: `app/console/EcuConsole.tsx`, `ControllerCommandDialog.tsx`, `cmd/console.rs`, `cmd/tune_io.rs` (`execute_controller_command`).
- Ours: extend `dialogs_misc.c` or split `dlg_console.c`.

### G-08 Firmware update helper
- Status: MISSING | Priority: P2 | Effort: L
- Missing: detection of DFU / OpenBLT host tools, guidance text per ECU, "enter DFU" command, port-blocker
  release, recovery, progress. Original shells out to `dfu-util` / OpenBLT tools.
- Orig: `app/dialogs/FirmwareUpdateDialog.tsx` (669 lines), `cmd/firmware_update.rs`.
- Ours: needs a process-spawn call in `plat.h` (none today; `lt_plat_open_url` only). See section 14.

---------------------------------------------------------------------------------------------------

## 2. Tune files and projects

### G-10 Tune version history (Git-style) with branches
- Status: MISSING (core present, no UI) | Priority: P1 | Effort: M
- Have: `project/snap.c` (`lt_snap_commit/history/diff_compute/restore/prune`), tested; unused by the app.
- Missing: History panel (list, commit details, diff, restore), manual commit with message, auto-commit on
  save with the `always/never/ask` setting and message format, enable/disable per project, branches
  (create/switch/list; not in `snap.c`, add as named snapshot chains, S-M extra), "has changes" indicator.
- Orig: `app/TuneHistoryPanel.tsx`, `cmd/git.rs`, `core/project/version_control.rs` (libgit2), settings `auto_commit_on_save`, `commit_message_format`.
- Ours: new `src/app/dlg_history.c`; File menu "Tune History...". See section 14 (git).

### G-11 INI library management
- Status: PARTIAL | Priority: P1 | Effort: S
- Have: New Project lists the repository (`lt_ini_repo_open`).
- Missing: Settings -> ECU Definitions: list imported INIs with signature/ECU name, import file, scan a folder
  (`lt_ini_repo_scan_dir`), delete, show storage path; per-project INI path display.
- Orig: `cmd/ini_repository.rs`, `cmd/available_inis.rs`, SettingsDialog "ECU Definitions".
- Ours: `src/app/dlg_inilib.c`.

### G-12 Change a project's INI and tune migration report
- Status: MISSING | Priority: P1 | Effort: M
- Have: core `lt_migration_compare/summary` (tune.h), `lt_tune_stamp_ini`; no caller.
- Missing: "Update project INI" (pick new INI, re-resolve tune), migration report dialog (severity, added /
  removed / changed / range-clamped constants, per-item actions), INI version stamp in tune, clear report.
- Orig: `app/dialogs/MigrationReportDialog.tsx`, `cmd/update_project_ini.rs`, `cmd/tune_migration.rs`, `core/tune/migration.rs`.
- Ours: `src/app/dlg_migrate.c`.

### G-13 Tune file diff and selective merge
- Status: MISSING (core present) | Priority: P1 | Effort: M
- Have: `lt_tune_diff_compare`, `lt_tune_merge_selected`.
- Missing: dialog to pick two tunes (or tune vs ECU vs restore point/snapshot), diff list with filter, merge
  selected constants/pages into current tune.
- Orig: `app/dialogs/TuneFileDiffDialog.tsx`, `cmd/tune_compare.rs` (`compare_tune_files`, `merge_from_tune`).
- Ours: `src/app/dlg_tunediff.c`.

### G-14 Restore points polish
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: create / list / restore / delete (`dialogs_misc.c`), auto point before burn.
- Missing: automatic cleanup (`lt_restore_prune` unused) with a keep-count setting, bulk delete, point details
  (size, signature, change summary), diff against current.
- Orig: `app/dialogs/RestorePointsDialog.tsx`, `cmd/restore_points.rs`.
- Ours: `dialogs_misc.c`.

### G-15 Change annotations
- Status: MISSING | Priority: P2 | Effort: S
- Missing: per-table/cell notes persisted with the tune; panel listing all; marker on annotated tables.
  Core `tune.h` states annotations are not implemented (snap commit has a message annotation only).
- Orig: `app/tuner-ui/AnnotationsPanel.tsx`, `cmd/annotations.rs`.
- Ours: `project/` JSON sidecar + `src/app/dlg_annot.c`.

### G-16 Tune CSV export / import
- Status: MISSING | Priority: P2 | Effort: S
- Orig: `cmd/csv_io.rs` (`export_tune_as_csv`, `import_tune_from_csv`).
- Ours: `src/tune/tune_csv.c` + File menu entries.

### G-17 Online INI search and download
- Status: MISSING | Priority: P1 | Effort: S (assisted flow) / XL (real HTTPS)
- Missing: search/refresh/download INIs from Speeduino, rusEFI, FOME, epicEFI repos, offered on signature mismatch.
- Orig: `cmd/online_ini.rs`, `core/project/online_repository.rs` (all `https://`, GitHub API), `app/dialogs/OnlineIniDialog.tsx`.
- Approach: section 14 (no TLS: open the source page in the browser, then watch Downloads for new `.ini`).

### G-18 TunerStudio import wizard
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: File -> Import TunerStudio Project (`lt_project_import_ts`).
- Missing: preview step (what will be imported: tunes, INI match, dashboards, restore points), INI-not-found
  handling, dashboard import (G-40), tune-only import.
- Orig: `app/dialogs/ImportProjectWizard.tsx`, `cmd/ts_import.rs`.

### G-19 Tune open/save dialogs and new tune
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: generic file dialog for load/save.
- Missing: list of tunes in the project folder (`list_tune_files`), load from another project, New Tune
  (defaults) from the menu, tune info (signature, INI, date), msq info preview.
- Orig: `app/dialogs/OpenTuneDialog.tsx`, `tuner-ui/dialogs/{LoadDialog,SaveDialog,NewTuneDialog}.tsx`, `cmd/tune_info.rs`, `cmd/project_misc.rs`.

---------------------------------------------------------------------------------------------------

## 3. Table / curve editing

### G-20 Table selection basics, context menu, keys
- Status: PARTIAL | Priority: P0 | Effort: S
- Have (`tables_table.c`): arrows/click/drag select, `= + - > < q w * / h v s`, Ctrl+C/V/Z/Y/R/D, `f`, `g`.
- Missing: right-click context menu (Set Equal, Increase/Decrease, Interpolate H/V, Smooth, Fill row/col,
  Copy/Paste, Toggle heatmap, Reset to original); Ctrl+A select all; Delete/Backspace clear to bin/zero;
  Escape = revert selection to original; Shift+arrow / shift-click range extension (no `LT_MOD_SHIFT` selection
  handling found); paste special (Ctrl+Shift+V); `3` toggles 3D; Tab next table.
- Orig: `app/tables/TableContextMenu.tsx`, `app/tuner-ui/table-editor/TableContextMenu.tsx`, `TableEditor.tsx`, `docs/src/features/table-editing/shortcuts.md`.
- Ours: `tables_table.c` (`lt_ui_popup`/`menu_item` pattern as in `dash_tab.c:575`).

### G-21 Set table size (resize)
- Status: MISSING (core present) | Priority: P1 | Effort: M
- Have: `lt_table_size_*`, `lt_dyn_*` (dynamic table allocation) in tableops, no caller.
- Missing: dialog to change X/Y counts for resizable VE / ignition / lambda tables, with memory-budget check,
  axis regeneration, write-through to INI constants that carry sizes.
- Orig: `app/dialogs/SetTableSizeDialog.tsx`, `cmd/table_ops.rs` (`resize_table_size`), `core/dynamic_table.rs`.
- Ours: new `src/app/tables_size.c`.

### G-22 Generate table from engine specs (in editor)
- Status: MISSING | Priority: P1 | Effort: S
- Have: base-map wizard (`dialogs_misc.c`) generates whole tunes.
- Missing: toolbar "Generate..." for one table (VE/ignition/AFR) using stored engine specs, preview, apply.
- Orig: `app/dialogs/GenerateTableDialog.tsx`, `cmd/generate_table.rs`, `utils/tableGenerator.ts`.
- Ours: reuse `lt_basemap_generate`; `src/app/tables_gen.c`.

### G-23 TunerStudio `.table` import/export
- Status: MISSING (core present) | Priority: P1 | Effort: S
- Have: `lt_table_file_parse/write`.
- Missing: toolbar Load / Save table to file (with axis handling, size mismatch prompt).
- Orig: `cmd/table_file_io.rs`, `core/table_file.rs`, TableToolbar buttons.
- Ours: `src/app/tables_io.c`.

### G-24 Configurable increments and "set to value"
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: Scale dialog (multiply/add); increment fixed at 1 and a small step derived from digits.
- Missing: user-set increment amount, `=` with a user parameter value, TableAdjustDialog.
- Orig: `app/tables/TableAdjustDialog.tsx`, `utils/tableAdjustInput.ts`, `components/dialogs/CellEditDialog.tsx`.

### G-25 Curve editor parity
- Status: PARTIAL | Priority: P1 | Effort: S
- Have: `tables_curve.c` plot, vertical drag, undo/redo, live marker, burn.
- Missing: horizontal (X) drag of points (README limitation), numeric grid of X/Y values with direct entry and
  the same op set as tables (interpolate, smooth, scale), copy/paste, Y-axis auto-range options.
- Orig: `app/curves/CurveEditor.tsx`, `cmd/curve_ops.rs`.

### G-26 3D view parity
- Status: PARTIAL | Priority: P2 | Effort: M
- Have: software 3D surface with rotate, live point, trail.
- Missing: zoom, pan, keyboard controls, surface style / wireframe / grid options, click-to-select cell from 3D,
  edit-in-3D (README "no zoom/pan").
- Orig: `app/tables/TableEditor3D.tsx`, `tables/3d/SceneComponents.tsx`, `docs/src/features/table-editing/3d-visualization.md`.

### G-27 Lambda preview table
- Status: MISSING | Priority: P2 | Effort: S
- Missing: companion read-only table showing the lambda equivalent of an AFR table (and reverse), honouring the
  AFR/lambda unit setting (G-90). Conversions exist (`lt_afr_to_lambda`).
- Orig: `app/tables/LambdaPreviewTable.tsx`.

### G-28 Large-change alert on table edits
- Status: MISSING | Priority: P2 | Effort: S
- Missing: warn when an edit/paste/op changes a cell beyond absolute or percent thresholds (settings in G-96).
- Orig: settings `alert_large_change_*`, `docs/src/features/alert-rules.md`.

---------------------------------------------------------------------------------------------------

## 4. Dialogs / INI features

### G-30 Temperature sensor calibration (std_ms2gentherm)
- Status: MISSING (core present) | Priority: P0 | Effort: M
- Have: `project/calib.c` (`lt_therm_fit_points`, presets, `lt_cal_encode_temperature`); never called.
  `lt_app_open_target` sends `std_ms2gentherm` to the "Unsupported menu target" toast.
- Missing: dialog: CLT/IAT selection, thermistor presets, 3-point Steinhart-Hart fit, bias resistor, preview
  curve, write 1024-entry table to ECU (units C/F handling).
- Orig: `app/dialogs/TemperatureCalibrationDialog.tsx`, `cmd/sensor_calibration.rs`, `cmd/temperature_units.rs`, `core/calibration/mod.rs`.
- Ours: `src/app/dlg_calib.c` + std-target registry (G-100).

### G-31 AFR / O2 sensor calibration (std_ms2geno2)
- Status: MISSING (core present) | Priority: P0 | Effort: M
- Have: `lt_linear_wb_*`, `lt_afr_corr_*`, `lt_cal_encode_o2`; unused.
- Missing: dialog: preset wideband list, custom voltage/AFR points, preview, write calibration table.
- Orig: `app/dialogs/AfrCalibrationDialog.tsx` (448 lines), `cmd/sensor_calibration.rs` (`write_afr_calibration`, `preview_afr_calibration`).
- Ours: `src/app/dlg_calib.c`.

### G-32 INI help topics
- Status: PARTIAL | Priority: P1 | Effort: S
- Have: INI `help` entries parsed; field hint icons.
- Missing: Help menu items from the INI only toast "Help topic: X" (`app_shell.c:285`); no viewer for topic text
  lines, web URL (open with `lt_plat_open_url`, http/https only), related links.
- Orig: `app/dialogs/HelpViewer.tsx`, `cmd/ini_dialogs.rs` (`get_help_topic`), `utils/urlSafety.ts`.
- Ours: `src/app/dlg_help.c`.

### G-33 Port editor and pin-conflict warnings
- Status: MISSING (placeholder; INI `[PortEditor]` parsed, `lt_pin_*` present) | Priority: P1 | Effort: L
- Missing: `std_port_edit` screen: ports list from INI, per-port assignment combos, conflict detection
  while assigning (`lt_pin_conflict_if_assigning` is called once in `dialogs_ini.c` only for dialog fields),
  save assignments to constants, unassigned markers.
- Orig: `app/hardware/PortEditor.tsx`, `src-tauri/src/port_editor.rs`, `cmd/pin_conflicts.rs`, `core/pin_conflict.rs`, `docs/src/technical/port-editor.md`.
- Ours: `src/app/dlg_port.c`.

### G-34 Dialog component extras
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: `dialogs_ini.c` handles label, panel, field, checkbox, slider, settingSelector, runtime value, indicator,
  live graph, command button, table link.
- Missing: embedded gauge components (`LT_COMP_GAUGE` falls into `default: break`), wideband live status panel,
  indicator-panel layout options, `show_all_help_icons`.
- Orig: `app/dialogs/PanelComponents.tsx`, `DialogRenderer.tsx`, `hardware/WidebandLiveStatus.tsx`, `DialogValueSource.tsx`.

### G-35 Performance / economy calculator
- Status: MISSING | Priority: P2 | Effort: M
- Missing: vehicle specs dialog (weight, frontal area, Cd, gearing), HP/torque, acceleration times,
  fuel-economy figures from live channels.
- Orig: `app/dialogs/PerformanceFieldsDialog.tsx` (565 lines), `docs/src/features/performance-calculator.md`.
- Ours: `src/app/dlg_perf.c` (pure math, no deps).

---------------------------------------------------------------------------------------------------

## 5. Dashboards / gauges

### G-40 Dashboard management
- Status: PARTIAL | Priority: P1 | Effort: M
- Have (`dash_tab.c`): source combo (INI frontpage, basic, tuning, telemetry templates, file), Open .dash,
  Save / Save As, designer.
- Missing: dashboard library in project: new, rename, duplicate, delete, export, import (with compatibility
  report against the INI channels), selector overlay with categories, remember selected dashboard, reset to
  defaults, conflict check on import, import from TunerStudio project.
- Orig: `app/dashboards/components/DashboardSelectorOverlay.tsx`, `DashboardManagementDialogs.tsx`, `hooks/useDashboardCRUD.ts`,
  `cmd/dash_files.rs`, `cmd/dash_layout.rs`, `app/dialogs/ImportDashboardDialog.tsx`.

### G-41 Designer parity
- Status: PARTIAL | Priority: P1 | Effort: L
- Have: drag/resize, add gauge/indicator, property panel, painter selection (20 painters), duplicate, remove, peak-hold, zones.
- Missing (README): undo/redo, copy/paste, layers panel, background image and embedded fonts / asset manager,
  click actions, grid snap / free move / lock settings, gauge palette by category and channel browser, drag from
  sidebar, multi-select, alignment tools, auto-sync gauge ranges from INI.
- Orig: `app/dashboards/designer/*` (PropertyEditor, LayerPanel, AssetManager, GaugePalette, `useDesignerHistory.ts`, `useDesignerKeyboard.ts`), settings `gauge_snap_to_grid`, `gauge_free_move`, `gauge_lock`, `auto_sync_gauge_ranges`.

### G-42 Dashboard validation panel
- Status: MISSING (core present) | Priority: P1 | Effort: S
- Have: `lt_dash_validate` (unused by app).
- Missing: validation panel (errors vs warnings, click to select offending gauge), compatibility bar showing
  unresolved channels for the current INI.
- Orig: `app/dashboards/components/ValidationPanel.tsx`, `CompatibilityBar.tsx`, `cmd/dash_files.rs` (`validate_dashboard`), `docs/src/features/dashboards/validation.md`.

### G-43 Full-screen dashboard
- Status: MISSING | Priority: P1 | Effort: S
- Missing: F11 full-screen toggle and dashboard kiosk mode (hide menus/sidebar/status). No backend op exists in
  `gui.h` (`lt_backend` has `set_title` but nothing for full screen); add `set_fullscreen` to x11/cocoa/win32/web/tty.
- Orig: `docs/src/features/dashboards/using.md` "Full-Screen Mode", `shortcuts.md` (F11).

### G-44 Gauge interaction extras
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: context menu (properties, painter, reset peak, duplicate, remove, designer mode).
- Missing: reload default gauges, gauge demo / sweep toggle, antialiasing toggle, reset value, hover tooltips,
  double-click action.
- Orig: `app/dashboards/GaugeContextMenu.tsx`, `hooks/useGaugeDemo.ts`, `useGaugeSweep.ts`.

### G-45 Dashboard display settings
- Status: MISSING | Priority: P2 | Effort: S
- Missing: refresh cap (10..30 Hz; core has `lt_dash_view_set_redraw_hz`), right-align numeric values, indicator
  column count / fill / text-fit.
- Orig: settings `dashboard_refresh_hz`, `gauge_right_align_values`, `indicator_column_count`, `indicator_fill_empty`, `indicator_text_fit`.

---------------------------------------------------------------------------------------------------

## 6. AutoTune

### G-50 Multi-table AutoTune sessions
- Status: PARTIAL | Priority: P1 | Effort: M
- Have: one tunable table at a time with a target AFR table (`tables_autotune.c`).
- Missing: primary + secondary tables in one session, load-source selection (MAP / TPS / Alpha-N detection),
  keeping per-slot recommendations when switching, "Send & Switch / Discard & Switch / Cancel" guard, update
  mode (send to RAM vs send+burn).
- Status update: DONE: secondary table runs in parallel with per-slot recommendations, Send/Discard/Cancel guard, load-source detection, RAM vs RAM+burn update mode
- Orig: `app/tuner-ui/AutoTune.tsx` (2144 lines), `cmd/start_autotune.rs`, `cmd/autotune_misc.rs`.

### G-51 AFR delay test
- Status: MISSING (core present) | Priority: P1 | Effort: M
- Have: `lt_detect_delay`, `lt_fit_flow_delay`, `lt_delay_table_*`.
- Missing: dialog that drives a controlled fuel step through the ECU (reqFuel read, abort, safety limits),
  delay trace overlay, per-flow delay map, save model for AutoTune.
- Status update: DONE: Tools > AFR Delay Test (warmup-slot step, trace overlay, per-flow map, afr_delay_model.json read by AutoTune); the demo simulator has no AFR response so steps reject there
- Orig: `app/dialogs/AfrDelayTestDialog.tsx`, `DelayTraceOverlay.tsx`, `cmd/afr_delay_test.rs`, `cmd/autotune_preflight.rs` (`save_delay_model`), `core/autotune/delay_measure.rs`.
- Ours: `src/app/dlg_afrdelay.c` (owned with G-50, same autotune files).

### G-52 Heat map views and trace
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: recommendation grid with heat colouring, strip.
- Missing: switchable weighting / change / coverage maps, zoom and pan, trace line of visited cells in the grid, Y flip.
- Status update: PARTIAL: value/weighting/change/coverage views, trace line, Y flip; zoom and pan not done
- Orig: `docs/src/features/autotune/recommendations.md`, CHANGELOG 2026-11-02.

### G-53 Tune health, anomalies, predicted fills
- Status: PARTIAL | Priority: P2 | Effort: M
- Have: health card (`lt_health_score`).
- Missing: anomaly list/overlay (`lt_anomaly_detect`), predicted fills for empty cells (`lt_predict_cells`),
  dyno table overlay, "Tune Health" tab.
- Status update: PARTIAL: anomaly outline and list, predicted fills (show/apply); dyno overlay not done
- Orig: `app/tuner-ui/TuneHealth.tsx`, `cmd/tune_health.rs`, `core/autotune/{anomaly,predictor,health}.rs`.

### G-54 AutoTune export and proposals
- Status: MISSING | Priority: P2 | Effort: S
- Missing: CSV export of recommendations, build/save a proposal file, send-only vs burn actions.
- Status update: DONE: CSV export, proposal JSON, send-only vs send+burn
- Orig: `cmd/autotune_export.rs`, `cmd/autotune_misc.rs`.

### G-55 Offline log analysis report
- Status: PARTIAL | Priority: P2 | Effort: M
- Have: "Replay log..." applies a log through AutoTune (`lt_replay`).
- Missing: analysis report (sample weighting, "which samples count", why dropped, validation against unseen
  samples), accel-enrichment analysis (`lt_ae_*` unused), declared analyze filters from INI.
- Status update: PARTIAL: replay report (counted/dropped/validation), accel-enrichment analysis, INI filter count; per-sample verdict table not shown
- Orig: `app/tuner-ui/DatalogViewer.tsx`, `cmd/analyse_log.rs`, `cmd/analyze_filters.rs`, `core/autotune/{replay,accel_enrich}.rs`.

---------------------------------------------------------------------------------------------------

## 7. Data logging and analysis

### G-60 Math channels
- Status: MISSING (core present) | Priority: P1 | Effort: M
- Have: `lt_math_load/save/compile/apply_to_log`, hooks.
- Missing: editor dialog (name, units, expression, validate), persistence per project, evaluation of math
  channels during recording, in the viewer and in gauges/dashboards.
- Orig: `app/dialogs/MathChannelsDialog.tsx`, `cmd/math_channels.rs`, `docs/src/features/math-channels.md`.
- Ours: `src/app/dlg_math.c`.

### G-61 Smart / auto recording
- Status: MISSING | Priority: P1 | Effort: S
- Missing: start/stop recording on key-on / key-off by RPM threshold (50-500) and timeout (1-10 s), quick toggle in
  logger, settings.
- Orig: settings `auto_record_enabled`, `key_on_threshold_rpm`, `key_off_timeout_sec`, `docs/src/features/smart-recording.md`.
- Ours: needs a service hook in `lt_app_service` (G-100).

### G-62 Data statistics panel
- Status: MISSING | Priority: P2 | Effort: S
- Missing: per-channel mean, median, std dev, min/max, P25/P75, IQR for live window or loaded log / selection.
- Orig: `app/tuner-ui/LoggerStatsPanel.tsx`, `docs/src/features/data-statistics.md`.

### G-63 Alert rules
- Status: MISSING | Priority: P2 | Effort: S
- Missing: threshold rules on channels with toasts / log markers, defaults, enable toggles.
- Orig: `docs/src/features/alert-rules.md`, SettingsDialog "Alert Rules".

### G-64 Log formats
- Status: PARTIAL | Priority: P2 | Effort: M
- Have: CSV and MLG v2 read/write (deliberate spec deviation noted in `datalog.h`).
- Missing: native `.ltlog` read (binary, with compression; opening logs recorded by the original), TunerStudio `.msl`
  text import. Recommendation: do `.msl` (S) and `.ltlog` read-only only if its layout is simple enough after reading
  `core/datalog/ltlog.rs`; skip zstd (see section 14).
- Orig: `core/datalog/{ltlog,mlg,format}.rs`, `docs/src/features/datalog.md`.

### G-65 Logger / viewer parity
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: record start/stop/marker, viewer with playback/zoom/speed/loop, CSV export, drive gauges.
- Missing: sample-rate setting (`lt_rec_set_sample_rate` unused), channel selection for recording, delete/rename
  log files, markers shown on timeline, synchronized cursor to table editor / dashboards, MLG export button.
- Orig: `app/tuner-ui/{DataLogView,GraphLog,DatalogViewer}.tsx`, `cmd/data_logging.rs`.

---------------------------------------------------------------------------------------------------

## 8. Diagnostics / loggers

### G-70 Tooth logger
- Status: MISSING (placeholder tab) | Priority: P1 | Effort: L
- Have: `lt_proto_logger_start/read/stop`, INI `[LoggerDefinition]` parsed (`n_loggers`); `special_draw` shows
  the placeholder.
- Missing: capture control, decode of record layout from INI, tooth timing plot, zoom/cursors, CSV export, stats
  (missing/extra tooth detection).
- Orig: `app/diagnostics/ToothLoggerView.tsx`, `cmd/tooth_logger.rs`, `core/ini/diagnostic_logger.rs`, `docs/src/features/diagnostic-loggers.md`.
- Ours: `src/app/dlg_loggers.c` (shared plot with G-71).

### G-71 Composite logger
- Status: MISSING | Priority: P1 | Effort: M (after G-70)
- Missing: multi-signal digital trace (trigger, sync, injectors, ignition), cursors, export.
- Orig: `app/diagnostics/CompositeLoggerView.tsx`.

---------------------------------------------------------------------------------------------------

## 9. Tools

### G-80 Table comparison
- Status: MISSING | Priority: P2 | Effort: M
- Missing: compare two tables (current vs restore point / other tune / ECU), difference and percent views, heat map.
- Orig: `app/dialogs/TableComparisonDialog.tsx`, `cmd/table_compare.rs`, `docs/src/features/tools.md`.

### G-81 Dyno overlay
- Status: MISSING (core present) | Priority: P2 | Effort: M
- Have: `lt_dyno_from_csv/compare/compute_peaks/map_to_table`.
- Missing: load dyno CSV (header detection), compare runs, peaks, overlay on VE table.
- Orig: `app/tuner-ui/DynoOverlay.tsx`, `cmd/dyno.rs`.

### G-82 Action recorder / manager (action scripting)
- Status: MISSING (core present) | Priority: P2 | Effort: M
- Have: `project/action.c` (`lt_action_*`, record/validate/serialize).
- Missing: recorder UI, record while editing tables/constants, action list, tags, compatible-ECU metadata,
  playback with INI validation, export/import.
- Orig: `app/ActionRecorder.tsx`, `ActionManagement.ts`, `core/action_scripting.rs`, `docs/src/reference/action-scripting.md`.

---------------------------------------------------------------------------------------------------

## 10. Settings / preferences

### G-90 Unit preferences applied
- Status: PARTIAL | Priority: P0 | Effort: M
- Have: Temperature C/F, pressure kPa/psi/bar, AFR/lambda stored (`settings_draw`); converters in tableops.
- Missing: README admits "stored but not applied": no screen converts displayed values (tables, dashboards,
  dialogs, status, logs, calibration); also Kelvin, inHg, speed (km/h vs mph), and writing back in ECU units.
- Orig: `src/contexts/useUnitPreferences.tsx`, `utils/unitConversions.ts`, `core/unit_conversion.rs`, `cmd/temperature_units.rs`.
- Ours: one shared `lt_units_*` helper used by `tables_*`, `dialogs_ini.c`, `dash`, `gauges.c` (touches many files; do as its own batch).
- Status update: DONE: `lt_unit_detect`/`lt_uconv_*` (tableops_units.c) detect the native unit from the INI string and convert to the preference (C/F/K, kPa/psi/bar/inHg, AFR/lambda, km/h/mph). Applied live to: table/curve editors (axes, cells, limits, plot axes; edits stored in the ECU unit, preference change converts open edits and undo), INI dialog fields/sliders/runtime values/live graphs, dashboard gauges (value, range, zones, peak, unit label, digits), status-bar channels, datalog logger/viewer/stats, AutoTune target AFR, calibration dialog default. Log files and rule thresholds stay in ECU units.

### G-91 Hotkey editor
- Status: PARTIAL | Priority: P2 | Effort: M
- Have: static shortcuts list in Settings and F1.
- Missing: user-rebindable shortcuts (table ops and global), conflicts check, persistence, reset.
- Orig: `app/dialogs/HotkeyEditor.tsx`, `HotkeyManager.ts`, `services/hotkeyService.ts`, `cmd/hotkeys.rs`.
- Ours: shortcut registry (G-100) + `dlg_hotkeys.c`.

### G-92 Settings search and missing sections
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: General (units), Appearance, Connection, Keyboard Shortcuts tabs.
- Missing: search across sections, Version Control, Data Logging (auto record), Alert Rules, Dashboard, Indicator
  Panel, Status Bar, Project Settings, ECU Definitions (G-11), AI, MCP, Language.
- Orig: `app/tuner-ui/dialogs/SettingsDialog.tsx` (sections at lines 688-1404).

### G-93 Table display preferences and custom heat-map stops
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: 6 heat-map schemes, UI scale, per-tab Y flip.
- Missing: persisted Y-axis-bottom, cursor colour, trail colour and fade seconds, three heat-map contexts
  (value / change / coverage) each with scheme or custom colour stops, font size, grid options.
- Orig: settings `table_y_axis_bottom`, `table_cursor_color`, `table_trail_color`, `table_trail_fade_sec`, `heatmap_*`, `cmd/settings.rs` (`update_heatmap_custom_stops`).

### G-94 Session restore and window geometry
- Status: MISSING | Priority: P2 | Effort: S
- Missing: restore open tabs, active tab, sidebar expanded ids, selected dashboard; window size/position
  (settings struct has none).
- Orig: settings `open_tabs`, `last_active_tab`, `sidebar_expanded_ids`, `selected_dashboard`.

### G-95 Localization
- Status: MISSING | Priority: P2 | Effort: L
- Missing: string catalogue, language setting, translated menus (with mnemonics), en / pt-BR / hu-HU. Need
  glyph coverage check in the embedded fonts (hu needs Latin Extended-A).
- Orig: `src/i18n/` (locales en, hu-HU, pt-BR).
- Ours: `src/lt_i18n.c` with a flat key table + `locales/*.txt` embedded as C arrays; every UI string through `lt_tr()`.

### G-96 Misc behaviour toggles
- Status: MISSING | Priority: P2 | Effort: S
- Missing: auto-burn on close, ask/always/never auto-commit, large-change alert thresholds, show help icons everywhere,
  demo-mode toggle in settings (ours is a File menu item), onboarding flag.

---------------------------------------------------------------------------------------------------

## 11. UI shell / navigation / shortcuts

### G-100 Shell extension points (enabler for parallel work)
- Status: MISSING | Priority: P0 | Effort: S
- Today every new feature has to edit `app.c` (`lt_app_open_target`, service loop) and `app_shell.c`
  (`menu_tools`, `menu_file`, `shortcuts`, `app_dialogs`) and `dialogs_conn.c` (connect, settings, new project all
  in one file). Parallel agents would conflict.
- Needed: registries for std-target handlers (`std_ms2gentherm`, `std_port_edit`, ...), Tools/File/Help menu
  items (with capability predicate), global shortcuts, settings pages, per-frame service hooks (auto-record,
  alerts), post-connect hooks; split `dialogs_conn.c` into `dialogs_connect.c`, `dialogs_settings.c`,
  `dialogs_newproj.c` (pure move).

### G-101 In-app manual, quick start, onboarding
- Status: MISSING | Priority: P2 | Effort: M
- Missing: Help -> User Manual viewer (searchable), quick-start guide, first-run onboarding. Ours: shortcuts box
  and About only.
- Orig: `app/dialogs/{UserManualViewer,QuickStartGuide,OnboardingDialog}.tsx`, `docs/src/**` (bundled).
- Ours: embed a trimmed manual as text at build time + a tiny markdown renderer in `view_kit.c`.

### G-102 Error details dialog
- Status: MISSING | Priority: P2 | Effort: S
- Missing: expandable copyable error dialog for failures (protocol detail from `lt_proto_errinfo`); we only toast.
- Orig: `app/dialogs/ErrorDetailsDialog.tsx`, `common/ErrorBoundary.tsx`.

### G-103 Tab management
- Status: PARTIAL | Priority: P2 | Effort: S
- Have: Ctrl+Tab, Ctrl+1..9, Ctrl+W, dirty dot.
- Missing: drag-reorder, tab context menu (close others, pop out), protected tabs.
- Orig: `app/tuner-ui/TabBar.tsx`, `TabContentRouter.tsx`.

### G-104 Status-bar channel selector
- Status: MISSING | Priority: P2 | Effort: S
- Missing: user-chosen (max 8) live channels in the status bar, persisted.
- Orig: `app/dialogs/StatusBarChannelSelector.tsx`, settings `status_bar_channels`, `cmd/channels.rs` (`get_status_bar_defaults`).

### G-105 Menu parity
- Status: PARTIAL | Priority: P1 | Effort: S
- Have: File/Edit/View/Tools/Help + ECU menus with mnemonics.
- Missing: Settings is only in Tools (needs an INI) and not in File on the welcome screen (Ctrl+, works);
  Tools lacks Console, Tune File Diff, Table Compare, Dyno, Math Channels, AFR Delay Test, Plugins, AI Assistant,
  Check for Updates, Firmware Update, Enter DFU; File lacks Search INI Online, Tune History; Help lacks User Manual.
  Edit menu Undo/Redo/Cut/Copy/Paste show a toast (the original does the same, so parity, but real routing to the
  active tab is cheap once G-100 exists).
- Orig: `src/menus/buildMenuItems.ts`, `buildToolbarItems.tsx`.
- Ours: filled in as each feature lands, via G-100 registries.

---------------------------------------------------------------------------------------------------

## 12. Platform / window

### G-110 Pop-out windows (multi-monitor)
- Status: MISSING | Priority: P2 | Effort: XL
- Missing: detach any tab into its own OS window with live sync back; protected tabs; dock back. The GUI is one
  surface per process (`lt_backend` single window in x11/cocoa/win32).
- Orig: `src/PopOutWindow.tsx`, `hooks/useTabPopout.ts`, `docs/src/features/popout-windows.md`.
- Approach: section 14.

### G-111 Update check
- Status: MISSING | Priority: P2 | Effort: S (link) / XL (in-app)
- Missing: Tools -> Check for Updates, startup check, version badge in status bar (ours shows version in About),
  signed in-app install.
- Orig: `tuner-ui/dialogs/UpdateSection.tsx`, tauri updater.
- Approach: section 14.

---------------------------------------------------------------------------------------------------

## 13. Extensibility / scripting / AI

### G-120 Lua console
- Status: MISSING | Priority: P2 | Effort: L
- Missing: sandboxed Lua 5.4 console (print, return value, table/string/math libs only, memory and instruction
  limits). The original exposes no tune/ECU API to the console.
- Orig: `core/lua/mod.rs` (mlua), `cmd/lua.rs`, `app/console/LuaConsole.tsx`, `docs/src/features/lua-scripting.md`.
- Approach: section 14.

### G-121 ECU Lua editor (rusEFI)
- Status: MISSING | Priority: P2 | Effort: S
- Missing: editor that writes the script into the INI's `luaScript` string constant and burns (no interpreter needed;
  runs on the ECU).
- Orig: `app/console/LuaConsole.tsx` (lines ~91-95: `update_constant_string`, `burn_to_ecu`); shown when the INI has `lua_script_constant`.
- Ours: `src/app/dlg_lua_ecu.c`; needs a multi-line text widget (check `lt_ui_text_multi` availability in `ui.h`, else M).

### G-122 WASM plugin system
- Status: MISSING | Priority: P2 | Effort: XL
- Missing: load/unload/list/execute `.wasm` plugins, permission model (read tables, read/write constants,
  subscribe channels, execute actions), plugin panel UI.
- Orig: `core/plugin_system.rs`, `core/plugin_api.rs`, `cmd/wasm_plugin.rs`, `app/PluginPanel.tsx`, `docs/src/technical/plugin-system.md`.
- Approach: section 14 (recommend replacing with Lua-subset plugins).

### G-123 AI assistant
- Status: PARTIAL (local-only form) | Priority: P2 | Effort: XL (cloud) / L (local HTTP only)
- Done: Tools -> AI Assistant tab (`src/app/feat_assistant.c`); Settings > Assistant (enable, risk acknowledgement,
  base URL with Ollama / LM Studio presets, model, API key, capability tier); OpenAI-compatible
  `/chat/completions` client over plain HTTP (`src/net/http.c`, SSE streaming or one JSON document, polled once per
  frame, Stop button, 6-round tool loop); the tool catalogue (list_tables, read_table, read_constant, list_features,
  tune_health_check, get_realtime_snapshot, query_datalog, propose_table_edit, propose_bulk_operation,
  propose_constant_change) with read / tune / config tiers enforced both in the offered catalogue and at execution;
  proposal review queue (INI validation, AutoTune authority clamping, dangerous-constant flag), Accept / Reject,
  "Stage accepted changes" creates a restore point first and never burns. Config in `<config>/assistant.conf`
  (mode 0600, API key in PLAINTEXT like the original's v1; `LT_AI_KEY` overrides). `https://` is refused with advice
  (no TLS): use a local endpoint or a local TLS-terminating proxy. Tests: `tests/test_agent.c` (fake server in-process).
- Left: summarize_tune_context tool (per-cell AFR error needs datalog correlation; tune_health_check covers table
  statistics only), batch-drift warning at stage time, per-project chat history and chat switcher, docked/popped-out
  panel (it is a tab), token usage per chat is shown but not persisted, "Ask AI" button inside table editors
  (the open table is sent as context instead), native Anthropic / Google protocols (need TLS).
- Orig: `core/agent/*` (2.4k lines), `core/llm/*`, `cmd/agent.rs`, `cmd/ai_keychain.rs`, `app/agent/*`, `docs/src/features/ai-assistant.md`.
- Approach: section 14.

### G-124 MCP server (read-only)
- Status: DONE | Priority: P2 | Effort: M
- Done: `src/mcp/mcp.c`. JSON-RPC 2.0 (initialize, ping, tools/list, tools/call, notifications, batches) over (a) opt-in
  loopback HTTP `POST /mcp` on 127.0.0.1 (Settings > Assistant: switch, port, access token with Show / Regenerate;
  bearer token in `<config>/mcp-token`, 0600, constant-time compare, Host and Origin pinned to loopback, bare 401) and
  (b) stdio: `libretune --mcp --project DIR` / `--ini FILE`. Exposes the read tools of the assistant only; the
  propose_* tools are not listed and are refused. Tests in `tests/test_agent.c`.
- Left: the original also lists summarize_tune_context (see G-123); no SSE/session management (every POST is
  answered in one JSON response, which MCP "streamable HTTP" clients accept).
- Orig: `src-tauri/src/mcp/*`, `app/tuner-ui/dialogs/McpServerSection.tsx`, `docs/src/features/mcp-server.md`.

---------------------------------------------------------------------------------------------------

## 14. Features that collide with the zero-dependency rule

| Feature | Need | Dependency-free approach | Honest effort / caveat |
|---|---|---|---|
| Online INI search (G-17) | HTTPS to GitHub API, rusefi.com, epicefi.com | (1) Assisted: dialog shows the source page, `lt_plat_open_url` opens it, app watches the user's Downloads dir (`lt_plat_list_dir`) for new `*.ini`/`*.zip`-less INIs and offers import via `lt_ini_repo_import`; also `lt_ini_repo_scan_dir` on a user-chosen folder. (2) Ship a signature->INI index snapshot with releases so signature matches can be named even offline. (3) Plain `http://` fetch only for hosts that serve it (most redirect to HTTPS, so do not count on it). | (1)+(2): S-M. Real HTTPS needs our own TLS 1.3 client (X25519, ChaCha20-Poly1305/AES-GCM, SHA-256/384, HKDF, X.509 parsing, RSA/ECDSA verify, embedded root store, hostname check): XL (4-8 weeks) and security-critical; do not ship without certificate verification. Recommend not doing it. |
| Auto-update (G-111) | HTTPS + signature check | "Check for updates" opens the releases page in the browser. Signature verification itself (Ed25519, minisign format) is M if ever needed. | Link: S. In-app download: blocked on TLS (XL). |
| AI assistant (G-123) | HTTPS to OpenAI / Anthropic / Google | Support only OpenAI-compatible endpoints on `http://127.0.0.1` (Ollama, LM Studio, llama.cpp server, or a local TLS-terminating proxy the user runs). Need: HTTP/1.1 client over `lt_tcp_connect`, chunked + SSE parsing, JSON (have `json`), tool-calling loop, proposals UI. Core logic (tiers, validation, authority clamp) is portable from `core/agent/*`. | Local-only: L (2-3 weeks incl. UI). Cloud providers: need TLS or user proxy; document it. |
| API key storage / OS keychain | Keychain APIs (Win Credential Manager, macOS Keychain, Secret Service) | The original's own settings comment says the key is stored in plaintext in v1. Equivalent: store in the config dir with 0600 permissions (`lt_plat` chmod) and prefer an env var (`LT_AI_KEY`); optionally XOR-obfuscate (not security). macOS/Windows keychain calls are possible in `src/plat/<os>` without extra libs (they are system frameworks) but add per-OS code: M each. | S for file+env; M per OS for native keychain. |
| Lua scripting (G-120, G-122 alt) | Lua 5.4 interpreter (mlua vendored) | Write a Lua-subset interpreter in C89: lexer, recursive-descent parser to AST, tree-walking evaluator (or small register VM), tables, closures, varargs, integer/float split, `string` (format, sub, find, match, gsub with Lua patterns), `math`, `table`, `pcall/error`, instruction counter and allocation cap for the sandbox. Goal "runs typical scripts", not conformance. | L (2-3 weeks) for subset; XL for full 5.4 semantics (coroutines, goto, metatables, string.pack). Lua source stays licence-free since we write our own. |
| WASM plugins (G-122) | wasmtime | A WebAssembly MVP interpreter in C: binary decoder, validator, stack interpreter, linear memory, imports for host API with the same permission checks. No JIT, no SIMD/threads/GC. | XL (6-10 weeks incl. validation and fuzzing); existing plugin ecosystem is tiny. Recommend instead: plugins = Lua-subset scripts with the same permission model (M on top of G-120), and drop WASM unless users ask. |
| Git versioning (G-10) | libgit2 | Already done in core as snapshots (`project/snap.c`): content-addressed commit/history/diff/restore without git. Missing only UI and branches. Do not read/write real `.git` repos; offer "export history as patch files" if interop is wanted. | UI: M. Branches on snapshots: S-M. Real git object format (zlib, packfiles, SHA-1): XL, not recommended. |
| Firmware update (G-08) | Spawn `dfu-util`/OpenBLT, USB access | Add `lt_plat_spawn(argv, flags)` (posix `fork/exec`, win32 `CreateProcess`) and drive existing external tools; the app only locates the tool, releases the serial port and shows guidance. Native DFU over USB is out of scope (needs libusb-style per-OS code). | Guidance + spawn: L (cross-OS process handling, tool discovery). Native DFU: XL. |
| Pop-out windows (G-110) | Multiple OS windows | Extend `lt_backend` to N surfaces (x11: second `Window` sharing the display; win32: second HWND; cocoa: second NSWindow; web: a second URL/tab on the existing server; tty: not supported). One `lt_app` state, per-window `lt_ui`/canvas, tab assigned to a window. | XL (3-5 weeks) because the app assumes one `lt_ui`/surface. Do after the registries (G-100). |
| zstd (.ltlog) (G-64) | zstd decompression | Skip `.ltlog`, keep MLG/CSV. If needed, a decompress-only zstd (FSE+Huffman) is ~1.5k lines: L. | Only if users need to open original-app logs. |

---------------------------------------------------------------------------------------------------

## 15. Summary of what is already done (not listed individually)

Verified by reading code: about 24 original feature areas have a working equivalent: INI parse + menus + dialogs
+ tables + curves + front-page gauges; table editor ops (set equal, +/-, scale, interpolate incl. H/V, smooth,
fill, rebin, copy/paste, undo/redo/revert, follow, live cell, trail, 3D view, heat map, Y flip); curve editor
(basic); INI dialog widgets and command buttons; serial/TCP/demo connect with discovery scan, page read, dirty
write, burn with confirmation; built-in ECU simulator; MSQ tune load/save/reset to defaults; restore points
(create/list/restore/delete, auto before burn); projects (create/open/recent/close/delete confirm, TunerStudio import);
dashboards (view, 20 painters, templates, .dash load/save, basic designer, peak hold, zones); data logging
(record, markers, CSV/MLG, viewer, playback, drive gauges); AutoTune core (filters, authority limits, hit weighting,
cell lock, preflight, replay, health card, rebin-aware); base-map wizard; output-channel status; basic console;
9 themes, 6 heat-map schemes, UI scale; sidebar search; menu mnemonics; global shortcuts; tab handling;
x11/cocoa/win32/web/tty backends.

---------------------------------------------------------------------------------------------------

## 16. Recommended implementation order (batches of disjoint work)

Rule: items inside one batch touch disjoint files, so parallel agents do not conflict. A batch starts only after the
previous batch is merged. File ownership is listed per agent; `app.c`, `app_shell.c`, `app.h` are owned by exactly one
agent per batch.

**Batch 0 (serial, one agent, S): enabler.** G-100. Owns `app.c`, `app_shell.c`, `app.h`, `dialogs_conn.c` (split into
`dialogs_connect.c` / `dialogs_settings.c` / `dialogs_newproj.c`), `dialogs_reg.c`. After this, features add new files
and register themselves.

**Batch 1 (P0, 4 parallel agents)**
- A: G-02, G-01, G-03, G-04. Owns `dialogs_connect.c`, new `dlg_sync.c`, `reconnect.c`, the `connect_io` region (via hook).
- B: G-30, G-31. Owns new `dlg_calib.c` (+ `project/calib.c` fixes only).
- C: G-20, G-23, G-21, G-22, G-25, G-24. Owns `tables_table.c`, `tables_curve.c`, `tables_common.c`, new `tables_io.c`, `tables_size.c`, `tables_gen.c`.
- D: G-11, G-12, G-13. Owns new `dlg_inilib.c`, `dlg_migrate.c`, `dlg_tunediff.c`, `project/repo.c`.

**Batch 2 (P1, 5 parallel agents)**
- E: G-40, G-41, G-42, G-43, G-44, G-45. Owns `dash_tab.c`, `dash/*`, `gauges.c`, `dash_gauges.h`, and the backends for G-43 (`gui/gui.h`, `gui/be_*.c`, `gui/os/*`).
- F: G-60, G-61, G-62, G-63, G-65. Owns `dialogs_log.c`, new `dlg_math.c`, `datalog/*`.
- G: G-70, G-71, G-07. Owns new `dlg_loggers.c`, `dialogs_misc.c` (console, placeholders), `proto/` logger reads.
- H: G-10, G-14, G-15, G-16. Owns new `dlg_history.c`, `dlg_annot.c`, `project/snap.c`, `tune/tune_csv.c`, restore dialog.
- I: G-32, G-33, G-34. Owns `dialogs_ini.c`, new `dlg_help.c`, `dlg_port.c`, `project/pin.c`.

**Batch 3 (P1/P2 cross-cutting, 3 agents)**
- J: G-90 units applied. Touches many files; run alone for `tables_*`, `dialogs_ini.c`, `gauges.c`, `dash_view`, status bar. (Run after Batches 1-2 so it does not collide.)
- K: G-50, G-51, G-52, G-53, G-54, G-55. Owns `tables_autotune.c`, `autotune/*`, new `dlg_afrdelay.c`.
- L: G-17 (assisted flow), G-18, G-19, G-05, G-06, G-92, G-93, G-94, G-96, G-104, G-105, G-102. Owns `dialogs_settings.c`, `dialogs_newproj.c`, `app_shell.c` status bar region, `app/settings.c`.

**Batch 4 (P2, 4 parallel agents)**
- M: G-80, G-81, G-82, G-27, G-28, G-35, G-26. New `dlg_tablecmp.c`, `dlg_dyno.c`, `dlg_actions.c`, `dlg_perf.c`, 3D in `tables_table.c` (after C is merged).
- N: G-120, G-121 and later G-122-as-Lua-plugins. New `src/script/` (Lua subset) + `dlg_lua.c`.
- O: G-124, G-123 (local HTTP only). New `src/net/http.c`, `src/mcp/`, `src/agent/`, `dlg_agent.c`.
- P: G-91, G-95, G-101, G-103. `i18n`, `dlg_hotkeys.c`, manual viewer, tab bar.

**Batch 5 (XL / platform, only if demanded)**: G-110 pop-out windows,
G-08 firmware helper, G-111 update link, G-64 `.msl`/`.ltlog`, real TLS (not recommended), WASM runtime (not recommended).
