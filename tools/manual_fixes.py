"""manual_fixes.py - the per-page corrections tools/gen_manual.py applies to the stock manual.

The stock pages (ref/LibreTune/docs/src) describe the Rust/Tauri original: its menus, dialogs,
settings, defaults and file formats.  Every passage below was checked against this program's
code and replaced with what clibretune actually does.  Each entry is (page id, regex, replacement)
and is applied to the RAW stock text (before LibreTune is renamed clibretune), so patterns quote
"LibreTune" where the stock text has it.  Every pattern must match: a changed upstream page fails
the generator instead of silently keeping a false statement.

Helpers: lit(s) matches s literally; span(a, b) matches from a up to (not including) b.
"""
import re

A = "→"   # the arrow of menu paths


def lit(s):
    return re.escape(s)


def span(a, b):
    return re.escape(a) + r".*?(?=" + re.escape(b) + r")"


def to_end(a):
    return re.escape(a) + r".*\Z"


ISSUES = "https://github.com/v3ai/clibretune/issues"

# ---------------------------------------------------------------------------------------------
# Introduction, Getting Started, FAQ
FIXES = [
    ("introduction", lit("![LibreTune Welcome Screen](../screenshots/welcome.png)\n\n"), ""),
    ("introduction", lit("multiple tune versions with Git-based history"),
     "multiple tune versions with restore points and a tune history with branches"),
    ("introduction", lit("- **In-app Help**: Press `F1` or click the `?` button on any dialog"),
     "- **In-app Help**: Press `F1` (or **Help " + A + " User Manual**) to open this manual at the"
     " page for what you are looking at; ECU dialogs with help text have a **Help** button"),
    ("introduction", lit("https://github.com/RallyPat/LibreTune/issues"), ISSUES),
    ("introduction", lit("- **Community**: Join discussions in the GitHub repository"),
     "- **Community**: Join discussions in the clibretune GitHub repository"),
    ("introduction", lit("https://github.com/RallyPat/LibreTune/blob/main/LICENSE"),
     "https://github.com/v3ai/clibretune/blob/main/LICENSE"),

    # first-project
    ("getting-started/first-project",
     span("When you launch LibreTune without an open project", "### Recent Projects"),
     "With no project open clibretune shows its start screen.\n\n"
     "### Main Actions\n\n"
     "| Button | What It Does |\n"
     "|--------|-------------|\n"
     "| **Connect to my ECU** | Finds the ECU on the USB serial ports, picks its definition (INI)"
     " and sets up a project in one go. |\n"
     "| **Try the demo** | Runs a simulated ECU inside the program: everything works without"
     " hardware. |\n"
     "| **Start from a base tune** | Opens the built-in Speeduino base tunes for a new"
     " project. |\n"
     "| **Import a TunerStudio project** | Imports a TunerStudio project folder. |\n"
     "| **Open a project folder...** | Opens an existing clibretune project. |\n"
     "| **Set up a project manually...** | The New Project dialog: name, ECU definition and"
     " port. |\n\n"
     "When an ECU is plugged in, a green strip names it; if this computer has talked to that ECU"
     " before, one click opens its project and connects.\n\n"),
    ("getting-started/first-project",
     span("- **Last modified date**\n", "---\n\n## Opening a Tune File"),
     "- **When it was last opened**\n"
     "- **x** on the right takes the project off the list; the project folder stays on disk\n\n"),
    ("getting-started/first-project",
     lit("1. Click **Open Tune File** on the Welcome View, or go to **File → Open Tune File...** (Ctrl+N)"),
     "1. Go to **File " + A + " Open Tune...** (with no project open the dialog is \"Open Tune as"
     " New Project\") and click **Browse...**"),
    ("getting-started/first-project",
     span("| **Signature** | The ECU firmware signature embedded in the tune |", "4. LibreTune"),
     "| **File** | The tune file |\n"
     "| **Signature** | The ECU firmware signature embedded in the tune |\n"
     "| **Saved** | When the tune was saved (if recorded) |\n"
     "| **Author** | Who created the tune (if recorded) |\n"
     "| **Contents** | Number of constants, pages and bytes |\n\n"),
    ("getting-started/first-project",
     lit("from your local repository:"), "from your INI library:"),
    ("getting-started/first-project",
     span("   - If **no match** is found, you can either:", "5. A **project name**"),
     "   - If **no match** is found, import the INI into the INI library first (**Settings " + A +
     " ECU Definitions " + A + " Import INI File...** or **File " + A + " Search for INI"
     " Online...**)\n\n"),
    ("getting-started/first-project", lit("6. Click **Open** to create the project and start tuning."),
     "6. Click **Create Project** to create the project and start tuning."),
    ("getting-started/first-project", lit("When you click Open:"), "When you click Create Project:"),
    ("getting-started/first-project", lit("(`~/Documents/LibreTuneProjects/`)"),
     "(`~/Documents/clibretune Projects/` by default)"),
    ("getting-started/first-project",
     span("If you want to read the current tune directly from a connected ECU:",
          "For detailed connection instructions"),
     "If you want to start from the tune that is in a connected ECU:\n\n"
     "1. Click **Connect to my ECU** on the start screen\n"
     "2. clibretune scans the USB serial ports, identifies the ECU, picks its definition (bundled,"
     " from the INI library or from the ECU's own USB drive) and asks for a project name\n"
     "3. If this computer has used that ECU before, its project reopens instead\n\n"
     "To choose the port and baud rate yourself use **File " + A + " Connection Settings...**"
     " (115200 for current rusEFI, FOME, epicEFI and Speeduino).\n\n"),
    ("getting-started/first-project",
     lit("1. Click **Import TS Project** on the Welcome View, or go to **File → Import TS Project...**"),
     "1. Click **Import a TunerStudio project** on the start screen, or go to **File " + A +
     " Import TunerStudio Project...**"),
    ("getting-started/first-project", lit("4. Click **Import**\n"),
     "4. Click **Import Project** (or **Import Tune Only** to bring just the tune into the open"
     " project)\n"),
    ("getting-started/first-project",
     span("1. Click **Generate Base Map from Engine Specs** on the Welcome View",
          "For the complete guide, see [Base Map Generator]"),
     "1. With a project open, go to **Tools " + A + " Generate Base Map...** (or tick **Open the"
     " base map wizard after creating** in the New Project dialog)\n"
     "2. Enter your engine specifications:\n"
     "   - Cylinder count, displacement, injector size\n"
     "   - Fuel type, aspiration, injection/ignition mode\n"
     "   - Idle RPM, redline RPM, boost target (if turbocharged)\n"
     "3. Check the preview (VE table, Ignition, AFR target); it updates as you type\n"
     "4. Click **Apply to tune**, then save or burn\n\n"),
    ("getting-started/first-project",
     lit("- **Auto-downloaded** when LibreTune detects a signature mismatch and finds a match online\n"
         "- **Imported manually** via **Settings → ECU Definitions** or during the Open Tune File flow\n"),
     "- **Bundled**: clibretune carries 167 definitions and picks the matching one by the ECU's"
     " signature\n"
     "- **Imported** via **Settings " + A + " ECU Definitions**\n"
     "- **Search for INI Online** (File menu) opens the firmware's INI page in your web browser and"
     " offers to import an INI you download (clibretune downloads nothing itself)\n"),
    ("getting-started/first-project", lit("switch to the **ECU Definitions** tab:"),
     "choose the **ECU Definitions** section:"),
    ("getting-started/first-project",
     lit("- **Import** new INI files by clicking the import button and browsing\n"
         "- **Delete** unused INI files by clicking the ✕ button (click once to select, again to confirm)"),
     "- **Import INI File...** or **Scan Folder...** to add definitions\n"
     "- **Remove** an unused INI file (the button changes to **Confirm Remove**; click again)"),
    ("getting-started/first-project", lit("2. The Welcome View appears"), "2. The start screen appears"),

    # connecting
    ("getting-started/connecting",
     span("## Opening the Connection Dialog", "## Next Steps"),
     "## Connecting the First Time\n\n"
     "Plug in the ECU's USB cable (a data cable: some cables only charge), switch the ignition on"
     " and press **Connect to my ECU** on the start screen (or **File " + A + " Connect ECU"
     " Wizard...**). clibretune looks at USB serial ports first and recognizes common ECU boards"
     " by their USB id: STM32 boards (rusEFI, FOME, epicEFI), Arduino Mega (Speeduino), Teensy,"
     " and the usual USB-serial adapters (CH340, FTDI, CP210x, PL2303). Each port is asked at its"
     " likeliest baud rate first, and the window stays responsive while it searches (**Cancel"
     " scan** stops it).\n\n"
     "When the ECU answers, clibretune reads its signature and picks the definition by itself:\n\n"
     "- **exact match** among the bundled definitions, the INI library or the ECU's own USB"
     " drive: used straight away, with a short message;\n"
     "- **close match** (a nightly build, a newer firmware than this clibretune knows): the"
     " nearest build of the same board is used, with a warning and a **Find Exact INI...**"
     " button;\n"
     "- **nothing fits**: a dialog offers **Switch to Selected INI** (local matches), **Browse for"
     " INI File...**, **Search Online** (opens the INI source in your web browser and offers to"
     " import an INI you download), **Continue Anyway** or **Disconnect**.\n\n"
     "The port, baud rate, signature and project of every ECU that answered are remembered, so"
     " the same board is found first the next time, even under another port name.\n\n"
     "## Connecting a Project\n\n"
     "1. **File " + A + " Connect** (`Ctrl+Shift+C`, or the plug button in the toolbar) connects"
     " with the port and baud rate saved in the project\n"
     "2. A project without a saved port gets the Connect ECU wizard instead\n"
     "3. **File " + A + " Disconnect** (`Ctrl+Shift+D`) disconnects\n\n"
     "On connect clibretune compares the tune in the project with the one in the ECU; when they"
     " differ it asks which one to keep.\n\n"
     "## Connection Settings\n\n"
     "**File " + A + " Connection Settings...** picks everything by hand:\n\n"
     "| Setting | Description | Default |\n"
     "|---------|-------------|---------|\n"
     "| **Port** | Serial port of the ECU (**Refresh ports**, or **Find my ECU** to probe"
     " them) | Auto-detected |\n"
     "| **Baud rate** | 9600 to 921600 | 115200 |\n"
     "| **Show all ports** | Also list built-in serial ports (`/dev/ttyS*`, COM1) | Off |\n"
     "| **TCP/WiFi** | Host:port of a network connection | - |\n"
     "| **Demo** | The built-in simulated ECU | - |\n\n"
     "Port, baud rate and auto-connect are stored in the project and restored when it opens.\n\n"
     "Serial ports are named `COM3`, `COM4` (Windows), `/dev/cu.usbmodem*`,"
     " `/dev/cu.usbserial*` (macOS) and `/dev/ttyACM0`, `/dev/ttyUSB0` (Linux).\n\n"
     "### Runtime Packet Mode\n\n"
     "**File " + A + " Settings... " + A + " Connection " + A + " Runtime packet mode** chooses"
     " how realtime data is read: **Auto**, **Force Burst**, **Force OCH** or **Disabled**. It"
     " applies at the next connect. In Auto:\n\n"
     "- **Speeduino / MegaSquirt (MS2 / MS3)** use burst reads (the `A` command); with the newer"
     " CRC-framed protocol and an INI that defines `ochGetCommand` they read output channels"
     " (OCH).\n"
     "- **rusEFI / FOME / epicEFI** read output channels (OCH) when the INI defines an OCH block"
     " read.\n\n"
     "If your gauges stall or report invalid values in Auto mode, try **Force Burst** first.\n\n"
     "### Reconnecting\n\n"
     "If the cable is pulled or the ECU resets, clibretune reconnects by itself as soon as the"
     " same device is back (for up to 30 minutes, under any port name) and keeps your edits; the"
     " status bar says what it is waiting for.\n\n"
     "**Settings " + A + " Reconnect " + A + " Reconnect automatically after controller"
     " commands** (on by default) reconnects and reads the ECU again after a controller command"
     " that reboots or reconfigures the ECU, so its new settings show up in clibretune. A second"
     " option does the same after a firmware update.\n\n"
     "### Common Baud Rates\n\n"
     "| ECU | Baud Rate |\n"
     "|-----|-----------|\n"
     "| Speeduino | 115200 |\n"
     "| rusEFI | 115200 |\n"
     "| epicEFI | 115200 |\n"
     "| MegaSquirt MS2 | 115200 |\n"
     "| MegaSquirt MS3 | 115200 |\n\n"
     "## Connection Status\n\n"
     "The status bar shows the connection state (Disconnected, Connecting..., Connected or Demo)."
     " Click it for **Connection Metrics**: realtime rate, good and failed frames, packets and"
     " bytes, and the last protocol error.\n\n"
     "## Troubleshooting\n\n"
     "When something is in the way clibretune says what to do:\n\n"
     "- *in use by another program*: close TunerStudio or a serial monitor, only one program can"
     " use a port;\n"
     "- *no permission* (Linux): it shows the command, e.g. `sudo usermod -aG dialout $USER`,"
     " then log out and back in;\n"
     "- *the device went away*: the cable came loose or the ECU restarted;\n"
     "- *no answer*: check power and the baud rate, or the ECU is in its bootloader (flash"
     " firmware first);\n"
     "- *wrong baud rate*: data arrived but made no sense; pick the rate by hand.\n\n"
     "**Tools " + A + " Connection Metrics...** and **Tools " + A + " ECU Console** help to see"
     " what the link is doing; see [Troubleshooting](../reference/troubleshooting.md).\n\n"),
]

