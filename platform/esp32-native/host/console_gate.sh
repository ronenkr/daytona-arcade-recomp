#!/bin/bash
# M2 gate on the console for one recorded race.
#   console_gate.sh COURSE SCRIPT_DIR EXPECTED_ROWS_FILE OUT_PREFIX
# Builds the headless race for COURSE with SCRIPT_DIR's race_script_data.h, flashes slot game3, starts it
# from the menu (BOOT three times, PWR), captures the serial log and diffs the CHECKPOINT rows with
# EXPECTED_ROWS_FILE. Needs EspRacer at ../../../EspRacer, the card with the course's packs, ESP-IDF 6.0.
set -e
# the console by its USB id (another USB serial device may hold ttyACM0)
PORT="${PORT:-$(ls /dev/serial/by-id/*Espressif*USB_JTAG* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no console on USB"; exit 1; }
course=$1; script=$2; expected=$3; out=$4
here="$(cd "$(dirname "$0")/.." && pwd)"
ESPRACER="${ESPRACER_DIR:-$here/../../../EspRacer}"
( . ~/esp/v6.0/esp-idf/export.sh >/dev/null 2>&1
  cd "$here"
  export DAYTONA_SCRIPT_DIR="$script" DAYTONA_COURSE="$course"
  idf.py -B build-racer reconfigure >/dev/null 2>&1
  idf.py -B build-racer build 2>&1 | grep -E "error|binary size|FAILED" || true )
cd "$ESPRACER"
tools/flash_all.sh $PORT daytona 2>&1 | tail -1
sleep 6
for i in 1 2 3; do python3 tools/racerctl.py --port $PORT key n; sleep 1; done
python3 tools/racerctl.py --port $PORT key p
python3 -I - > "$out.log" <<'PY'
import serial, time
s = serial.Serial('$PORT', 115200, timeout=0.5)
end = time.time() + 400
buf = b''
while time.time() < end:
    buf += s.read(65536)
    if b'race finished' in buf or b'abort' in buf or b'Guru' in buf:
        time.sleep(1); buf += s.read(65536); break
print(buf.decode(errors='replace'))
PY
grep -E "m2:|Guru|abort|Native practice end|Native checkpoints|FAIL" "$out.log" || true
grep "^CHECKPOINT" "$out.log" | tr -d '\r' > "$out.rows"
grep "^CHECKPOINT" "$expected" > "$out.expected"
echo "rows: $(wc -l < "$out.rows") of $(wc -l < "$out.expected")"
if diff "$out.rows" "$out.expected" > /dev/null; then echo "CONSOLE == EXPECTED: identical"; else echo DIFFERENT; diff "$out.rows" "$out.expected" | head -5; fi
