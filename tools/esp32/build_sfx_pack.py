#!/usr/bin/env python3
"""Packs the rendered sound effects for the EspRacer port.

    python3 -I tools/esp32/build_sfx_pack.py RENDER_DIR WAV_DIR OUTDIR

RENDER_DIR   prepare_sfx render's output (render.tsv, gains.bin); WAV_DIR the 22,050 Hz mono WAVs
             vgm_render made of its vgm/*.vgm (build_music.py's tool); OUTDIR/sfx/ gets
  race.pak    the effects a race plays, resident in PSRAM on the console (sfx-banks.txt: 'race' lines,
              without the engine layers)
  voices.pak  the announcer's phrases (sfx-banks.txt: 'voices'), read from the card when one is played
  gains.bin   the 128 velocity gains of channel 14's notes (prepare_sfx render)
Pak format: "SFX2", u32 count, u32 total sample count, then count entries of
  {u32 command (0xae2109, or 0x9e0100 for the note 9e01), u32 first sample, u32 samples},
  then the samples as signed 8 bit (the chip's own samples are 8 bit; the mixer shifts them left by 8). Entries are sorted by command. Output is ROM-derived: keep it under build/.
"""
import os, struct, sys, wave

render, wavs, out = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(here))
rows = [l.split("\t")[0] for l in open(os.path.join(render, "render.tsv")).read().splitlines()[1:]]

def expand(spec):
    if "-" in spec:
        first, last = spec.split("-")
        n = 0
        while n < min(len(first), len(last)) and first[n] == last[n]:
            n += 1
        prefix, lo, hi = first[:n], int(first[n:], 16), int(last[n:], 16)
        return [k for k in rows if k.startswith(prefix) and len(k) == len(first) and lo <= int(k[n:], 16) <= hi]
    return [k for k in rows if k == spec]

race, voices = [], []
for line in open(os.path.join(repo, "platform/dreamcast-native/recipes/sfx-banks.txt")):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    bank, _coding, *keys = line.split()
    dest = voices if bank == "voices" else race
    for spec in keys:
        dest += expand(spec)

def command(key):
    return int(key, 16) << 8 if len(key) == 4 else int(key, 16)

def pak(keys, name):
    keys = sorted(set(k for k in keys if not (k[0] == "e" and len(k) == 2)), key=command)
    data, entries, first = bytearray(), [], 0
    for k in keys:
        w = wave.open(os.path.join(wavs, k + ".wav"))
        raw = w.readframes(w.getnframes())
        n = len(raw) // 2
        s16 = struct.unpack("<%dh" % n, raw)
        data += bytes((max(-128, min(127, (v + 128) >> 8)) & 255) for v in s16)
        entries.append((command(k), first, n))
        first += n
    blob = b"SFX2" + struct.pack("<II", len(entries), first)
    for e in entries:
        blob += struct.pack("<III", *e)
    path = os.path.join(out, "sfx", name)
    with open(path, "wb") as f:
        f.write(blob + bytes(data))
    print(f"{name}: {len(entries)} sounds, {first / 1e6:.2f} MB")

os.makedirs(os.path.join(out, "sfx"), exist_ok=True)
pak(race, "race.pak")
pak(voices, "voices.pak")
open(os.path.join(out, "sfx", "gains.bin"), "wb").write(open(os.path.join(render, "gains.bin"), "rb").read())
