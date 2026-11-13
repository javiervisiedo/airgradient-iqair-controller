#!/usr/bin/env bash
# Wrapper for common PlatformIO operations on the AtomS3 Lite.
# Usage: pio.sh {build|upload|monitor|capture <secs>|test|port|clean|size} [extra pio args]
set -euo pipefail

ENV="${PIO_ENV:-atoms3}"
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"   # .agents/skills/<skill>/scripts -> project root
cd "$ROOT"

PIO_BIN="$(command -v pio || true)"
if [[ -z "$PIO_BIN" && -x "$HOME/.platformio/penv/bin/pio" ]]; then
  PIO_BIN="$HOME/.platformio/penv/bin/pio"
fi
[[ -n "$PIO_BIN" ]] || { echo "ERROR: pio not found (checked PATH and ~/.platformio/penv/bin/pio)." >&2; exit 2; }
pio() { "$PIO_BIN" "$@"; }
[[ -f platformio.ini ]] || { echo "ERROR: no platformio.ini in $ROOT." >&2; exit 2; }

find_port() {
  if [[ -n "${UPLOAD_PORT:-}" ]]; then echo "$UPLOAD_PORT"; return; fi
  # Espressif native USB (VID 303A) shows up as usbmodem on macOS, ttyACM on Linux.
  local p
  p=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -n1 || true)
  if [[ -z "$p" ]]; then
    echo "ERROR: no AtomS3 serial port found. Check the USB-C cable (must carry data)," >&2
    echo "       or enter download mode: hold button G41 while plugging in." >&2
    exit 3
  fi
  local n; n=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | wc -l | tr -d ' ')
  [[ "$n" -gt 1 ]] && echo "WARN: $n candidate ports, using $p (override with UPLOAD_PORT=...)" >&2
  echo "$p"
}

cmd="${1:-build}"; shift || true
case "$cmd" in
  build)   pio run -e "$ENV" "$@" ;;
  size)    pio run -e "$ENV" -t size "$@" ;;
  clean)   pio run -e "$ENV" -t clean "$@" ;;
  port)    find_port ;;
  upload)
    PORT=$(find_port); echo ">> Flashing env=$ENV to $PORT"
    pio run -e "$ENV" -t upload --upload-port "$PORT" "$@" ;;
  monitor)
    PORT=$(find_port); echo ">> Monitoring $PORT (Ctrl+C to exit)"
    pio device monitor -e "$ENV" -p "$PORT" "$@" ;;
  capture)
    SECS="${1:-20}"; PORT=$(find_port)
    # Use the Python interpreter that runs PlatformIO (it ships pyserial).
    PY=$(head -1 "$PIO_BIN" | sed 's/^#!//'); [[ -x "$PY" ]] || PY=python3
    "$PY" - "$PORT" "$SECS" <<'EOF'
import sys, time, serial
port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.2)
end = time.time() + secs
while time.time() < end:
    line = s.readline()
    if line:
        sys.stdout.write(time.strftime("%H:%M:%S ") + line.decode("utf-8", "replace"))
        sys.stdout.flush()
EOF
    ;;
  test)    pio test -e native "$@" ;;
  *) echo "usage: $0 {build|upload|monitor|capture <secs>|test|port|clean|size}" >&2; exit 1 ;;
esac
