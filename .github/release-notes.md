Native builds for each operating system. No installer, no runtime dependencies.

| Platform | Download | How to run |
|---|---|---|
| Linux x86-64 | `libretune-linux-x86_64` | `chmod +x libretune-linux-x86_64 && ./libretune-linux-x86_64` |
| Linux ARM64 | `libretune-linux-aarch64` | `chmod +x libretune-linux-aarch64 && ./libretune-linux-aarch64` |
| Windows x86-64 | `libretune-windows-x86_64.exe` (console) or `libretune-windows-x86_64-gui.exe` (no console window) | double-click, or run from a terminal |
| macOS (Intel + Apple Silicon) | `libretune-macos-universal` | `chmod +x libretune-macos-universal && ./libretune-macos-universal` |

Try it without an ECU: press **Try the demo** on the start screen, or run the binary with `--demo`.
Pick a UI backend with `LT_BACKEND=x11|cocoa|win32|web|tty`.

macOS Gatekeeper: the binary is ad-hoc signed, not notarized. If macOS refuses to open it, run
`xattr -dr com.apple.quarantine libretune-macos-universal` once.

Verify downloads against `SHA256SUMS`.
