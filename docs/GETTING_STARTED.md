# Getting started

A short path from download to a running engine. clibretune works with rusEFI, FOME, epicEFI,
Speeduino and MegaSquirt ECUs: anything described by a TunerStudio-style INI file.

> Tuning can damage an engine. Change one thing at a time, watch the wideband and knock, and keep
> a copy of a tune that runs before you experiment.

## 1. The start screen

With no project open clibretune shows its start screen:

- **Connect to my ECU** finds the ECU, picks its definition (INI) and sets up a project in one go.
- When an ECU is plugged in, a green strip names it ("rusEFI ECU is plugged in on /dev/ttyACM0").
  If this computer has talked to that ECU before, one click opens its project and connects.
- **Recent projects** shows each project's ECU and when it was last opened; `x` takes one off the
  list (the folder stays on disk). Projects are remembered by their full path, so a project opened
  as `./MyCar` from a terminal still opens from the list when clibretune is started elsewhere.
- A project that cannot be opened says why in plain words: its folder is gone, its settings file
  (`project.json`) is missing, empty or damaged, or its ECU definition is missing or unreadable.
  **Show Folder** opens the folder, **Remove from Recent Projects** takes it off the list and, for a
  definition problem, **Replace Definition...** picks one (bundled with clibretune, from the INI library
  or a file), puts it into the project and opens it. A definition that loads but looks incomplete (no
  menus and no dialogs, or settings that do not fit its pages) opens with a warning: replace it, or
  **Continue anyway** to look around (do not burn with it).
- `clibretune <project folder>` (or `--project <folder>`) opens a project straight away, for example
  from a desktop shortcut or a file manager; the folder must hold the project's `project.json`.
- **Try the demo**, **Start from a base tune**, **Import a TunerStudio project**, **Open a project
  folder** and **Set up a project manually** cover the other ways in.
- **Before you connect** lists what a first connection needs, including the exact command when your
  user cannot use serial ports yet. It goes away once an ECU has answered.

## 2. Try it without an ECU

    clibretune --demo

Demo mode runs a simulated ECU inside the app (**Try the demo**, or File > Demo mode). Everything
works: gauges move, tables highlight the live cell, AutoTune collects data and logs record. It is the
safest place to learn the editor.

## 3. Install and connect

- **Linux**: your user needs access to the serial port. On most distributions:
  `sudo usermod -aG dialout $USER`, then log out and back in. For firmware flashing also install the
  udev rule shipped with each release (`50-clibretune.rules`; instructions are inside the file).
- **Windows**: ECUs with a USB serial port appear as `COMx`; no driver is needed for rusEFI and
  Speeduino on current Windows. Firmware flashing needs the WinUSB driver, see
  [WINDOWS_USB.md](WINDOWS_USB.md).
- **macOS**: ports appear as `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`. The release binary is not
  signed yet: right-click the app and choose Open the first time.

Plug in the ECU's USB cable (a data cable: some cables only charge), switch the ignition on and press
**Connect to my ECU**. clibretune looks at USB serial ports first and recognizes common ECU boards by
their USB id: STM32 boards (rusEFI, FOME, epicEFI) as 0483:5740, Arduino Mega (Speeduino) as
2341:0042 / 2341:0010, Teensy, and the usual USB-serial adapters (CH340, FTDI, CP210x, PL2303). Each
port is asked at its likeliest baud rate first, and the window stays responsive while it searches
(**Cancel scan** stops it). Built-in serial ports (`/dev/ttyS*`, COM1) are skipped unless you tick
**Show all ports** in **File > Connection Settings...**.

Once a project is set up, **File > Connect** (Ctrl+Shift+C, or the plug button) connects with the
port and baud rate saved in the project. A project without a saved port, or no project at all, gets
the Connect ECU wizard instead, which finds the ECU first. **File > Connection Settings...** picks
the port, baud rate, a TCP/WiFi address or the simulator by hand.

