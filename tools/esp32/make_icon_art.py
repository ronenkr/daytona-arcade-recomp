#!/usr/bin/env python3
"""Original 192x192 menu art for the EspRacer launcher card (no Sega artwork): a chequered
flag over a road in perspective with a speed stripe. Feed the PNG to EspRacer's tools/mkicon.py.

    make_icon_art.py out.png
"""
import sys
from PIL import Image, ImageDraw

N = 192
img = Image.new("RGB", (N, N), (22, 60, 140))
d = ImageDraw.Draw(img)

# sky gradient
for y in range(0, 96):
    t = y / 96
    d.line([(0, y), (N, y)], fill=(int(30 + 90 * t), int(90 + 80 * t), int(190 + 40 * t)))
# road in perspective
d.polygon([(0, N), (N, N), (N // 2 + 14, 96), (N // 2 - 14, 96)], fill=(48, 50, 58))
d.polygon([(0, N), (24, N), (N // 2 - 14, 96), (N // 2 - 16, 96)], fill=(230, 40, 40))
d.polygon([(N - 24, N), (N, N), (N // 2 + 16, 96), (N // 2 + 14, 96)], fill=(240, 240, 245))
for i in range(6):
    y0 = 100 + i * i * 3 + i * 6
    y1 = y0 + 3 + i * 2
    w = 2 + i
    d.rectangle([N // 2 - w, y0, N // 2 + w, y1], fill=(255, 205, 60))
# chequered flag
fx, fy, cs = 28, 20, 14
for r in range(4):
    for c in range(6):
        if (r + c) & 1 == 0:
            d.rectangle([fx + c * cs, fy + r * cs + c, fx + (c + 1) * cs - 1, fy + (r + 1) * cs - 1 + c], fill=(16, 16, 20))
        else:
            d.rectangle([fx + c * cs, fy + r * cs + c, fx + (c + 1) * cs - 1, fy + (r + 1) * cs - 1 + c], fill=(245, 245, 248))
d.rectangle([fx - 5, fy - 4, fx - 2, 120], fill=(200, 200, 205))
# speed stripes
for i, y in enumerate((70, 80, 90)):
    d.rectangle([110 + i * 6, y, 176, y + 3], fill=(255, 205, 60))
img.save(sys.argv[1])
