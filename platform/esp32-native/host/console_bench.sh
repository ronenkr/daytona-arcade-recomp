#!/bin/bash
# Build the bench mode, flash slot game3, start it from the menu and print the timing table.
#   console_bench.sh [LOG]
set -e
# the console by its USB id (another USB serial device may hold ttyACM0)
PORT="${PORT:-$(ls /dev/serial/by-id/*Espressif*USB_JTAG* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no console on USB"; exit 1; }
log=${1:-/tmp/daytona_bench.log}
here="$(cd "$(dirname "$0")/.." && pwd)"
ESPRACER="${ESPRACER_DIR:-$here/../../../EspRacer}"
( . ~/esp/v6.0/esp-idf/export.sh >/dev/null 2>&1
  cd "$here"
  export DAYTONA_MODE=bench DAYTONA_SCRIPT_DIR="${DAYTONA_SCRIPT_DIR:-$here/../../build/esp-assets/m2/script-fast}"
  idf.py -B build-racer reconfigure >/dev/null 2>&1
  idf.py -B build-racer build 2>&1 | grep -E "error:|binary size|FAILED" || true )
cd "$ESPRACER"
python3 -I -c "
import serial, time
s = serial.Serial('$PORT', 115200); s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False"
tools/flash_all.sh $PORT daytona 2>&1 | tail -1
sleep 7
for i in 1 2 3; do python3 tools/racerctl.py --port $PORT key n; sleep 1.5; done
python3 tools/racerctl.py --port $PORT key p
python3 -I -c "
import serial, time
s = serial.Serial('$PORT', 115200, timeout=0.5)
end = time.time() + 90; buf = b''
while time.time() < end:
    buf += s.read(65536)
    if b'bench: done' in buf or b'Guru' in buf or b'abort()' in buf: time.sleep(0.5); buf += s.read(65536); break
open('$log', 'wb').write(buf)"
grep -aE "app_init: Project|bench|sw3d|Guru|abort" "$log"
