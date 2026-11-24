Native builds for each operating system. No installer, no runtime dependencies.

| Platform | Download | How to run |
|---|---|---|
| Linux x86-64 | `clibretune-linux-x86_64` | `chmod +x clibretune-linux-x86_64 && ./clibretune-linux-x86_64` |
| Linux ARM64 | `clibretune-linux-aarch64` | `chmod +x clibretune-linux-aarch64 && ./clibretune-linux-aarch64` |
| Windows x86-64 | `clibretune-windows-x86_64.exe` (console) or `clibretune-windows-x86_64-gui.exe` (no console window) | double-click, or run from a terminal |
| macOS (Intel + Apple Silicon) | `clibretune-macos-universal` | `chmod +x clibretune-macos-universal && ./clibretune-macos-universal` |

Try it without an ECU: press **Try the demo** on the start screen, or run the binary with `--demo`.
Pick a UI backend with `LT_BACKEND=x11|cocoa|win32|web|tty`.

macOS Gatekeeper: the binary is ad-hoc signed, not notarized. If macOS refuses to open it, run
`xattr -dr com.apple.quarantine clibretune-macos-universal` once.

Verify downloads against `SHA256SUMS`.
