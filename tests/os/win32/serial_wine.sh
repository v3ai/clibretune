#!/bin/sh
# serial_wine.sh - run test_win32_serial.exe under wine against a pty.
#
#   tests/os/win32/serial_wine.sh path/to/test_win32_serial.exe
#
# Wine maps COM1 to the Unix device named by HKLM\Software\Wine\Ports\Com1
# (it re-creates dosdevices/com1 at every server start, so a hand-made symlink
# is overwritten). That value is set once to $WINEPREFIX/com1_pty, a symlink
# this script re-points at a fresh pty slave on every run, so no wine server
# restart is needed. A Python peer owns the pty master and answers the test's
# protocol ("ping" -> "pong", 300 bytes -> their sum as 4 hex digits).
# Needs wine and python3; skips (exit 0) without them. Uses $WINEPREFIX
# (build.sh exports a private one for the windows test run).
exe=$1
[ -f "$exe" ] || { echo "usage: $0 test_win32_serial.exe" >&2; exit 2; }
command -v wine >/dev/null 2>&1 || { echo "serial_wine: no wine, skipped"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "serial_wine: no python3, skipped"; exit 0; }
prefix=${WINEPREFIX:-$HOME/.wine}
[ -d "$prefix/dosdevices" ] || { echo "serial_wine: no wine prefix, skipped"; exit 0; }
export WINEPREFIX="$prefix" WINEDEBUG=-all
link="$prefix/com1_pty"
if [ ! -f "$prefix/.lt_com1" ]; then
    # one-time: point COM1 at the link, then restart the server so it re-reads
    wine reg add 'HKLM\Software\Wine\Ports' /v Com1 /t REG_SZ /d "$link" /f >/dev/null 2>&1
    wine wineboot -k >/dev/null 2>&1
    sleep 1
    : > "$prefix/.lt_com1"
fi
exec python3 -I - "$exe" "$link" <<'PY'
import os, pty, subprocess, sys, threading, time, tty

exe, link = sys.argv[1], sys.argv[2]
master, slave = pty.openpty()
tty.setraw(slave)
try:
    os.remove(link)
except OSError:
    pass
os.symlink(os.ttyname(slave), link)
stop = False


def peer():
    buf = b""
    while not stop:
        try:
            d = os.read(master, 4096)
        except OSError:
            time.sleep(0.01)
            continue
        buf += d
        if buf.startswith(b"ping"):
            os.write(master, b"pong")
            buf = buf[4:]
        if len(buf) >= 300:
            os.write(master, ("%04X" % (sum(buf[:300]) & 0xFFFF)).encode())
            buf = buf[300:]


threading.Thread(target=peer, daemon=True).start()
env = dict(os.environ, LT_TEST_COM="COM1", WINEDEBUG="-all")
try:
    r = subprocess.run(["wine", exe], env=env, timeout=60, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    code = r.returncode
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        # wine's server dumps its object table after a pty COM port closes; drop it
        if not line.startswith("0x"):
            print(line)
except subprocess.TimeoutExpired:
    print("serial_wine: timeout")
    code = 1
stop = True
try:
    os.remove(link)
except OSError:
    pass
sys.exit(code)
PY
