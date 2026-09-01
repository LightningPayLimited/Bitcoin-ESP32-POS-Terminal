#!/usr/bin/env python3
"""Regenerate logo.h from a brand-logo SVG.

Produces two assets in one header:
  - LOGO_RGB565: display logo composited on black for Arduino_GFX
  - LOGO_MONO:   384-dot-wide 1-bpp logo for the thermal printer,
                 pre-squashed vertically to compensate for the printer's
                 vertical over-step (same ~0.43 factor as the original).

Usage: gen_branding.py <logo.svg> <logo.h>
Needs: pip install pillow cairosvg
"""
import sys
from PIL import Image
import cairosvg
import io

SRC, DST = sys.argv[1], sys.argv[2]

DISPLAY_H = 80          # fits above "POS SETUP" on the setup screen
PRINT_W = 384           # full thermal paper width in dots
PRINT_SQUASH = 0.434    # vertical pre-squash for printer over-step

png = cairosvg.svg2png(url=SRC, output_width=2400)
img = Image.open(io.BytesIO(png)).convert("RGBA")
img = img.crop(img.getbbox())  # trim the SVG canvas whitespace
aspect = img.width / img.height

# --- display logo: composite on black, RGB565 ---
disp_w = round(DISPLAY_H * aspect)
disp = Image.new("RGBA", (disp_w, DISPLAY_H), (0, 0, 0, 255))
disp.alpha_composite(img.resize((disp_w, DISPLAY_H), Image.LANCZOS))
disp = disp.convert("RGB")

# --- printer logo: 1-bpp, MSB-first, set bit = black ink ---
print_h = round(PRINT_W / aspect * PRINT_SQUASH)
mono = img.resize((PRINT_W, print_h), Image.LANCZOS)
alpha = mono.getchannel("A")

with open(DST, "w") as f:
    f.write(f"// Auto-generated from Stacked brand assets ({SRC.split('/')[-1]}) —\n")
    f.write("// do not edit by hand. Regenerate with gen_branding.py.\n")
    f.write("#pragma once\n#include <Arduino.h>\n\n")

    f.write(f"#define LOGO_W {disp.width}\n#define LOGO_H {disp.height}\n\n")
    f.write(f"// {disp.width}x{disp.height} RGB565 logo on black. "
            "For Arduino_GFX::draw16bitRGBBitmap().\n")
    f.write("static const uint16_t LOGO_RGB565[LOGO_W * LOGO_H] PROGMEM = {\n")
    px = disp.load()
    vals = []
    for y in range(disp.height):
        for x in range(disp.width):
            r, g, b = px[x, y]
            vals.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    for i in range(0, len(vals), 12):
        f.write("    " + ", ".join(f"0x{v:04X}" for v in vals[i:i+12]) + ",\n")
    f.write("};\n\n")

    bpr = PRINT_W // 8
    f.write(f"// {PRINT_W}x{print_h} monochrome logo, packed MSB-first (set bit = black ink).\n")
    f.write("// Pre-squashed vertically to compensate for the printer's vertical over-step.\n")
    f.write(f"#define LOGO_PRINT_W   {PRINT_W}\n")
    f.write(f"#define LOGO_PRINT_H   {print_h}\n")
    f.write(f"#define LOGO_PRINT_BYTES_PER_ROW ({PRINT_W} / 8)\n")
    f.write("static const uint8_t LOGO_MONO[LOGO_PRINT_BYTES_PER_ROW * LOGO_PRINT_H] PROGMEM = {\n")
    ap = alpha.load()
    for y in range(print_h):
        row = bytearray(bpr)
        for x in range(PRINT_W):
            if ap[x, y] >= 128:
                row[x >> 3] |= 0x80 >> (x & 7)
        f.write("    " + ", ".join(f"0x{b:02X}" for b in row) + ",\n")
    f.write("};\n")

print(f"wrote {DST}: display {disp.width}x{disp.height}, print {PRINT_W}x{print_h}")