# settings: the stock page lists the original's settings; this is the Settings dialog here.
SETTINGS = (
    "Settings are grouped in sections, listed on the left of the dialog. Changes apply at once"
    " and are saved automatically; **Close** closes the dialog.\n\n"
    "### Finding a Setting (Search)\n\n"
    "The **Search** box at the top filters the section list by section name and by keywords"
    " (\"baud\" shows Connection, \"trail\" shows Table Display). \"No matching section\""
    " appears when nothing matches.\n\n"
    "To find an **ECU** setting (one from the ECU definition), use the sidebar search"
    " (`Ctrl+K`) instead.\n\n"
    "---\n\n"
    "## Language\n\n"
    "**Settings " + A + " Language** localizes the clibretune interface. Supported"
    " languages:\n\n"
    "- **English** (default / fallback)\n"
    "- **Português (Brasil)**\n"
    "- **Magyar** (Hungarian)\n\n"
    "Switching the language translates the **application menus** (File, Edit, View, Tools, Help)"
    " and the texts that have a translation; the rest stays in English. The ECU tuning menus"
    " (Fuel, Ignition, etc.) are **not** translated: those labels come from your ECU's INI"
    " definition file and are shown verbatim.\n\n"
    "Translations are built into clibretune. To contribute one, add the English text and its"
    " translation to `tools/locales_extra.tsv` in the source repository and run"
    " `tools/gen_locales.py`.\n\n"
    "---\n\n"
    "## Menu Bar Layout\n\n"
    "### Menu grouping\n\n"
    "The top menu bar is organized into three groups separated by visual dividers:\n\n"
    "```\n"
    "File  Edit  View  Tools   |   <ECU tuning menus>   |   Help\n"
    "```\n\n"
    "- **Application menus** (File, Edit, View, Tools) keep stable positions regardless of which"
    " ECU you connect. Edit and Tools appear once an ECU definition is loaded.\n"
    "- **ECU tuning menus** (Fuel, Ignition, etc.) come from the INI and vary per ECU. When the"
    " window is too narrow for all of them they are grouped under one menu.\n"
    "- **Help** stays at the far right.\n\n"
    "Each menu also has an **access key** (the underlined letter): press `Alt` plus that letter"
    " to open the menu (e.g. `Alt+F` for File, `Alt+T` for Tools).\n\n"
    "### Show ECU menus in menu bar\n\n"
    "**Settings " + A + " Appearance " + A + " Show ECU menus in the menu bar** (default: on;"
    " also **View " + A + " Show ECU menus in menu bar**)\n\n"
    "Uncheck it for a leaner menu bar. The ECU menus remain available in the **sidebar** (the"
    " navigation panel on the left); toggle the sidebar with **View " + A + " Toggle Sidebar**"
    " (`F9`).\n\n"
    "---\n\n"
    "## General\n\n"
    "Units of the values clibretune shows (the ECU keeps its own units; values are converted"
    " for display and typing):\n\n"
    "- **Metric** checkbox: switches temperature and pressure together (also **View " + A +
    " Units**); speed follows it (km/h or mph)\n"
    "- **Temperature**: Celsius, Fahrenheit or Kelvin\n"
    "- **Pressure**: kPa, psi, bar or inHg\n"
    "- **Air-Fuel Ratio**: AFR or Lambda\n\n"
    "The fuel used to convert between AFR and lambda is chosen in the lambda preview beside a"
    " Target AFR table.\n\n"
    "## Appearance\n\n"
    "- **Theme**: dark (default), light, midnight, carbon, synthwave, solarized, nord, dracula"
    " or highcontrast (also **View " + A + " Theme**)\n"
    "- **Heat map**: the color scheme of table cells\n"
    "- **UI scale**: Auto (shows the scale it detected) or 100% to 300%\n"
    "- **Show ECU menus in the menu bar**\n\n"
    "## Connection\n\n"
    "- **Realtime rate (Hz)**: how often live data is read (5-100, default 20)\n"
    "- **Connect automatically when a project opens**\n"
    "- **Graphics backend**: auto, x11, win32, web or tty (applies at the next start)\n"
    "- **Runtime packet mode**: Auto, Force Burst, Force OCH or Disabled (see [Connecting to"
    " Your ECU](./connecting.md))\n"
    "- **Adaptive timing**: shortens waits on fast links\n"
    "- **Connection metrics**: live frame counts, rate, bytes and the last protocol error\n\n"
    "The port and baud rate (9600-921600, default 115200) are set in **File " + A +
    " Connection Settings...** and saved in the project.\n\n"
    "## Keyboard Shortcuts\n\n"
    "Rebind the application shortcuts (File, Edit, tabs, AutoTune and others). A key that is"
    " already taken is reported; **Reset** restores one shortcut and **Reset all** all of"
    " them.\n\n"
    "## Dashboard\n\n"
    "- **Gauge refresh rate**: 10, 15, 20, 25, 30 or 60 Hz (default); lower rates use less CPU\n"
    "- **Right-align numeric values**\n"
    "- **Indicators**: columns (0 = automatic), what to do with text that does not fit, fill"
    " the empty cells of the last row\n"
    "- **Designer**: Snap gauges to the grid, Free move, Lock gauge positions, Adopt gauge"
    " ranges from the ECU definition\n\n"
    "## ECU Definitions\n\n"
    "The INI library: the ECU definition files available for projects, besides the 167"
    " bundled ones.\n\n"
    "### Viewing Imported Definitions\n\n"
    "Each entry shows the ECU name and its signature.\n\n"
    "### Importing a New INI File\n\n"
    "1. Click **Import INI File...** (or **Scan Folder...** to add every INI in a folder)\n"
    "2. Pick the `.ini` file\n"
    "3. It is copied into the library and listed\n\n"
    "**File " + A + " Search for INI Online...** opens the firmware's INI page in your web"
    " browser and offers to import an INI that appears in your Downloads folder (clibretune"
    " downloads nothing itself).\n\n"
    "### Removing an INI File\n\n"
    "1. Click **Remove** next to the entry\n"
    "2. It changes to **Confirm Remove**; click again to remove it\n\n"
    "### Definition Storage Location\n\n"
    "- **Windows**: `%LOCALAPPDATA%\\clibretune\\definitions\\`\n"
    "- **macOS**: `~/Library/Application Support/clibretune/definitions/`\n"
    "- **Linux**: `~/.local/share/clibretune/definitions/`\n\n"
    "The environment variable `CLIBRETUNE_DEFINITIONS` moves the library.\n\n"
    "## Version Control\n\n"
    "Settings of the tune history (snapshots of the project with branches; **File " + A +
    " Tune History...**):\n\n"
    "- **Auto-commit on save**: Never, Always or Ask each time\n"
    "- **Commit message format**: placeholders `{date}`, `{time}` and `{table}`; the default"
    " is `Tune saved on {date} at {time}`\n"
    "- **Restore points to keep** (0 = all)\n\n"
    "Branches are created and switched in **File " + A + " Tune History...**; the first one is"
    " `main`.\n\n"
    "## Reconnect\n\n"
    "- **Reconnect automatically after controller commands** (on by default)\n"
    "- **Reconnect automatically after a firmware update** (on by default)\n\n"
    "A lost link is always retried: a re-plugged ECU is reconnected for up to 30 minutes.\n\n"
    "## Data Logging\n\n"
    "- **Smart recording**: Enable auto-record (start on key-on, stop on key-off), Key-on"
    " threshold (RPM, 50-500), Key-off timeout (s, 1-10)\n"
    "- **Recording**: Sample rate (Hz, 1-200, default 10) and File format (MLG, the default,"
    " or MSL). The recorded channels are chosen with **Channels...** in the Data Logging tab.\n"
    "- **Alert rules**: warn on large table changes (absolute and percent thresholds), and"
    " channel limits (a channel above or below a value gives a message and a log marker)\n\n"
    "Logs are written to the project's `datalogs` folder, named by date and time.\n\n"
    "## Table Display\n\n"
    "- **Y axis starts at the bottom (new table tabs)**\n"
    "- **Show the live-cell trail in table editors**, and the **Trail fade** time (0 = never)\n"
    "- **Custom cursor color** and **Custom trail color**\n"
    "- **Heat maps**: the scheme of each heat map (value, change, coverage), or custom color"
    " stops\n\n"
    "## Behavior\n\n"
    "- **Burn pending changes to the ECU when closing**\n"
    "- **Large change alert**: warn when an edit changes a cell by more than an absolute or"
    " percent threshold\n"
    "- **Show help icons next to every field**\n"
    "- **Restore open tabs when a project opens**\n"
    "- **Demo mode (simulated ECU)**\n"
    "- **Onboarding completed** (untick to see the first-start hints again)\n\n"
    "## Status Bar\n\n"
    "Up to 8 live channels shown in the status bar: reorder them with **Up**, take one off with"
    " **Remove**, add from the filtered list, or **Use defaults**.\n\n"
    "## Project\n\n"
    "Shown with a project open: its name, folder and ECU signature, and **Load the tune when the"
    " project opens**, **Save the tune when the project closes**, **Connect to the ECU when the"
    " project opens** and **Restore points to keep**.\n\n"
    "## Project INI Settings\n\n"
    "The ECU definition's own project options (its `settingGroups`, for example the MCU type or"
    " units) for the open project.\n\n"
    "## Assistant\n\n"
    "The AI assistant works with a model you run yourself, through an OpenAI-compatible"
    " endpoint over plain `http://` (for example Ollama or LM Studio); `https://` is not"
    " supported. It only proposes changes, which you confirm; a restore point is created first"
    " and nothing is burned.\n\n"
    "1. Tick **I understand the risks** and **Enable AI Assistant**\n"
    "2. Set **Base URL** (**Ollama preset** and **LM Studio preset** fill it in), **Model** (one"
    " with tool calling) and, if your endpoint needs one, **API key**. The key is stored in"
    " plaintext in `assistant.conf` in the settings folder; set the environment variable"
    " `LT_AI_KEY` instead to keep it out of the file.\n"
    "3. Choose the **Capability tier**: *Read* (inspect and diagnose only), *Tune* (read +"
    " propose table edits) or *Config* (tune + propose constant changes)\n\n"
    "The assistant opens from **Tools " + A + " AI Assistant**.\n\n"
    "**MCP server (local, read-only)**: **Expose read-only tools over MCP on 127.0.0.1** starts"
    " it, **Port** defaults to 8765, and the **Access token** has **Show** and"
    " **Regenerate**.\n\n"
    "## Updates\n\n"
    "There is no automatic update check; **Help " + A + " Check for Updates** shows your"
    " version and opens the releases page.\n\n"
    "---\n\n"
    "## Keyboard Shortcuts\n\n"
    "Common settings-related shortcuts:\n"
    "- `Ctrl+,` - Open Settings\n"
    "- `Ctrl+Z` - Undo (in table editors)\n"
    "- `Ctrl+Y` - Redo\n"
    "- `Tab` - Next field\n"
    "- `Shift+Tab` - Previous field\n\n"
    "---\n\n"
    "## Tips for Configuration\n\n"
    "### First Time Setup\n\n"
    "1. Connect once with **Connect to my ECU** (port and baud rate are remembered)\n"
    "2. Choose the theme and UI scale\n"
    "3. Set unit preferences (°C/°F, psi/kPa, AFR/lambda)\n"
    "4. Turn on version control for the project (**File " + A + " Tune History...**)\n\n"
    "### Backup Settings\n\n"
    "Settings are saved automatically in:\n"
    "- Windows: `%APPDATA%\\clibretune\\`\n"
    "- macOS: `~/Library/Application Support/clibretune/`\n"
    "- Linux: `~/.config/clibretune/`\n\n"
    "(`settings.conf`, `devices.conf`, `hotkeys.conf` and others). The environment variable"
    " `CLIBRETUNE_CONFIG_DIR` moves it. Manual backup can be made by copying this folder.\n\n"
    "---\n\n")

