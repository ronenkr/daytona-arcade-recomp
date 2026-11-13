#!/usr/bin/env python3
"""Expands C23 #embed lines into byte lists, for compilers without #embed (the host's gcc 13).
   expand_embed.py IN OUT"""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
out = []
for line in open(src):
    m = re.match(r'\s*#embed\s+"([^"]+)"', line)
    if not m:
        out.append(line)
        continue
    data = open(m.group(1), "rb").read()
    for i in range(0, len(data), 32):
        out.append(",".join(str(b) for b in data[i:i + 32]) + ",\n")
open(dst, "w").write("".join(out))
