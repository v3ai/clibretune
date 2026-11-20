# Getting started

A short path from download to a running engine. LibreTune works with rusEFI, FOME, epicEFI,
Speeduino and MegaSquirt ECUs: anything described by a TunerStudio-style INI file.

> Tuning can damage an engine. Change one thing at a time, watch the wideband and knock, and keep
> a copy of a tune that runs before you experiment.

## 1. Try it without an ECU

    libretune --demo

Demo mode runs a simulated ECU inside the app (File > Demo mode does the same). Everything works:
gauges move, tables highlight the live cell, AutoTune collects data and logs record. It is the
safest place to learn the editor.

## 2. Install and connect

- **Linux**: your user needs access to the serial port. On most distributions:
  `sudo usermod -aG dialout $USER`, then log out and back in. For firmware flashing also install the
  udev rule shipped with each release (`50-libretune.rules`; instructions are inside the file).
- **Windows**: ECUs with a USB serial port appear as `COMx`; no driver is needed for rusEFI and
  Speeduino on current Windows. Firmware flashing needs the WinUSB driver, see
  [WINDOWS_USB.md](WINDOWS_USB.md).
- **macOS**: ports appear as `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`. The release binary is not
  signed yet: right-click the app and choose Open the first time.

Then **File > New Project...**: name the project, pick the ECU's INI from the built-in library or a
file, and choose the port. **File > Connect ECU...** connects later; the status bar shows the link
rate.

On connect LibreTune compares the tune in the project with the one in the ECU; when they differ it
asks which one to keep. If in doubt, use the ECU's settings, then **File > Save Tune** so the project
has a copy.

## 3. Find your way around

- The sidebar is the ECU's own menu tree from its INI; **Ctrl+K** searches it.
- Tables: arrow keys or the mouse select, type a number to set, `+`/`-` step, `=` average,
  `/` interpolate, `s` smooth, Ctrl+Z undo. Right-click for every other operation, including the 3D view.
- **View > Dashboard** (Ctrl+D) shows gauges; the Designer button edits them.
- **Help > Keyboard Shortcuts** lists everything; **Help > User Manual** is the full manual.

## 4. Change and burn

Edits go to the ECU's RAM immediately while connected, so you can try a change with the engine
running. They are lost at power-off until you **burn** them: **File > Burn to ECU** (Ctrl+B), or the
flame button. LibreTune checks the tune for pin conflicts before burning.

Save often (Ctrl+S). Restore points (**File > Create Restore Point**) and **File > Tune History...**
let you go back to any earlier tune.

## 5. Log

**Tools > Data Logging** (Ctrl+L) records every channel. **Tools > Datalog Viewer...** opens logs
(`.csv`, `.msl`, `.mlg`) with stacked graph pages, scatter plots and histograms; logs from
TunerStudio open directly.

## 6. AutoTune

With a wideband sensor and a warm engine: **Tools > AutoTune** (Ctrl+Shift+A), choose the VE table,
check the target AFR table and the filter settings, then press Space to start. Drive steadily
through the load and rpm range; cells fill as data arrives. Lock cells you trust with `L`, review the
proposed changes, apply them and burn. **Tools > WUE Analyze** and **Trim Table Analyze** work the same
way for warm-up enrichment and per-cylinder trims.

## 7. Update the firmware

**Tools > Firmware Update...** flashes STM32 ECUs (rusEFI, FOME, epicEFI, miata-ecus) over USB; see
the README's "Firmware update" section. Save your tune first: a firmware update can reset settings.
