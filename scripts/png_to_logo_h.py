#!/usr/bin/env python3
"""Convert a PNG (any mode, any size) to a 1-bit packed C header for the
tiny-reader splash logo.

usage: png_to_logo_h.py <in.png> <out.h> <max_w> <max_h>

Output is `LOGO_W`, `LOGO_H` macros + `LOGO_BITMAP[]` byte array. Each row is
padded to a byte boundary; bits are MSB-first within each byte. Bit=1 means
black, bit=0 means white.
"""
import sys
from PIL import Image

if len(sys.argv) != 5:
    print(__doc__)
    sys.exit(1)

src, out, max_w, max_h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

im = Image.open(src).convert("RGBA")
# composite onto white so transparency reads as background
bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
im = Image.alpha_composite(bg, im).convert("L")
im.thumbnail((max_w, max_h), Image.LANCZOS)
im = im.point(lambda v: 0 if v < 128 else 255, mode="1")

w, h = im.size
row_bytes = (w + 7) // 8
data = bytearray(row_bytes * h)
px = im.load()
for y in range(h):
    for x in range(w):
        if px[x, y] == 0:  # black
            data[y * row_bytes + (x >> 3)] |= 0x80 >> (x & 7)

with open(out, "w") as f:
    f.write(f"// Generated from {src} ({w}x{h}, 1-bit packed)\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"#define LOGO_W {w}\n#define LOGO_H {h}\n")
    f.write("const uint8_t LOGO_BITMAP[] = {\n")
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        f.write("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write("};\n")

print(f"wrote {out}: {w}x{h}, {len(data)} bytes")
