One self-contained binary that runs on Linux, macOS, Windows and the BSDs (x86-64 and ARM64), built with [Cosmopolitan](https://github.com/jart/cosmopolitan). No installer, no runtime dependencies.

| Platform | How to run |
|---|---|
| Linux / BSD | `chmod +x libretune.com && ./libretune.com` |
| macOS | `chmod +x libretune.com && ./libretune.com` (opens the UI in your browser) |
| Windows | download `libretune.exe` and run it |

Try it without an ECU: download `demo.ini` too, then `./libretune.com --demo --ini demo.ini`. Pick a UI backend with `LT_BACKEND=x11|win32|web|tty`.

`libretune.com` and `libretune.exe` are the same file; Windows just needs the `.exe` name. Verify downloads against `SHA256SUMS`.
