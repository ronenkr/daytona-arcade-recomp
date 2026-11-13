#!/bin/bash
# Build the render mode, flash slot game3, start it from the menu and print the perf lines of SECONDS seconds.
#   console_render.sh [SECONDS] [LOG]
set -e
# the console by its USB id (another USB serial device may hold ttyACM0)
PORT="${PORT:-$(ls /dev/serial/by-id/*Espressif*USB_JTAG* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no console on USB"; exit 1; }
secs=${1:-60}; log=${2:-/tmp/daytona_render.log}
here="$(cd "$(dirname "$0")/.." && pwd)"
ESPRACER="${ESPRACER_DIR:-$here/../../../EspRacer}"
( . ~/esp/v6.0/esp-idf/export.sh >/dev/null 2>&1
  cd "$here"
  export DAYTONA_MODE=${DAYTONA_MODE:-render} DAYTONA_SCRIPT_DIR="${DAYTONA_SCRIPT_DIR:-$here/../../build/esp-assets/m2/script-fast}"
  idf.py -B build-racer reconfigure >/dev/null 2>&1
  idf.py -B build-racer build > build-racer/last_build.log 2>&1; rc=$?
  grep -E "error:|binary size|FAILED" build-racer/last_build.log
  exit $rc ) || { echo "build failed: not flashed"; exit 1; }
cd "$ESPRACER"
python3 -I -c "
import serial, time
s = serial.Serial('$PORT', 115200); s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False"
tools/flash_all.sh $PORT daytona 2>&1 | tail -1
sleep 7
for i in 1 2 3; do python3 tools/racerctl.py --port $PORT key n; sleep 1.5; done
python3 tools/racerctl.py --port $PORT key p
SECS=$secs python3 -I -c "
import os, serial, time
s = serial.Serial('$PORT', 115200, timeout=0.5)
end = time.time() + int(os.environ['SECS']); buf = b''
while time.time() < end:
    buf += s.read(65536)
    if b'Guru' in buf or b'abort()' in buf: time.sleep(1); buf += s.read(65536); break
open('$log', 'wb').write(buf)"
grep -aE "app_init: Project|sw3d|perf|FAIL|Guru|abort" "$log" | head -60