FIXES += [
    ("getting-started/settings", span("The Settings dialog includes multiple tabs", "## See Also"),
     SETTINGS),
    ("getting-started/settings", lit("- [AutoTune Setup](./autotune/setup.md)"),
     "- [AutoTune Setup](../features/autotune/setup.md)"),

    # faq
    ("faq", lit("Use **File → Import TS Project** to import"),
     "Use **File " + A + " Import TunerStudio Project...** (or **Import a TunerStudio project** on"
     " the start screen) to import"),
    ("faq", lit(" See\n[AI Assistant](./features/ai-assistant.md) for the full safety model."), ""),
    ("faq", span("### Do I need an OpenAI account to use the AI Assistant?", "## Connection Issues"),
     "### Do I need an online account to use the AI Assistant?\n\n"
     "No. The assistant talks to a model you run yourself through an OpenAI-compatible endpoint"
     " over plain `http://` (for example Ollama or LM Studio), so your data stays on your"
     " machine. Set it up in **Settings " + A + " Assistant**.\n\n"),
    ("faq", lit("4. Click **Refresh** in the connection dialog"),
     "4. Click **Refresh ports** (or **Find my ECU**) in **File " + A + " Connection"
     " Settings...**; tick **Show all ports** for built-in serial ports"),
    ("faq", lit("2. Or use LibreTune's online search to find a matching INI"),
     "2. Or use **File " + A + " Search for INI Online...** to find a matching INI"),
    ("faq", lit("- TPS above minimum\n- No rapid throttle changes"),
     "- No rapid throttle changes (**Max TPS rate**)\n- Wideband reading inside its range"),
    ("faq", lit("Logs are saved in your project folder under `logs/`."),
     "Logs are saved in your project folder under `datalogs/`, named by date and time."),
    ("faq", lit("LibreTune uses CSV format compatible with MegaLogViewer and other analysis tools."),
     "LibreTune records MegaLogViewer `.mlg` (the default) or `.msl` files (**Settings " + A +
     " Data Logging " + A + " File format**), which also open in MegaLogViewer and TunerStudio."
     " The viewer also opens `.csv` logs."),
    ("faq", lit("1. Go to **Tools → Data Log Viewer**\n2. Click **Open Log**\n3. Select a CSV file"),
     "1. Go to **Tools " + A + " Datalog Viewer...**\n2. Pick a log from the project's list, or"
     " click **Open...**\n3. Select a `.mlg`, `.msl` or `.csv` file"),
    ("faq", lit("By default: `~/Documents/LibreTuneProjects/`\n\nYou can change this in Settings."),
     "By default: `~/Documents/clibretune Projects/` (or `~/clibretune Projects/` without a"
     " Documents folder).\n\n**File " + A + " New Project...** lets you type another folder; the"
     " `CLIBRETUNE_PROJECTS` environment variable changes the default."),
    ("faq", lit("2. Enable **Git version control** for full history"),
     "2. Turn on version control in **File " + A + " Tune History...** (versions with"
     " branches)"),
    ("faq", span("Try these settings:\n1. Disable 3D table visualization", "### Gauges aren't updating"),
     "Try these settings:\n"
     "1. Lower **Settings " + A + " Dashboard " + A + " Gauge refresh rate**\n"
     "2. Lower **Settings " + A + " Connection " + A + " Realtime rate (Hz)**\n"
     "3. Close unused tabs\n\n"),
    ("faq", lit("2. Reduce polling rate in Settings"),
     "2. Check **Tools " + A + " Connection Metrics...** for failed frames"),
    ("faq", lit("\n\nSee [NISSAN_QG18_TRIGGER_SETUP.md](../../NISSAN_QG18_TRIGGER_SETUP.md) for a detailed example."),
     ""),
]

