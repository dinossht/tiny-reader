#!/usr/bin/env python3
"""Generate a 1-bit QR-code C header for the tiny-reader splash. Usage:

    make_qr.py <text> <out.h> [<symbol_name>]

Produces a `<symbol>_W`/`_H` macro and `<symbol>_BITMAP[]` byte array.
"""
import sys
import qrcode

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(1)

text = sys.argv[1]
out  = sys.argv[2]
sym  = sys.argv[3] if len(sys.argv) > 3 else "QR"

qr = qrcode.QRCode(
    version=None,
    error_correction=qrcode.constants.ERROR_CORRECT_L,
    box_size=4,
    border=2,
)
qr.add_data(text)
qr.make(fit=True)
img = qr.make_image(fill_color="black", back_color="white").convert("1")
w, h = img.size
row_bytes = (w + 7) // 8
data = bytearray(row_bytes * h)
px = img.load()
for y in range(h):
    for x in range(w):
        if px[x, y] == 0:  # black module
            data[y * row_bytes + (x >> 3)] |= 0x80 >> (x & 7)

with open(out, "w", encoding="utf-8") as f:
    f.write(f"// Generated from text: {text!r} ({w}x{h})\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"#define {sym}_W {w}\n#define {sym}_H {h}\n")
    f.write(f"const uint8_t {sym}_BITMAP[] = {{\n")
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        f.write("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write("};\n")

print(f"wrote {out}: {w}x{h}, {len(data)} bytes")
