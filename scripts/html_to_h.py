#!/usr/bin/env python3
"""Wrap an HTML file in a C++ raw-string-literal header.

usage: html_to_h.py <in.html> <out.h>

The output defines `HUB_HTML[]` as a `const char` PROGMEM array suitable for
serving directly from AsyncWebServer. Output goes to a `.h` file so the
Arduino .ino preprocessor never touches it (the .ino-to-cpp transformation
chokes on `function name(){}` patterns inside string literals when they are
embedded in .ino files; .h files use the regular C++ preprocessor and are
left alone).
"""
import sys

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(1)

with open(sys.argv[1], "r", encoding="utf-8") as f:
    html = f.read()

# Pick a raw-string delimiter that doesn't appear in the HTML.
delim = "TINYREADER"
while ")" + delim + '"' in html:
    delim += "X"

with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write(f"// Generated from {sys.argv[1]}; edit the .html, not this file.\n")
    f.write("#pragma once\n\n")
    f.write(f"static const char HUB_HTML[] PROGMEM = R\"{delim}(")
    f.write(html)
    f.write(f"){delim}\";\n")

print(f"wrote {sys.argv[2]}: {len(html)} chars")