When something is in the way clibretune says what to do:

- *in use by another program*: close TunerStudio or a serial monitor, only one program can use a port;
- *no permission* (Linux): it shows the command, e.g. `sudo usermod -aG dialout $USER`;
- *the device went away*: the cable came loose or the ECU restarted;
- *no answer*: check power, or the ECU is in its bootloader (flash firmware first);
- *wrong baud rate*: data arrived but made no sense; pick the rate by hand.

The port, baud rate, signature and project of every ECU that answered are remembered per USB serial
number (`devices.conf` in the configuration folder), so the next time the same board is found first,
even under another port name. **File > New Project...** still sets up a project by hand: pick the
INI from the built-in library or a file and choose the port.

### ECU definitions come with clibretune

You do not need to find or download an INI file. clibretune carries 167 definitions: every Speeduino
release since 2017, every board of the last three rusEFI releases, every FOME board of its latest
release and the miata-ecus board. When an ECU answers, clibretune reads its signature and picks the
definition by itself:

- **exact match**: used straight away; a short message says which one;
- **close match** (a rusEFI nightly build, a newer Speeduino than this clibretune knows): the
  nearest build of the same board is used, with a warning that some settings may differ and a
  **Find Exact INI...** button that searches online;
- **nothing fits** (MegaSquirt, other firmware): choose the INI file that came with the firmware or
  search online. MegaSquirt INIs are not bundled because their license does not allow it.
  This covers the MegaSquirt-I family too (B&G 2.x / 3.x and MSnS at 9600 baud, MS1-Extra /
  MSnS-extra). For a MegaTune-era MS1 INI, put the options your firmware was built for at the top of
  the file as `#set` lines (for example `#set SPEED_DENSITY`, `#set CELSIUS`), as MegaTune's
  settings file did; without them some tables have no load axis.

rusEFI F7/H7 boards also show a small USB drive with their own INI; when it is there and matches,
clibretune uses that one. A definition you add yourself (**Settings > ECU Definitions**) wins over the
bundled one with the same signature.

If the cable is pulled or the ECU resets, clibretune reconnects by itself as soon as the same device
is back (for up to 30 minutes, under any port name) and keeps your edits; the status bar says what it
is waiting for.

On connect clibretune compares the tune in the project with the one in the ECU; when they differ it
asks which one to keep. If in doubt, use the ECU's settings, then **File > Save Tune** so the project
has a copy.

### Engine Setup checklist

A new project opens **Tools > Engine Setup Checklist**: engine (displacement, cylinders), injectors
(flow, dead time), trigger, MAP sensor, TPS, coolant and air temperature sensors, wideband and
ignition. Each **Open** button goes to the page of your ECU's definition that handles it (rusEFI,
FOME, Speeduino and MegaSquirt menus are recognized); pages you open are ticked, and the ticks are
kept with the project (`setup.conf`). **Hide this checklist** puts it away; the Tools menu brings it
back.

### Starting from a base tune

Speeduino users can start from one of the built-in base tunes (Miata NA/NB, BMW M50/M52/M60/M43,
Audi 1.8T, Mitsubishi 4G93, generic): **File > New Project from Base Tune...**, or **File > Load Base
Tune...** into an open Speeduino project. If the tune was made for a different firmware version, a
report lists what did not carry over before anything is applied. A base tune only gets the engine
started: check injector size and dead time, trigger settings and ignition advance against your car
before cranking. rusEFI, FOME, epicEFI and miata-ecus boards ship their base tune in the firmware;
connect and read it.

## 4. Find your way around

- The sidebar is the ECU's own menu tree from its INI. **Ctrl+K** searches it and every setting
  inside its dialogs, by caption or by the INI's constant name: matching entries of the tree come
  first, then a **Settings** list of fields ("Caption — Dialog"), best match first. **Down** moves
  into the results, **Enter** opens the best match or the selected one (a field opens its dialog
  with the field focused and outlined for a moment), **Esc** empties the search and goes back to
  where you were. Fields that another setting hides are listed grayed, "(hidden by another setting)".
  Drag its right edge to make it wider or narrower (double-click the edge for the default width);
  **F9** hides and shows it. Both are remembered.