# ---------------------------------------------------------------------------------------------
# Project Management, Supported ECUs, INI format
FIXES += [
    ("projects/tunes", lit("- Creates restore point (if enabled)"),
     "- Records a Tune History version if auto-commit is on (Settings " + A + " Version"
     " Control)"),
    ("projects/tunes", span("### Auto-Save\n", "## Loading Tunes"),
     "### Auto-Save\n"
     "While the tune has unsaved changes, clibretune saves a recovery copy every 60 seconds to"
     " `recovery/autosave.msq` in the project folder. If clibretune was not closed normally, it"
     " offers that copy the next time the project opens. There are no settings for it.\n\n"),
    ("projects/tunes",
     lit("LibreTune warns before burning:\n- Shows changed values count\n- Asks for confirmation\n"
         "- Cannot be undone (use restore points!)"),
     "LibreTune asks for confirmation before burning; the status bar shows how many changed"
     " settings are not burned yet. A restore point is created automatically just before every"
     " burn."),
    ("projects/tunes", lit("1. **Tools → Compare Tunes**"), "1. **Tools " + A + " Compare Tune Files...**"),
    ("projects/tunes", span("### Difference View\n", "## Resetting to Defaults"),
     "### Difference View\n"
     "Rows are colored by the size of the change: red at 20% or more, yellow at 5-20%. You can"
     " filter and sort the list, tick constants and merge them from tune B into the open tune.\n\n"),
    ("projects/tunes", lit("1. **Tools → Reset to Defaults**"),
     "1. **Edit " + A + " Reset Tune to Defaults**"),

    ("projects/version-control", lit("Track tune history with Git-based versioning."),
     "Track tune history with versioned snapshots."),
    ("projects/version-control", lit("LibreTune can use Git to track changes to your tune:"),
     "LibreTune can keep a version history of your project:"),
    ("projects/version-control", span("### New Projects\n", "## How It Works"),
     "Version control is turned on per project:\n\n"
     "1. Open **File " + A + " Tune History...**\n"
     "2. Click **Initialize Version Control**\n"
     "3. The first version is recorded\n\n"),
    ("projects/version-control", lit("1. Creates a Git repository in your project folder"),
     "1. Keeps a copy of each version (the tune, `project.json`, `projectCfg/` and"
     " `dashboards/`) in a `snapshots` folder inside the project"),
    ("projects/version-control", lit("| **Ask** | Prompt each time |"),
     "| **Ask each time** | Prompt after each save |"),
    ("projects/version-control",
     lit("1. **File → Tune History** or click history icon\n2. Timeline shows all commits\n"
         "3. Click a commit to see details"),
     "1. **File " + A + " Tune History...**\n2. The list shows every version of the current"
     " branch, newest first\n3. Each entry shows its ID, date and time, message and note;"
     " **View Changes** lists the files and constants it changed"),
    ("projects/version-control", lit("3. Click **Checkout**"), "3. Click **Restore**"),
    ("projects/version-control", lit("2. Click **View Diff**"), "2. Click **View Changes**"),
    ("projects/version-control", lit("1. Open branch selector dropdown"),
     "1. Open the branch selector at the top of Tune History"),
    ("projects/version-control", lit("2. Click **Commit**\n3. Enter descriptive message\n4. Commit created"),
     "2. Click **Create Checkpoint**\n3. Enter a descriptive message (and an optional"
     " annotation)\n4. The version is recorded"),
    ("projects/version-control", lit("- Merge when proven"),
     "- To carry proven values over, use **Tools " + A + " Compare Tune Files...** and merge the"
     " selected constants"),

    ("projects/change-annotations", lit("1. Open **Tools → Tune History**.\n2. Click **Commit**."),
     "1. Open **File " + A + " Tune History...**.\n2. Click **Create Checkpoint** (or answer the"
     " commit prompt after a save when auto-commit is set to Ask each time)."),
    ("projects/change-annotations", lit("## Recommended Use"),
     "For notes on constants, tables and single table cells, use **Tools " + A +
     " Annotations...**: they are stored in `projectCfg/annotations.json`, and annotated tables"
     " get a bullet in their tab title.\n\n## Recommended Use"),

    ("projects/restore-points", span("### Automatic Creation\n", "## Viewing Restore Points"),
     "### Automatic Creation\n"
     "Restore points are auto-created:\n"
     "- Before every burn to the ECU\n"
     "- Before the project tune is replaced by the ECU's settings (or kept with Ignore) when they"
     " differ on connect\n"
     "- Before changes proposed by the AI assistant are applied\n\n"),
    ("projects/restore-points", lit("3. Click **Load**"), "3. Click **Restore**"),
    ("projects/restore-points", span("## Deleting Restore Points\n", "## Automatic Cleanup"),
     "## Deleting Restore Points\n\n"
     "1. Tick the restore points to delete (**Select All** / **Select None** help)\n"
     "2. Click **Delete Selected**\n"
     "3. Confirm deletion\n\n"
     "To thin out old points, set **Keep newest** in the dialog; older points are deleted right"
     " away.\n\n"),
    ("projects/restore-points", lit("Configure max restore points in Settings:"),
     "Configure the number to keep in **Settings " + A + " Project** (Restore points to"
     " keep):"),
    ("projects/restore-points", lit("| Setup | None needed | Initialize repo |"),
     "| Setup | None needed | Initialize once per project |"),
    ("projects/restore-points", lit("| Comparison | No | Yes (diff) |"),
     "| Comparison | Yes (against the open tune) | Yes (between versions) |"),
    ("projects/restore-points", lit("no Git knowledge required"), "nothing to set up"),
    ("projects/restore-points", span("[Project Folder]/restore-points/", "```\n\nYou can"),
     "[Project Folder]/restorePoints/\n"
     "+-- MyProject_2026-01-11_14.30.00.msq\n"
     "+-- MyProject_2026-01-11_12.15.00.msq\n"
     "+-- ...\n"),

    ("projects/importing", lit("1. **File → Import TS Project...**"),
     "1. **File " + A + " Import TunerStudio Project...**"),
    ("projects/importing", lit("4. Click **Import**\n5. Project created in LibreTune"),
     "4. Click **Import Project** (or **Import Tune Only** to bring just the tune into the open"
     " project)\n5. Project created in LibreTune"),
    ("projects/importing", lit("2. Click **Import ECU Definition**"),
     "2. Click **Import INI File...** (or **Scan Folder...** to add every INI in a folder)"),
    ("projects/importing",
     span("You can also import INI files inline during the [Open Tune File]", "### Managing Your INI Library"),
     "If **File " + A + " Open Tune...** finds no matching INI, import one into the INI library"
     " first (here, or with **File " + A + " Search for INI Online...**).\n\n"),
    ("projects/importing",
     lit("1. **File → Import Dashboard...**\n2. Select `.dash` file\n3. Dashboard converted to LibreTune format\n"
         "4. Available in dashboard selector"),
     "1. On the dashboard tab, click the dashboard name button in the toolbar\n"
     "2. Click **Import...** and pick a `.dash` file, or **From TunerStudio...** to pick from the"
     " dashboards of a TunerStudio project\n"
     "3. It is added to the project's `dashboards` folder\n\n"
     "The TunerStudio project import also copies the project's dashboards."),
    ("projects/importing", lit("Add an INI to your local repository:"), "Add an INI to your INI library:"),
    ("projects/importing", lit("Settings** (Ctrl+Comma) → **ECU Definitions** tab"),
     "Settings** (Ctrl+Comma) " + A + " **ECU Definitions**"),

    ("reference/supported-ecus",
     lit("LibreTune includes common INI files:\n- Speeduino (multiple versions)\n- rusEFI (multiple boards)\n- epicEFI"),
     "LibreTune includes 167 INI files: every Speeduino release since 201712, all boards of the"
     " latest rusEFI release (plus two earlier ones), all boards of the latest FOME release, and"
     " miata-ecus. MegaSquirt and epicEFI INIs are not included: use the one that came with the"
     " firmware."),
    ("reference/supported-ecus", span("### Online Search\nSearch GitHub repositories:", "### Manual Import"),
     "### Online Search\n"
     "**File " + A + " Search for INI Online...** opens the INI download page for your ECU"
     " (Speeduino, rusEFI, FOME or epicEFI) in your web browser and watches your Downloads"
     " folder; a downloaded INI is offered for import into the library.\n\n"),
    ("reference/supported-ecus", lit("1. **Settings → ECU Definitions → Import**"),
     "1. **Settings " + A + " ECU Definitions " + A + " Import INI File...**"),
    ("reference/supported-ecus", lit("3. Added to local repository"), "3. Added to the INI library"),
    ("reference/supported-ecus",
     lit("\n**Example**: For detailed information on requesting trigger pattern support, see "
         "[NISSAN_QG18_TRIGGER_SETUP.md](../../NISSAN_QG18_TRIGGER_SETUP.md).\n"), ""),

    ("reference/ini-format", lit("Format: channel, title, units, min, max, lowWarn, lowDanger, highWarn, highDanger"),
     "Format: channel, title, units, min, max, lowDanger, lowWarn, highWarn, highDanger, value"
     " digits"),
    ("reference/ini-format", lit("own min/max, committing the written value when the handle is released. The"),
     "own min/max, writing the value as the handle moves (arrow keys, Home and End also work)."
     " The"),
]

# ---------------------------------------------------------------------------------------------
# Base map, performance calculator, loggers, tools, console, migration
PERF = (
    "# Performance Calculator\n\n"
    "**Tools " + A + " Performance Calculator...** estimates power and fuel economy from"
    " vehicle data and live (or typed) engine data. The results are physics-based estimates,"
    " not dyno numbers.\n\n"
    "## Vehicle Specs\n\n"
    "Enter weight (lb or kg), frontal area, drag coefficient (Cd), tire diameter, drivetrain"
    " loss, final drive, gear ratios (comma separated), redline, injector size and the number of"
    " injectors. **Save specs** stores them in the project (`vehicle_specs.properties`).\n\n"
    "## Results\n\n"
    "With **Use realtime data** ticked, the calculator uses live RPM, vehicle speed,"
    " acceleration and injector pulse width; otherwise you type them in. It shows:\n\n"
    "- engine horsepower, torque (lb-ft and Nm) and kW\n"
    "- 0-60 mph, 1/4-mile time and trap speed\n"
    "- aerodynamic drag and rolling resistance\n"
    "- a gearing table: the speed in each gear at the current RPM and at redline\n"
    "- fuel economy: fuel flow, mpg or l/100 km, per-hour figures and a trip total (**Reset"
    " trip**), in US gallons, imperial gallons or liters\n\n"
    "## Tips\n\n"
    "- Use a realistic drag coefficient and frontal area for your car\n"
    "- Weigh the car with driver and fuel for the acceleration figures\n"
    "- Compare runs made under the same conditions\n")

