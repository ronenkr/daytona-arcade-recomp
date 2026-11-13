#!/usr/bin/env python3
"""Log the console's USB serial for SECONDS and take a screenshot every EVERY seconds (main/console_esp.c 'S'),
optionally sending keys at given times.   console_watch.py PORT SECONDS EVERY OUTDIR [t:key ...]
Writes OUTDIR/log.txt and OUTDIR/shot-<t>.png."""
import os, struct, sys, time, zlib, base64, serial

def png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

port, secs, every, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
keys = sorted((float(a.split(":")[0]), a.split(":")[1]) for a in sys.argv[5:])
os.makedirs(out, exist_ok=True)
s = serial.Serial(port, 115200, timeout=0.05)
log = open(os.path.join(out, "log.txt"), "wb")
start = time.time(); next_shot = every; buf = b""
while time.time() - start < secs:
    t = time.time() - start
    while keys and keys[0][0] <= t:
        s.write(keys.pop(0)[1].encode()); s.flush()
    if every and t >= next_shot:
        next_shot += every
        s.write(b"S"); s.flush()
        data = b""; end = time.time() + 6
        while time.time() < end and b"END\n" not in data: data += s.read(65536)
        head = data.find(b"SHOT ")
        if head >= 0:
            log.write(data[:head])
            lines = data[head:].split(b"\n")
            w, h = map(int, lines[0].split()[1:3]); rows = []
            for l in lines[1:]:
                if l == b"END": break
                try: r = base64.b64decode(l, validate=True)
                except Exception: continue
                if len(r) == 2 * w: rows.append(r)
            if len(rows) == h:
                rgb = bytearray()
                for r in rows:
                    for (v,) in struct.iter_unpack("<H", r):
                        rgb += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
                png(os.path.join(out, "shot-%03d.png" % int(t)), w, h, bytes(rgb))
            log.write(data[data.find(b"END\n") + 4:] if b"END\n" in data else b"")
        else: log.write(data)
        continue
    log.write(s.read(4096))
log.close()