- Tables: arrow keys or the mouse select, type a number to set, `+`/`-` step, `=` average,
  `/` interpolate, `s` smooth, Ctrl+Z undo. `-` steps down, as in TunerStudio: for a negative
  value press F2 or Enter first, then type it (`-5`). Esc cancels a value you are typing (otherwise it only
  shrinks the selection to one cell); **Revert** in the toolbar goes back to the values the table
  had when it was opened. Right-click for every other operation, including the 3D view. In a
  narrow window the toolbar buttons that do not fit are in the **»** menu at its right end.
  A Target AFR table has a read-only **lambda preview** beside it that scrolls with it; collapse it
  with its **»** button (clibretune remembers) and open it again from the strip.
- **View > Dashboard** (Ctrl+D) shows gauges; the Designer button edits them. In the designer,
  double-click a gauge (or select it and press **Properties**) to set its title, channel, range and
  the warning and danger levels; **Save** keeps them in the dashboard file.
- **Help > Keyboard Shortcuts** lists everything; **Help > User Manual** is the full manual.

## 5. Change and burn

Edits go to the ECU's RAM immediately while connected, so you can try a change with the engine
running. They are lost at power-off until you **burn** them: **File > Burn to ECU** (Ctrl+B), the
flame button, or the **Burn** button next to "*N* unburned changes" in the status bar, which counts
the settings that are only in the ECU's RAM. clibretune checks the tune for pin conflicts before
burning.

Quitting, closing the project (also by opening, creating or importing another one, or starting the
demo) or disconnecting with unburned or unsaved changes asks first: **Burn and quit**, **Save and
quit**, **Quit without saving** or **Cancel** (the same for close and disconnect).

Save often (Ctrl+S). While a project has unsaved changes clibretune also writes them every minute to
`recovery/autosave.msq` in the project folder; if it was not closed normally (crash, power cut), the
next time the project opens it offers to **Restore** that tune. Restore points (**File > Create
Restore Point**) and **File > Tune History...** let you go back to any earlier tune.

**File > Load Tune...** opens a `.msq` file. A tune saved by other firmware (or one with values that
do not fit this ECU definition) first shows what does not map, and is loaded only when you press
**Continue with Tune**; check every page before burning a tune that came from another ECU.

## 6. Log

**Tools > Data Logging** (Ctrl+L) records every channel. **Tools > Datalog Viewer...** opens logs
(`.csv`, `.msl`, `.mlg`) with stacked graph pages, scatter plots and histograms; logs from
TunerStudio open directly. Its list of logs in the project's `datalogs` folder updates when a
recording starts or stops and when you come back to the viewer from another tab.

## 7. AutoTune

With a wideband sensor and a warm engine: **Tools > AutoTune** (Ctrl+Shift+A), choose the VE table,
check the target AFR table and the filter settings, then press Space to start. Drive steadily
through the load and rpm range; cells fill as data arrives. Lock cells you trust with `L`, review the
proposed changes, apply them with **Send to ECU** (**Undo** or Ctrl+Z puts the table back) and burn.
**Tools > WUE Analyze** and **Trim Table Analyze** work the same way for warm-up enrichment and
per-cylinder trims.

## 8. Update the firmware

**Tools > Firmware Update...** flashes STM32 ECUs (rusEFI, FOME, epicEFI, miata-ecus) over USB DFU or
OpenBLT, Speeduino on an Arduino Mega 2560 over its serial port and Speeduino on a Teensy 3.5/3.6/4.1
(Linux and macOS); see the README's "Firmware update" section. Save your tune first: a firmware update can reset settings.
