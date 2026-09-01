#!/usr/bin/env python3
"""Regenerate splash_image.h from a 480x800 PNG (RGB565 for Arduino_GFX)."""
import sys
from PIL import Image

src = sys.argv[1]
dst = sys.argv[2]

img = Image.open(src).convert("RGB")
if img.size != (480, 800):
    print(f"resizing {img.size} -> (480, 800)")
    img = img.resize((480, 800), Image.LANCZOS)

w, h = img.size
px = img.load()

vals = []
for y in range(h):
    for x in range(w):
        r, g, b = px[x, y]
        vals.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

with open(dst, "w") as f:
    f.write(f"// Auto-generated from Stacked splash ({src.split('/')[-1]})\n")
    f.write("// — do not edit by hand. Regenerate with gen_splash.py.\n")
    f.write("#pragma once\n#include <Arduino.h>\n\n")
    f.write(f"#define SPLASH_W {w}\n#define SPLASH_H {h}\n\n")
    f.write(f"// {w}x{h} RGB565. For Arduino_GFX::draw16bitRGBBitmap().\n")
    f.write("static const uint16_t SPLASH_RGB565[SPLASH_W * SPLASH_H] PROGMEM = {\n")
    for i in range(0, len(vals), 12):
        row = ", ".join(f"0x{v:04X}" for v in vals[i:i+12])
        f.write(f"    {row},\n")
    f.write("};\n")

print(f"wrote {dst}: {len(vals)} pixels")
