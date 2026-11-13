#!/usr/bin/env python3
"""The ten continuous engine layers (six rpm layers, the nearest opponent left and right, slip, skid) for the
EspRacer port: each layer's looping sample and, per row of its controller, the playback rate and gain.

    python3 -I tools/esp32/build_engine_pack.py RENDER_DIR SOUND_DIR OUT.pak

RENDER_DIR  prepare_sfx render's output (vgm/e0..e9.vgm: each layer's sample keyed on, with its sample ROM)
SOUND_DIR   extract_sound_roms.py's output (sound_program.bin: the tables of update_engines)
The layer table is prepare_sfx.cpp's kLayers (update_engines of src/runtime/native_sound_sequencer.cpp).
Format "ENG1", u32 layers, then per layer: u32 samples, u32 loop start, u16 rows, u8 row controller,
u8 level controller (0x34/0x35 for the opponent layers, else 0), float rate_hz[rows], u8 gain[rows]
(0..255, linear), padding to 4, int8 samples. ROM-derived: keep it under build/.
"""
import gzip, math, struct, sys

render, sound, out = sys.argv[1:4]
program = open(f"{sound}/sound_program.bin", "rb").read()
LAYERS = [  # slot, index control, pitch table, level table, pan, rows (prepare_sfx.cpp kLayers)
    (25, 0x17, 0x18d4, 0x1854, 0, 128), (24, 0x17, 0x18d4, 0x1a86, 0, 128), (23, 0x17, 0x1d38, 0x1cb8, 0, 128),
    (27, 0x17, 0x1f6c, 0x1eec, 0, 128), (26, 0x17, 0x2120, 0x1eec, 0, 128), (22, 0x17, 0x2344, 0x22c4, 0, 128),
    (20, 0x17, 0x18d4, 0, 7, 128), (19, 0x17, 0x18d4, 0, 9, 128), (21, 0x19, 0x2528, 0x2518, 0, 16),
    (18, 0x37, 0x27ce, 0x27be, 0, 16)]
CHIP_HZ = 10_000_000 / 224   # the Model 1 sound board's MultiPCM clock, as the music renderer uses

def word(at):
    return program[at] << 8 | program[at + 1]

def rate_hz(code):
    r2, r3 = code & 255, code >> 8
    octave = ((r3 >> 4) - 1) & 15
    if octave & 8:
        octave -= 16
    fns = (r3 & 15) * 64 + (r2 >> 2)
    return math.ldexp((1024 + fns) / 1024, octave) * CHIP_HZ

def vgm_sample(path):
    d = open(path, "rb").read()
    if d[:2] == b"\x1f\x8b":
        d = gzip.decompress(d)
    rom = bytearray(0x400000)
    i = 0x34 + struct.unpack_from("<I", d, 0x34)[0]
    regs, slot, reg = {}, 0, 0
    while i < len(d) and d[i] != 0x66:
        c = d[i]
        if c == 0x67:
            size = struct.unpack_from("<I", d, i + 3)[0] & 0x7fffffff
            start = struct.unpack_from("<I", d, i + 11)[0]
            rom[start:start + size - 8] = d[i + 15:i + 7 + size]
            i += 7 + size
            continue
        if c == 0xb5:
            port, val = d[i + 1], d[i + 2]
            if port == 1: slot = val
            elif port == 2: reg = val
            elif port == 0: regs[reg] = val
            i += 3
        elif c == 0x61: i += 3
        elif c in (0x62, 0x63) or 0x70 <= c < 0x80: i += 1
        else: raise ValueError(f"{path}: VGM command {c:02x}")
    index = regs.get(1, 0) | (regs.get(2, 0) & 1) << 8
    h = rom[index * 12:index * 12 + 12]
    start = h[0] << 16 | h[1] << 8 | h[2]
    fmt = (start >> 20) & 0xfe
    start &= 0x3fffff
    loop = h[3] << 8 | h[4]
    end = 0x10000 - (h[5] << 8 | h[6])
    if fmt & 4:  # 12-bit: keep the top 8 bits
        pcm = bytearray()
        for s in range(end):
            a = start + (s >> 1) * 3
            pcm.append(rom[a] if not s & 1 else rom[a + 2])
    else:
        pcm = rom[start:start + end]
    return bytes(pcm), loop

blob = b"ENG1" + struct.pack("<I", len(LAYERS))
for l, (slot, control, pitch_table, level_table, pan, rows) in enumerate(LAYERS):
    pcm, loop = vgm_sample(f"{render}/vgm/e{l}.vgm")
    hz, gain = [], []
    for row in range(rows):
        hz.append(rate_hz(word(pitch_table + 2 * row)))
        level = ((row or 255) >> 1) if l in (6, 7) else program[level_table + row] >> 1
        gain.append(max(0, min(255, round(255 * 10 ** (-(level & 127) * 0.375 / 20)))))
    level_control = 0x34 if l == 6 else 0x35 if l == 7 else 0
    rec = struct.pack("<IIHBB", len(pcm), loop, rows, control, level_control)
    rec += struct.pack(f"<{rows}f", *hz) + bytes(gain)
    rec += b"\0" * (-len(rec) % 4) + pcm + b"\0" * (-len(pcm) % 4)
    blob += rec
    print(f"layer {l}: {len(pcm)} samples, loop at {loop}, {rows} rows, {hz[0]:.0f}..{hz[-1]:.0f} Hz")
open(out, "wb").write(blob)
print(f"{out}: {len(blob)} bytes")