FIXES += [
    ("features/base-map",
     lit("1. **Welcome View**: Click the **\"Generate Base Map from Engine Specs\"** link below the main action buttons\n"
         "2. **Tools menu**: Go to **Tools → Generate Base Map...** (available any time, with or without a project open)"),
     "1. **Tools " + A + " Generate Base Map...** (the Tools menu is there once an ECU definition"
     " is loaded, e.g. with a project open)\n"
     "2. In the **New Project** dialog, tick **Open the base map wizard after creating**"),
    ("features/base-map", lit("Number of engine cylinders. Select from: 1, 2, 3, 4, 5, 6, 8, 10, or 12."),
     "Number of engine cylinders, 1 to 12."),
    ("features/base-map", lit("Valid range: 50 – 15,000 cc."), "Valid range: 50 – 20,000 cc."),
    ("features/base-map", lit("Valid range: 50 – 5,000 cc/min."), "Valid range: 50 – 3,000 cc/min."),
    ("features/base-map", lit("Valid range: 400 – 2,000 RPM."), "Valid range: 300 – 3,000 RPM."),
    ("features/base-map", lit("Valid range: 2,000 – 20,000 RPM."), "Valid range: 2,000 – 12,000 RPM."),
    ("features/base-map", lit("When Turbocharged or Supercharged is selected, an additional **Boost Target** field appears."),
     "The **Boost target (kPa)** field can be edited when Turbocharged or Supercharged is"
     " selected."),
    ("features/base-map", lit("The dialog shows a real-time conversion to bar and psi gauge pressure as you type.\n\n"), ""),
    ("features/base-map", lit("Valid range: 120 – 400 kPa."),
     "Valid range: 100 – 400 kPa (the generator uses at least 120)."),
    ("features/base-map",
     lit("Override the default safe WOT (wide-open throttle) AFR. Leave blank to use the fuel type's safe default."),
     "The **WOT AFR** field starts at the fuel type's safe default; change it to override. The"
     " optional **Chamber**, **Compression ratio** and **Octane** fields refine the ignition"
     " table."),
    ("features/base-map", span("After filling in your engine specs, click **Generate Base Map**.", "### VE Table Tab"),
     "The preview beside the form updates as you change any field, and the **Calculated"
     " reqFuel** is shown under the form. Choose **VE table**, **Ignition** or **AFR target** to"
     " see that 16x16 table as a heat-map grid.\n\n"),
    ("features/base-map", lit("### Enrichments Tab\n\nShows all the non-table settings the generator produces:"),
     "### Enrichments\n\nThe generator also computes these values, but the wizard does not show"
     " them or write them into the tune:"),
    ("features/base-map", span("## Applying the Base Map\n", "## What Gets Generated"),
     "## Applying the Base Map\n\n"
     "**Apply to tune** writes the generated VE, ignition and AFR target tables into the open"
     " tune, resampled to each table's own size, and the constants reqFuel, nCylinders, injType,"
     " sparkMode and stoich when the ECU definition has them. A message says how many values"
     " were written. **Overwrite the table axes with the generated bins** (on by default) also"
     " replaces the tables' RPM and load axes. **Close** closes the wizard.\n\n"
     "Apply needs an open tune; to start a new project from a base map, tick **Open the base map"
     " wizard after creating** in the New Project dialog.\n\n"
     "---\n\n"),
    ("features/base-map",
     span("| **Cranking enrichment** | Temperature-indexed fuel enrichment curve |", "\n---"),
     "\nThe wizard writes the bins, the three tables, reqFuel and the constants above. The"
     " cranking and warm-up curves, acceleration enrichment, idle air control and prime pulse are"
     " computed (see Enrichments) but not written: set them in the ECU's own dialogs.\n"),

    ("features/performance-calculator", to_end("# Performance Calculator"), PERF),

    ("features/diagnostic-loggers", lit("   Tuning → Tooth Logger"),
     "   Tools " + A + " Tooth Logger (listed when the ECU definition declares loggers; it opens"
     " a tab)"),
    ("features/diagnostic-loggers",
     lit("   - Select trigger type (360°, 720°, or custom)\n"
         "   - Set capture duration (1-10 seconds)\n"
         "   - Choose teeth to log (all or specific range)"),
     "   - Set the capture length (**for** ... s, 1-600, default 20)\n"
     "   - Set the wheel's **teeth/rev** and **missing** teeth"),
    ("features/diagnostic-loggers", lit("   - Click **Start**"),
     "   - Click **Capture** (it becomes **Stop**); the capture ends at the time limit or on Stop"
     " and is saved as CSV automatically"),
    ("features/diagnostic-loggers", lit("   Tuning → Composite Logger"), "   Tools " + A + " Composite Logger"),
    ("features/diagnostic-loggers",
     lit("While primarily accessed from **View → Data Logger**, you can also access diagnostic logs here."),
     "Data logs are recorded with **Tools " + A + " Start Data Logging** and opened with **Tools"
     " " + A + " Datalog Viewer...**."),
    ("features/diagnostic-loggers", span("## Exporting Logger Data", "## Advanced Logging Tips"),
     "## Exporting Logger Data\n\n"
     "Each capture is saved as CSV when it ends; **Export CSV** saves the current capture again."
     " Files go to the project's `datalogs` folder as `tooth_<date time>.csv` or"
     " `composite_<date time>.csv`. **Clear** discards the capture.\n\n"),

    ("features/tools", lit("- Export comparison reports\n"), ""),
    ("features/tools", span("### Workflow", "### Use Cases"),
     "### Workflow\n\n"
     "1. **Tools " + A + " Compare Tables...**\n"
     "2. Pick **Table A** (from the open tune), **Compare against** (Open tune, Tune file or"
     " Restore point) and **Table B**\n"
     "3. Click **Compare Tables** (**Swap** exchanges A and B)\n"
     "4. Choose the view (**Difference**, **Percent** or **Values (B)**) and toggle **Heat"
     " map**\n\n"
     "The summary shows Cells Changed, Total Cells, Max Difference and Avg Difference, with the"
     " changed cells grouped as High (10% or more), Medium (5-10%) and Low (under 5%)"
     " variance. Red means higher in B, blue lower in B, uncolored unchanged.\n\n"),
    ("features/tools", lit("- 🟦 Blue = Richer (added fuel)"), "- Red = higher in B"),
    ("features/tools", lit("- 🟥 Red = Leaner (removed fuel)"), "- Blue = lower in B"),
    ("features/tools", span("### Opening Table Comparison", "### Workflow"),
     "### Views\n\n"
     "- **Difference**: B minus A in each cell\n"
     "- **Percent**: the change from A in percent\n"
     "- **Values (B)**: the values of table B\n"
     "- **Heat map**: red = higher in B, blue = lower in B, uncolored = unchanged\n\n"),
    ("features/tools", span("### Example Output", "---"), ""),
    ("features/tools", span("## Action Manager", "## AFR Delay Test"),
     "## Action Recorder\n\n"
     "**Tools " + A + " Action Recorder...** records table and constant edits so they can be"
     " played back later, for example to apply the same changes to another tune.\n\n"
     "1. Type a name (plus optional description, tags and compatible ECUs) and click **Start"
     " Recording**\n"
     "2. Edit tables and constants in the editors; edits are recorded until **Stop Recording**,"
     " even with the dialog closed\n\n"
     "Sets are kept in the project's `actions` folder as `.ltaction` files. For each set you can"
     " edit the details and steps (**Remove step**, **+ Pause 500 ms**) and **Save changes**."
     " **Validate** checks the set against the ECU definition. **Play** (speed 0.5x-5x) applies"
     " it to the open tune, or to the ECU when connected. **Import .ltaction...** and"
     " **Export...** share sets, **Delete set** removes one, and **Tools " + A + " Action"
     " Triggers...** plays sets automatically when a condition is met.\n\n"),
    ("features/tools", lit("| Settle | 500–10000 ms | Recovery window sampled after the restore |"),
     "| Settle | 1500–15000 ms | Recovery window sampled after the restore |"),
    ("features/tools", lit("| Run until stopped | — | Continuous mode; keep driving and let the map fill |\n"), ""),
    ("features/tools", lit("The overlay draws every step's trace"), "The overlay draws the last six steps' traces"),
    ("features/tools", span("Each successful step lands in a bin of the rpm × load table", "\n\n"),
     "Each successful step lands in a bin of a 6 (rpm) x 5 (load) table showing the mean delay"
     " and sample count. Run tests at different operating points to fill it. **Save model**"
     " (after 3 measurements) writes `afr_delay_model.json`."),
    ("features/tools", span("## Reset to Defaults", "## See Also"),
     "## Reset to Defaults\n\n"
     "**Edit " + A + " Reset Tune to Defaults** replaces every value in the tune with the ECU"
     " definition's default after a confirmation; when connected, the values are sent to the"
     " ECU. Undo does not bring the old values back, so save or create a restore point"
     " first.\n\n"),

    ("features/ecu-console", span("The menu item is only enabled when:", "\n\n"),
     "**Tools " + A + " ECU Console** is listed when the ECU definition is a rusEFI, FOME or"
     " epicEFI one. It also opens offline, showing DISCONNECTED."),
    ("features/ecu-console", span("### Output Colors", "### Command History"),
     "### Output Colors\n\n"
     "Commands you send start with `>` in the accent color, ECU responses are plain text, errors"
     " start with `x` in red, and notes are gray. The quick buttons (help, status, hello,"
     " triggerinfo, ...) send common commands.\n\n"),
    ("features/ecu-console", lit("Click the **Clear** button in the console header to clear the output history."),
     "Click **Clear History**. The command history is kept between sessions; Up/Down (or"
     " Ctrl+Up/Down) recall earlier commands."),
    ("features/ecu-console", span("## FOME Fast Comms", "## Connection States"), ""),
    ("features/ecu-console", lit('Input is disabled; a "Not connected" message is shown'),
     "Send and the quick buttons are disabled; the header says DISCONNECTED"),
    ("features/ecu-console", lit("| **Loading** | A blinking cursor animation indicates the ECU is processing your command |\n"), ""),

    ("features/tune-migration", lit("- A **structural hash** of all constant definitions (names, types, offsets, scales)\n"), ""),
    ("features/tune-migration", lit("- The structural hash uses SHA-256 over all non-PC constant definitions\n"), ""),
    ("features/tune-migration",
     lit("- **Removed Constants** — Settings that existed in the old INI but are no longer present. These values will be lost."),
     "- **Removed Constants** — Settings that existed in the old INI but are no longer present"
     " (listed as \"Removed from INI - Values Preserved\": the values stay in the tune file, the"
     " new definition does not use them)."),
    ("features/tune-migration",
     lit("Each section shows up to 20 items, with a \"...and N more\" indicator for larger lists."),
     "The Removed and New lists show up to 20 names with \"...and N more\"; the other sections"
     " list every change. A **Storage Location Changed** section lists constants that moved"
     " (handled automatically)."),
    ("features/tune-migration",
     lit("2. You **switch the INI file** for an open project (via Settings or the Signature Mismatch dialog)"),
     "2. You change the project's ECU definition with **File " + A + " Update Project INI...**"
     " (the report is shown before switching)\n\n**Tools " + A + " Tune Migration Report...**"
     " shows the report for the open tune at any time."),
    ("features/tune-migration", lit("**Table Comparison** (Tools → Table Comparison)"),
     "**Table Comparison** (Tools " + A + " Compare Tables...)"),
]

