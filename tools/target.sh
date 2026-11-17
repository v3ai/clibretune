# tools/target.sh - per-OS file selection and flags. SOURCED (not executed) by
# build.sh and tools/check_universal.sh, so both always agree.
#
# Platform code is chosen here, by the build system, never by #ifdef
# (docs/UNIVERSAL_C.md section 11). Variables set after sourcing:
#   TARGET        linux | macos | windows | bsd
#   PLAT          posix | win32               (selects src/plat/<PLAT>/)
#   GUIOS         unix | macos | win32        (selects src/gui/os/<GUIOS>/)
#   PLAT_CPPFLAGS feature-test macros for platform files only
#   TARGET_LIBS   libraries/frameworks to link
#   EXE           ".exe" on Windows, empty elsewhere
# Functions: lt_host_target, lt_sources, lt_platform_files, lt_is_platform_file

lt_host_target() {
    case $(uname -s 2>/dev/null) in
        Linux*) echo linux ;;
        Darwin*) echo macos ;;
        FreeBSD*|NetBSD*|OpenBSD*|DragonFly*) echo bsd ;;
        MINGW*|MSYS*|CYGWIN*) echo windows ;;
        *) echo linux ;;
    esac
}

TARGET=${TARGET:-$(lt_host_target)}
case $TARGET in
    linux)
        PLAT=posix; GUIOS=unix; EXE=""
        PLAT_CPPFLAGS="-D_POSIX_C_SOURCE=200112L -D_XOPEN_SOURCE=600 -D_DEFAULT_SOURCE"
        TARGET_LIBS="-lm" ;;
    bsd)
        PLAT=posix; GUIOS=unix; EXE=""
        PLAT_CPPFLAGS=""
        TARGET_LIBS="-lm" ;;
    macos)
        PLAT=posix; GUIOS=macos; EXE=""
        PLAT_CPPFLAGS=""
        TARGET_LIBS="-lm -framework Cocoa -framework CoreGraphics -framework CoreFoundation" ;;
    windows)
        # Console subsystem on purpose: --help/--version print to the console.
        # (Add -mwindows to LDFLAGS for a console-less GUI-only release build.)
        PLAT=win32; GUIOS=win32; EXE=".exe"
        PLAT_CPPFLAGS=""
        TARGET_LIBS="-lws2_32 -lgdi32 -luser32 -lshell32 -ladvapi32" ;;
    *)
        echo "unknown TARGET '$TARGET' (use linux, macos, windows or bsd)" >&2
        exit 2 ;;
esac

# Platform-specific source files for this TARGET (one per line).
lt_platform_files() {
    case $PLAT in
        posix)
            ls src/plat/posix/*.c | grep -v '/os_[a-z0-9]*\.c$'
            ls "src/plat/posix/os_$TARGET.c" ;;
        win32)
            ls src/plat/win32/*.c ;;
    esac
    ls src/gui/os/$GUIOS/*.c
}

# Every translation unit linked into the app for this TARGET, except main.c.
lt_sources() {
    find src -name '*.c' ! -path src/main.c ! -path 'src/plat/posix/*' \
        ! -path 'src/plat/win32/*' ! -path 'src/gui/os/*' | sort
    lt_platform_files
}

# True if $1 is a per-platform file (may use OS headers and PLAT_CPPFLAGS).
lt_is_platform_file() {
    case $1 in
        src/plat/posix/*|src/plat/win32/*|src/gui/os/*|tests/os/*) return 0 ;;
    esac
    return 1
}