# ---------------------------------------------------------------------------------------------
# Dashboards, math channels, data logging, smart recording, statistics, alert rules
DASH_PICK = "the dashboard name button at the left of the dashboard toolbar"
FIXES += [
    ("features/dashboards", span("LibreTune includes three professionally designed dashboards:", "## Opening a Dashboard"),
     "LibreTune has four built-in dashboards:\n\n"
     "- **ECU front page**: the gauges of the ECU definition's own front page\n"
     "- **Basic**: RPM tachometer, AFR dial, coolant, MAP and throttle bars, and battery, IAT,"
     " timing, VE and pulse-width readouts\n"
     "- **Tuning**: RPM and AFR dials, MAP/TPS and coolant/IAT bars, a lambda trend graph, and"
     " VE, pulse width, duty, timing, battery, EGT, AFR target and correction readouts\n"
     "- **Telemetry**: a dense panel of readouts and trend graphs\n\n"),
    ("features/dashboards", lit("1. Click the dashboard selector (dropdown in header)"),
     "1. Click " + DASH_PICK),
    ("features/dashboards", lit("- Gauges update automatically (10Hz default)"),
     "- Gauges update automatically (realtime rate 20 Hz by default, Settings " + A +
     " Connection)"),
    ("features/dashboards", lit("LibreTune supports all 13 standard gauge styles:"),
     "LibreTune supports 20 gauge styles, including:"),
    ("features/dashboards", lit("1. Right-click the dashboard background\n2. Select **Designer Mode**"),
     "1. Click **Designer** in the dashboard toolbar (or right-click the dashboard background"
     " and select **Designer Mode**)\n2. Click **Add** for the gauge palette"),
    ("features/dashboards", lit("4. Resize by dragging edges"),
     "4. Resize by dragging the bottom-right corner handle"),
    ("features/dashboards", lit("6. Click **Exit Designer** when done"),
     "6. Click **Designer** again (or press Esc) when done, and **Save** to keep the layout"),
    ("features/dashboards", lit("| **LibreTune Gauges** | Add gauges from INI definition |\n"), ""),
    ("features/dashboards", lit("| **Background** | Change background color/image |"),
     "| **Full screen** | Show only the dashboard (F11) |"),
    ("features/dashboards", span("LibreTune can import TunerStudio dashboard layouts:", "## Creating Custom Dashboards"),
     "1. Click " + DASH_PICK + ", then **Import...** (a `.dash` file) or **From TunerStudio...**"
     " (the dashboards of a TunerStudio project)\n"
     "2. LibreTune shows how many of its channels this ECU definition has, then copies it into"
     " the project's `dashboards` folder\n\n"
     "**Open...** in the dashboard toolbar opens any `.dash` file directly.\n\n"),
    ("features/dashboards", lit("3. Right-click → **Add Gauge**\n4. Select the data channel"),
     "3. Right-click " + A + " **Add gauge here** and pick a gauge type (or use **Add** for the"
     " gauge palette)\n4. Set its channel in **Properties**"),

    ("features/dashboards/using", lit("- Default rate: 10 updates per second"),
     "- Default realtime rate: 20 Hz (Settings " + A + " Connection " + A + " Realtime rate)"),
    ("features/dashboards/using", span("Click a gauge to:", "## Dashboard Selector"),
     "A click runs the gauge's **Click action** and a long press its **Long click action**, if"
     " they are set in the gauge's Properties.\n\n"
     "### Double-Click\n"
     "Double-click a gauge to open an enlarged view of it in a resizable window (Esc closes"
     " it).\n\n"),
    ("features/dashboards/using", lit("1. Click the dropdown in the header"), "1. Click " + DASH_PICK),
    ("features/dashboards/using", span("### Categories\n", "## Full-Screen Mode"),
     "### Categories\n"
     "Dashboards are grouped:\n"
     "- **User**: dashboards in the project's `dashboards` folder (including imported ones)\n"
     "- **Reference**: the built-in dashboards\n\n"),
    ("features/dashboards/using", lit("**Settings → General → Dashboard → Gauge refresh rate**"),
     "**Settings " + A + " Dashboard " + A + " Gauge refresh rate**"),
    ("features/dashboards/using", lit("- **30 Hz (default)** — smoothest motion"),
     "- **30 Hz**\n- **60 Hz (default)** — smoothest motion"),
    ("features/dashboards/using", lit("**Settings → General → Dashboard → Right-align gauge values**"),
     "**Settings " + A + " Dashboard " + A + " Right-align numeric values**"),

    ("features/dashboards/customizing", lit("3. Select **Configure Gauge**"),
     "3. Select **Properties...** (or double-click the gauge in Designer Mode)"),
    ("features/dashboards/customizing", span("| Normal Color | Safe range color |", "\n\n"),
     "| Text | Title and value color |\n"
     "| Needle / bar | Needle or bar color |\n"
     "| Back | Gauge background |"),
    ("features/dashboards/customizing",
     lit("2. Drag corner handles to resize\n3. Maintain aspect ratio with Shift"),
     "2. Drag the bottom-right corner handle to resize"),
    ("features/dashboards/customizing", lit("2. Snap to grid (optional)"),
     "2. Snaps to the grid when **Settings " + A + " Dashboard " + A + " Snap gauges to the"
     " grid** is on (Shift+arrow moves in bigger steps)"),
    ("features/dashboards/customizing", lit("3. Or right-click → **Remove Gauge**"),
     "3. Or right-click " + A + " **Remove**"),

    ("features/dashboards/creating", span("1. Go to **View → Dashboard → New Dashboard**", "## Designer Mode"),
     "1. Click " + DASH_PICK + ", then **New...**\n"
     "2. Enter a name and choose what to start from: **Blank**, **Basic**, **Tuning**,"
     " **Telemetry** or **Copy of the current dashboard**\n\n"),
    ("features/dashboards/creating", span("2. Select **Add Gauge**", "### Drag and Drop from Sidebar"),
     "2. Select **Add gauge here** and choose a gauge type\n"
     "3. The gauge appears at the click location; set its channel in **Properties**\n\n"),
    ("features/dashboards/creating", span("### Drag and Drop from Sidebar", "## Layout Tips"),
     "### From the Gauge Palette\n"
     "1. In Designer Mode click **Add** in the toolbar\n"
     "2. Pick the channel in the palette (filter by name)\n"
     "3. Drag a gauge tile onto the dashboard (or click **Add gauge**)\n\n"
     "New gauges take their range and limits from the ECU definition when **Settings " + A +
     " Dashboard " + A + " Adopt gauge ranges from the ECU definition** is on; **Sync** in the"
     " designer toolbar does the same for all gauges.\n\n"
     "## Gauge Categories\n\n"
     "The palette groups gauge types as **Dials**, **Bars**, **Lines and graphs** and"
     " **Readouts** (or **All**).\n\n"),
    ("features/dashboards/creating",
     lit("- Enable **Snap to Grid** for clean layouts\n- Grid size configurable in Settings"),
     "- Enable **Settings " + A + " Dashboard " + A + " Snap gauges to the grid** for clean"
     " layouts"),
    ("features/dashboards/creating", span("## Setting Background", "## Saving Dashboards"),
     "## Setting Background\n\n"
     "1. In Designer Mode click **Assets**\n"
     "2. **Add file...** to embed a PNG or GIF\n"
     "3. Under **Dashboard background** pick the image and a style (**Tile**, **Stretch**,"
     " **Center**, **Fit**)\n\n"),
    ("features/dashboards/creating", to_end("## Saving Dashboards"),
     "## Saving Dashboards\n\n"
     "Click **Save** (or **Save as...**) in the designer toolbar; a `*` after the dashboard name"
     " means unsaved changes.\n\n"
     "### Dashboard Files\n"
     "Dashboards are TunerStudio-compatible `.dash` files in the project's `dashboards`"
     " folder.\n\n"
     "## Sharing Dashboards\n\n"
     "In the Select Dashboard window, **Export...** saves a copy of a dashboard as a `.dash`"
     " file. **Import...** adds a `.dash` file (with a compatibility check against the ECU"
     " definition) and **From TunerStudio...** imports the dashboards of a TunerStudio"
     " project.\n"),

    ("features/dashboards/validation",
     lit("When a dashboard loads, LibreTune runs a validation check and shows a **Validate** button in the dashboard header if any issues are detected. Click it to view details."),
     "LibreTune checks a dashboard when it loads and after every change. The **Validate** button"
     " in the dashboard toolbar shows the number of issues (\"Validate (3)\"); click it to see"
     " them, and click an issue to select that gauge in Designer Mode."),
    ("features/dashboards/validation",
     lit("- **Warnings**: Non-critical issues (e.g., tiny gauges, poor contrast, out-of-bounds elements)."),
     "- **Warnings**: Non-critical issues (tiny gauges, gauges outside the dashboard, overlapping"
     " gauges, more than 50 components)."),

    ("features/math-channels", lit("| `timeNow()` | Current system time (ms) | `timeNow() / 1000` |"),
     "| `timeNow()` | Seconds since the session started | `timeNow() / 60` |"),

    ("features/datalog", span("2. Go to **Tools → Start Logging** or press `Ctrl+L`", "## Stopping a Log"),
     "2. Go to **Tools " + A + " Start Data Logging** or press `Ctrl+L` (or **Start recording** in"
     " **Tools " + A + " Data Logging**)\n"
     "3. Recording starts at once. The sample rate (1-200 Hz, default 10) and file format are in"
     " **Settings " + A + " Data Logging**; **Channels...** in the Data Logging tab chooses the"
     " recorded channels.\n\n"),
    ("features/datalog", lit("Click **Stop Logging** or press `Ctrl+L` again."),
     "Click **Tools " + A + " Stop Data Logging** (or **Stop recording**) or press `Ctrl+L`"
     " again."),
    ("features/datalog", lit("as `.ltlog` files."),
     "as `.mlg` (MegaLogViewer binary, the default) or `.msl` (text) files, named by date and"
     " time."),
    ("features/datalog", span("## Log File Format", "## Data Log Viewer"),
     "## Log File Format\n\n"
     "LibreTune records MegaLogViewer `.mlg` (binary) or `.msl` (text) logs, so they also open"
     " in MegaLogViewer and TunerStudio. The viewer opens `.mlg`, `.msl` and `.csv` logs.\n\n"),
    ("features/datalog", span("1. Go to **Tools → Data Log Viewer**", "### Playback Controls"),
     "1. Go to **Tools " + A + " Datalog Viewer...**\n"
     "2. Pick a log in the list of the project's `datalogs` folder, or click **Open...**\n"
     "3. Select a `.mlg`, `.msl` or `.csv` file\n\n"),
    ("features/datalog", lit("| ⏪⏩ Skip | Jump forward/backward |"), "| Start | Go back to the beginning |"),
    ("features/datalog", lit("| Speed | 0.5x, 1x, 2x playback speed |"),
     "| Speed | 0.25x to 8x playback speed |\n| Loop / Fit | Repeat playback / show the whole log |"),
    ("features/datalog", lit("When playing back a log:"),
     "When playing back a log while offline with **Drive gauges** checked:"),
    ("features/datalog", span("## Automatic Logging", "## Log Analysis Tips"),
     "## Automatic Logging\n\n"
     "**Smart recording** (Settings " + A + " Data Logging, or **Auto-record** in the Data"
     " Logging tab) starts a log when RPM rises above a threshold and stops it after RPM stays"
     " below it for a timeout. Recording also stops when the ECU disconnects. See [Smart"
     " Recording](smart-recording.md).\n\n"),
    ("features/datalog",
     lit("`.ltlog` files can be opened in LibreTune. Older CSV logs remain readable. TunerStudio `.msl` / `.mlg` can be imported for analysis."),
     "Recorded `.mlg` / `.msl` logs open in MegaLogViewer and TunerStudio. **Export...** in the"
     " Datalog Viewer saves the open log as `.msl`, `.mlg` or `.csv`."),

    ("features/alert-rules", lit("Open **Settings → Alert Rules** and set:"),
     "Open **Settings " + A + " Data Logging** (the **Alert rules** group) and set:"),
    ("features/alert-rules", lit("## Recommended Defaults"),
     "The same group has **channel limits**: a channel above or below a value gives a message"
     " and a marker in the data log.\n\n## Recommended Defaults"),

    ("features/smart-recording", lit("4. **Apply** → Click the Apply button to save"),
     "4. Changes are saved as soon as you make them"),
    ("features/smart-recording",
     lit("- **Live Mode**: Auto-record toggle appears in the recording controls"),
     "- The **Auto-record** checkbox is in the recording controls of **Tools " + A + " Data"
     " Logging**"),
    ("features/smart-recording", lit("\n- Current key state displays as `[on]` or `[off]` when enabled"), ""),
    ("features/smart-recording", lit("- **Normal commute**: 1.5 seconds (catches most stops)"),
     "- **Normal commute**: 2 seconds (catches most stops)"),
    ("features/smart-recording", lit("4. Click Apply and close Settings"),
     "4. Close Settings (changes are saved immediately)"),
    ("features/smart-recording", lit("1. **Open Data Logger** (View → Data Logger)"),
     "1. **Open Data Logging** (Tools " + A + " Data Logging)"),
    ("features/smart-recording", lit("   - Status shows `[on]` in the toggle\n"), ""),
    ("features/smart-recording", lit("   - Status shows `[off]` in the toggle\n"), ""),
    ("features/smart-recording", lit("7. **Review data** in playback mode"),
     "7. **Review data** in **Tools " + A + " Datalog Viewer...**"),
    ("features/smart-recording",
     lit("- Toggle view mode to \"Playback\" to review recorded data\n"
         "- Open Statistics panel (click Stats button) to analyze:"),
     "- Open the log in **Tools " + A + " Datalog Viewer...**\n"
     "- Check **Stats** to analyze:"),
    ("features/smart-recording", span("### Scenario 4: Diagnostic Logging with Tooth Logger", "## Troubleshooting"), ""),
    ("features/smart-recording", lit("2. Is view mode set to **\"Live\"**? (auto-record only works in live mode)"),
     "2. Is the ECU connected (or the demo running)? Auto-record works in the background whether"
     " or not the Data Logging tab is open."),
    ("features/smart-recording", lit("- Check Data Logger is in Live mode (not Playback)\n"), ""),
    ("features/smart-recording", lit("(slice in playback mode if available)"), "(in the Datalog Viewer)"),
    ("features/smart-recording", lit("3. Click **Apply** button (not just close dialog)\n4. Restart app to verify persistence"),
     "3. Settings are saved when changed (in `logging.conf` in the settings folder)\n"
     "4. Restart the app to verify"),
    ("features/smart-recording", lit("(File → Export) as backup before AutoTune analysis"),
     "(Datalog Viewer " + A + " **Export...**) before AutoTune analysis"),
    ("features/smart-recording", lit("- Export logs as CSV (File → Export Log as CSV)"),
     "- Export logs as CSV if needed (Datalog Viewer " + A + " **Export...**); AutoTune can also"
     " replay a log directly"),
    ("features/smart-recording", lit("- Open Statistics panel (click Stats button in Data Logger)"),
     "- Check **Stats** in the Datalog Viewer"),
    ("features/smart-recording", span("### With Tooth Logger", "## See Also"), ""),

    ("features/data-statistics", span("1. **Record or load a log** in Data Logger", "## Statistical Measures Explained"),
     "1. Check **Stats** in **Tools " + A + " Data Logging** (live data) or **Tools " + A +
     " Datalog Viewer...** (a log)\n"
     "2. The table lists the channels of the selected graph pane, over the visible time range"
     " (zoom to analyze a section)\n\n"
     "### Quick Stats Display\n\n"
     "One row per channel: Mean, Median, Std dev, Min, Max, P25, P75, IQR.\n\n"),
    ("features/data-statistics", lit("4. Save log: \"baseline_tune_log.csv\""),
     "4. Rename the log (Datalog Viewer " + A + " Rename): \"baseline_tune_log\""),
    ("features/data-statistics", lit("4. Save log: \"modified_tune_log.csv\""),
     "4. Rename the log: \"modified_tune_log\""),
    ("features/data-statistics", lit("Open both logs in Data Logger playback:"),
     "Open each log in the Datalog Viewer and note its statistics:"),
    ("features/data-statistics", lit("The Statistics Panel in the Data Logger provides"),
     "The statistics table of the Data Logging tab and the Datalog Viewer provides"),
]

# ---------------------------------------------------------------------------------------------
# Table editing, shortcuts, AutoTune
AUTH_ROWS = (
    "| **Max change** | Largest change per cell, in table units (default 10) |\n"
    "| **Max change %** | Largest change per cell, in percent of its value (default 20) |\n"
    "| **Min / Max cell value** | Allowed range of a recommended value (default 0 / 255) |\n")
LOCK_STEPS = ("2. Press **L** or click **Lock** (again to unlock; **Unlock** clears all)\n"
              "3. Locked cells are tinted and marked \"L\"")
FIXES += [
    ("features/table-editing", lit("- Hold Shift for larger steps (10x)"),
     "- `q` / `w`: small step; Ctrl with `+` / `-`: the step times the multiplier;"
     " Ctrl+PageUp / PageDown: change by a percentage (Right-click > **Set increments...**)"),
    ("features/table-editing", lit("Applies Gaussian smoothing to reduce abrupt transitions. Higher factors blend values more."),
     "Applies a 3x3 Gaussian smoothing to the selected cells to reduce abrupt transitions."),
    ("features/table-editing", span("- The file's dimensions must match the table's **current** size exactly.", "## Undo/Redo"),
     "- **Load** shows the file's and the table's size. If they match you can also load the"
     " file's axis values (checkbox). If they differ, the values are interpolated onto this"
     " table's axes and the axes stay as they are. The rest of the tune is untouched; burn to"
     " ECU when you're happy with the result.\n\n"),
    ("features/table-editing", span("By default the Y axis (load) starts at the top", "## Follow Mode"),
     "By default the lowest load row is at the bottom, like a classic graph. The **Y axis**"
     " toolbar button (arrows icon) flips the open table only. **Settings " + A + " Table"
     " Display " + A + " Y axis starts at the bottom (new table tabs)** sets the default for"
     " tables opened later.\n\n"),
    ("features/table-editing", span("to \"where is the engine operating right now?\". It is drawn in both the", "## Editing Curves"),
     "to \"where is the engine operating right now?\". It is drawn in table tabs (tables"
     " embedded in dialogs show only the live cell).\n\n"
     "- Older segments fade out after the *Trail fade* time (Settings " + A + " Table Display,"
     " 0 = never fade)\n"
     "- The position comes from the table's axis channels; a table without them has no live"
     " cell\n"
     "- `F` toggles follow mode; the **Trail** toolbar button (or Settings " + A + " Table"
     " Display) shows or hides the trail; the colors are the *Custom cursor color* / *Custom"
     " trail color* settings\n\n"),
    ("features/table-editing", lit("- **Drag to edit** — Click anywhere on the chart to grab the nearest point and"),
     "- **Drag to edit** — Grab a point (click on or next to it; with **MoveX** on it also"
     " moves sideways) and"),

    ("features/table-editing/2d-tables", lit("- **Average**: Average with existing values"),
     "- **Subtract**: Subtract pasted values\n"
     "- **Keep the lower / Keep the higher**: Keep the smaller or larger of the pasted and"
     " existing value"),
    ("features/table-editing/2d-tables",
     lit("the *Trail fade* setting; if the table's INI declares no axis channels, the\n"
         "standard `rpm` / `map` channels drive the cursor."),
     "the *Trail fade* setting (Settings " + A + " Table Display). A table whose INI declares"
     " no axis channels has no live cursor."),

    ("features/table-editing/3d-visualization", span("> The 3D toggle is available on every table editor", "\n\n"),
     "> The 3D view is part of the table tab (toolbar, right-click menu or key `3`). A table"
     " embedded in a dialog has an **Open in editor** button that opens it in a tab."),
    ("features/table-editing/3d-visualization", lit("- **+/-**: Zoom in/out"),
     "- **+/-**: Zoom in/out (PageUp/PageDown or [ ] also zoom; once a cell is picked in 3D,"
     " +/- step that cell)\n\nThe keys act while the mouse pointer is over the 3D view."),

    ("features/table-editing/shortcuts", lit("| Tab | Move to next cell |\n| Shift+Tab | Move to previous cell |"),
     "| Tab / Shift+Tab | While typing a value: commit it and move to the next / previous"
     " cell |"),
    ("features/table-editing/shortcuts", lit("| Ctrl+X | Cut selection |\n"), ""),
    ("features/table-editing/shortcuts", lit("| Ctrl+0 | Zoom to fit |\n| Ctrl++ | Zoom in |\n| Ctrl+- | Zoom out |\n"),
     "| G | Jump to the live cell |\n| H / V | Interpolate horizontally / vertically |\n"
     "| Ctrl+R / Ctrl+D | Fill right / down |\n"),
    ("features/table-editing/shortcuts", lit("Shortcuts can be customized in **Settings → Keyboard Shortcuts**."),
     "Application shortcuts (File, Edit, tabs, AutoTune and so on) can be changed in **Settings"
     " " + A + " Keyboard Shortcuts**; the table editing keys above are fixed."),

    ("reference/shortcuts", lit("| Ctrl+O | Open project / Load tune |"), "| Ctrl+O | Load tune |"),
    ("reference/shortcuts", lit("| F11 | Toggle full screen |"), "| F11 | Full-screen dashboard |"),
    ("reference/shortcuts", lit("| Ctrl+? | Keyboard shortcuts |"),
     "| Ctrl+K | Search the sidebar and settings |\n| F9 | Show / hide the sidebar |\n"
     "| Ctrl+, | Settings |"),

    # autotune
    ("features/autotune",
     lit("- **From Table**: Use ECU's AFR target table\n- **Fixed Value**: Use a constant target (e.g., 14.7:1)"),
     "- **Target table**: the INI's AFR target table by default, or another AFR target table"
     " (Settings " + A + " Reference tables)\n"
     "- **Target AFR** (or lambda): a flat target used when the definition has no target"
     " table"),
    ("features/autotune", lit("### Update Controller\nWhen enabled, recommendations are automatically sent to the ECU."),
     "### Update mode\n**Send to ECU RAM** or **Send and burn**: what the **Send to ECU** button"
     " does. Nothing is sent automatically."),
    ("features/autotune", span("| **Max Increase** | Maximum % to increase any cell |", "\n## Filters"),
     AUTH_ROWS),
    ("features/autotune", span("| **Min RPM** | Ignore data below this RPM | 800 |", "| **Exclude Accel Enrich**"),
     "| **Min RPM** | Ignore data below this RPM | 1000 |\n"
     "| **Max RPM** | Ignore data above this RPM | 7000 |\n"
     "| **Min coolant** | Ignore when engine is cold | 160 (or the INI's value) |\n"
     "| **Max TPS rate** | Ignore rapid throttle changes | 50 %/s |\n"
     "| **Custom filter** | Expression; matching samples are rejected | (empty) |\n"),
    ("features/autotune", lit("- ⚪ **Gray**: Insufficient data"), "- Cells without data show the table's own value"),
    ("features/autotune", lit("#### Cell Weighting"), "#### Hit weighting (and Coverage)"),
    ("features/autotune", lit("#### Cell Change"), "#### Change"),
    ("features/autotune", lit("2. Right-click → **Lock Selected Cells**\n3. Locked cells show a 🔒 icon"),
     LOCK_STEPS),

    ("features/autotune/usage-guide",
     lit("- Secondary: Tick the checkbox to tune a second VE table simultaneously; if the tune has only one fuel table, the selector shows \"No other fuel tables\""),
     "- Secondary: tick **Tune a second table in this session** (Session panel); with only one"
     " fuel table clibretune says \"No second fuel table to tune in this definition\""),
    ("features/autotune/usage-guide", lit("\nAlgorithm: Simple (recommended for beginners)"), ""),
    ("features/autotune/usage-guide", span("- On Speeduino the load-axis channel is named `fuelLoad`", "- Selecting a load source never"),
     "- Choosing a source whose channel the definition lacks keeps the current one and says"
     " so\n"),
    ("features/autotune/usage-guide", lit("Max Increase:   5%\nMax Decrease:   5%\nAbsolute Max:  15%"),
     "Max change %:   5\nMax change:     5   (table units)"),
    ("features/autotune/usage-guide", lit("- You do a track day: Lower Min TPS to 0 for coast-down testing\n"), ""),
    ("features/autotune/usage-guide", span("**While running — the sample indicator**", "**Step 6: Click Start**"),
     "**While running — the Statistics group**\n\n"
     "The **Statistics** group (Health panel) shows \"Samples: N accepted, M rejected\" and the"
     " three most frequent rejection reasons. **If AutoTune looks like nothing is happening,"
     " check this first** — rejections counting up with few accepted samples mean the filters"
     " (usually Min coolant while warming up, or Max TPS rate) are discarding everything.\n\n"),
    ("features/autotune/usage-guide", lit("- Status changes to \"Running\"\n- UI shows \"Waiting for data...\""),
     "- A green **RUNNING** badge appears next to the title"),
    ("features/autotune/usage-guide", span("the fuel-table editors have: a line through the recently visited cells", "**Y-Axis Order**"),
     "of the cells the operating point visited while AutoTune is collecting, with a dot on the"
     " newest one. Toggle it with the **Trace line** button in the grid toolbar (or the Report"
     " panel checkbox).\n\n"),
    ("features/autotune/usage-guide", lit("zero-at-top and zero-at-bottom — the same global setting the table editors\nuse."),
     "zero-at-top and zero-at-bottom for this tab (it starts with zero at the bottom)."),
    ("features/autotune/usage-guide", lit("Heat Map View: Cell Weighting (shows data coverage)"),
     "Heat Map View: Hit weighting or Coverage (shows data coverage)"),
    ("features/autotune/usage-guide", lit("Switch to: Cell Change (shows correction magnitude)"),
     "Switch to: Change (shows correction magnitude)"),
    ("features/autotune/usage-guide", lit("   Max Increase:  10%\n   Max Decrease:  10%\n   Absolute Max:  30%"),
     "   Max change %:  10\n   Max change:    10   (table units)"),
    ("features/autotune/usage-guide", lit("1. Tick \"Secondary:\""), "1. Tick \"Tune a second table in this session\""),
    ("features/autotune/usage-guide", lit("measures against, chosen during the preflight check."),
     "measures against (Settings " + A + " Reference tables " + A + " Target table)."),
    ("features/autotune/usage-guide",
     lit("The Primary, Secondary, and Load Source selectors stay live while a session\nruns — no need to stop and restart:"),
     "The Primary and Secondary selectors stay live while a session runs (Load source changes"
     " only while stopped) — no need to stop and restart:"),
    ("features/autotune/usage-guide",
     lit("1. Right-click cell in grid\n2. Select \"Lock Selected Cells\"\n3. Locked cells show 🔒 icon"),
     "1. Select cells\n2. Press L or click Lock\n3. Locked cells are tinted and marked \"L\""),
    ("features/autotune/usage-guide", lit("File → Export AutoTune Data\nOpens save dialog"),
     "Report panel " + A + " Export recommendations (CSV)..."),
    ("features/autotune/usage-guide", lit("4. Loosen filters: Lower Min RPM, Min TPS"),
     "4. Loosen filters: lower Min RPM / Min coolant, raise Max TPS rate"),
    ("features/autotune/usage-guide",
     span("**Check the sample indicator first.** While running, the indicator under the", "\n\n"),
     "**Check the Statistics group first.** While running, the **Statistics** group (Health"
     " panel) says how many samples were accepted and rejected and which filters reject most:"),
    ("features/autotune/usage-guide",
     lit("Ctrl+Shift+E     Export data\nTab              Switch between tables\nF5               Refresh heat map"),
     "Space            Start / stop\nL                Lock / unlock selected cells\n"
     "Ctrl+Z           Undo the last Send to ECU\n+ / - / Ctrl+0   Zoom in / out / 100%"),

    ("features/autotune/setup", lit("- **Other**: Any table that affects AFR\n"), ""),
    ("features/autotune/setup", span("**From AFR Target Table** (Recommended)", "### Update Mode"),
     "**Target table** (Settings " + A + " Reference tables): the INI's AFR target table"
     " (default) or another AFR target table. It supports different targets for WOT and"
     " cruise.\n\n"
     "**Target AFR**: a flat target used when the definition has no target table.\n\n"),
    ("features/autotune/setup", span("### Update Mode", "## Filter Configuration"),
     "### Update Mode\n\n"
     "Recommendations are only sent when you press **Send to ECU**; **Update mode** (Session"
     " panel) decides what that does:\n\n"
     "- **Send to ECU RAM**: try the change; burn later\n"
     "- **Send and burn**: also burns it to ECU flash\n\n"),
    ("features/autotune/setup", lit("- **Min CLT**: Ignore cold engine data (160°F+)"),
     "- **Min coolant**: Ignore cold engine data (default 160, or the INI's value)"),
    ("features/autotune/setup", lit("### Throttle Filter\n- **Min TPS**: Ignore closed throttle (1%+)\n- Helps avoid decel enleanment data\n\n"),
     "### Custom Filter\n- An expression over the output channels; matching samples are"
     " rejected\n\n"),
    ("features/autotune/setup", lit("- **Max TPS Rate**: Ignore rapid throttle (10%/sec)"),
     "- **Max TPS rate**: Ignore rapid throttle (default 50 %/s)"),
    ("features/autotune/setup", span("### Maximum Change Per Cell", "## Lambda Delay Compensation"),
     "### Maximum Change Per Cell\n"
     "- **Max change** / **Max change %**: largest change per cell (table units / percent of"
     " its value)\n"
     "- **Min / Max cell value**: allowed range for recommendations\n\n"
     "The limits apply to the table as it was when the data was collected; after each Send to"
     " ECU they start again from the new values.\n\n"
     "### Starting Values\n"
     "| Experience | Max change % |\n"
     "|------------|--------------|\n"
     "| Beginner | 5 |\n"
     "| Intermediate | 10 |\n"
     "| Expert | 15 |\n\n"),

    ("features/autotune/recommendations", lit("Correction = Current VE × (1 - Error)"),
     "Correction = Current VE × (Actual AFR / Target AFR)"),
    ("features/autotune/recommendations",
     lit("2. **Weighted average**: Recent samples weighted more\n3. **Confidence**: More hits = more confidence"),
     "2. **Weighted average**: each sample is weighted by the **Hit weighting** mode (uniform,"
     " or by distance to the cell center)\n"
     "3. **Confidence**: the change is phased in until the accumulated weight reaches **Base"
     " weight**; changes smaller than **Min change** are dropped"),
    ("features/autotune/recommendations", lit("- ⬜ **Gray**: No data or insufficient hits"),
     "- Cells without data show the table's own value"),
    ("features/autotune/recommendations", lit("### Cell Weighting Map"), "### Hit Weighting and Coverage Maps"),
    ("features/autotune/recommendations", lit("### Cell Change Map"), "### Change Map"),
    ("features/autotune/recommendations", span("**Minimum hits** before recommendation is trusted:", "## Filtering Effects"),
     "Each cell shows its hit count. Instead of a minimum hit count, **Base weight** (Settings)"
     " sets how much accumulated weight a cell needs before its full correction is shown;"
     " **Min change** hides smaller changes.\n\n"),
    ("features/autotune/recommendations",
     span("Final = clamp(Recommendation,", "## Interpreting Results"),
     "Final = clamp(Recommendation,\n"
     "              Current ± Max change,\n"
     "              Current ± Max change % of Current)\n"
     "then limited to Min / Max cell value\n"
     "```\n\n"),

    ("features/autotune/filters", lit("| Min RPM | Ignore below this | 800 |\n| Max RPM | Ignore above this | 6500 |"),
     "| Min RPM | Ignore below this | 1000 |\n| Max RPM | Ignore above this | 7000 |\n\n"
     "When the INI declares its own value, a \"Differs from INI value\" note with **Use INI"
     " value** appears."),
    ("features/autotune/filters", lit("| Min CLT | Ignore below this | 160°F |"),
     "| Min coolant | Ignore below this | 160 (or the INI's value) |"),
    ("features/autotune/filters",
     lit("| Min TPS | Ignore closed throttle | 1% |\n| Max TPS Rate | Ignore rapid changes | 10%/sec |"),
     "| Max TPS Rate | Ignore rapid changes | 50 %/s |"),
    ("features/autotune/filters",
     lit("- Raise min TPS if decel noise is an issue\n- Lower max rate if you want more data (less filtering)"),
     "- Raise the max rate if you want more data (less filtering)\n"
     "- Use **Custom filter** for anything else (an expression; matching samples are"
     " rejected)"),
    ("features/autotune/filters", span("### Per-Update Limits", "### Progressive Strategy"),
     "### Per-Update Limits\n\n"
     "| Setting | Description | Default |\n"
     "|---------|-------------|---------|\n"
     "| Max change | Largest change per cell, table units | 10 |\n"
     "| Max change % | Largest change per cell, percent | 20 |\n"
     "| Min / Max cell value | Allowed range | 0 / 255 |\n\n"
     "Each time changes are sent, cells can only change by this much from the values the data"
     " was collected on.\n\n"),
    ("features/autotune/filters",
     span("| Stage | Per-Update | Absolute | When |", "## Filter Indicators"),
     "| Stage | Max change % | When |\n"
     "|-------|--------------|------|\n"
     "| Initial | 5 | Base tune rough |\n"
     "| Refining | 10 | Getting close |\n"
     "| Fine-tuning | 15 | Nearly dialed |\n\n"),
    ("features/autotune/filters", span("### Status Bar\n", "## Troubleshooting Filters"),
     "### Statistics\n"
     "The Health panel's **Statistics** group shows how many samples were accepted and"
     " rejected and the most frequent rejection reasons.\n\n"
     "### Cell Marks\n"
     "- **L** (tinted cell): locked, excluded from updates (L key / Lock button)\n"
     "- Red outline: anomaly (when \"Outline anomalies\" is on)\n\n"),
]
