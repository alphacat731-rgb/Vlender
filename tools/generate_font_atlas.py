#!/usr/bin/env python3
"""Generate a compact antialiased Noto Sans raster atlas for the Vita UI."""
import base64
import textwrap
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans[wght].ttf",
)
font_path = next((p for p in FONT_CANDIDATES if Path(p).is_file()), None)
if not font_path:
    raise SystemExit("Noto Sans Regular was not found; install fonts-noto-core")
font = ImageFont.truetype(font_path, 12)

CELL_W, CELL_H, COLS, ROWS = 12, 16, 16, 6
ATLAS_W, ATLAS_H = CELL_W * COLS, CELL_H * ROWS
image = Image.new("L", (ATLAS_W, ATLAS_H), 0)
draw = ImageDraw.Draw(image)
metrics = []

for code in range(32, 128):
    index = code - 32
    ox = (index % COLS) * CELL_W
    oy = (index // COLS) * CELL_H
    char = chr(code)
    if code == 127:
        bbox, advance = (0, 4, 0, 4), 6.0
    else:
        bbox = draw.textbbox((0, 0), char, font=font)
        advance = font.getlength(char)
        left, top, right, _bottom = bbox
        draw.text((ox + 2 - left, oy), char, font=font, fill=255)
    left, top, right, bottom = bbox
    metrics.append((
        max(0, min(CELL_W - 3, right - left)),
        max(0, min(CELL_H - top, bottom - top)),
        max(-2, min(2, left)),
        max(-4, min(12, top - 4)),
        max(1, min(255, round(advance * 4))),
    ))

raw = image.tobytes()
rle = bytearray()
pos = 0
while pos < len(raw):
    value = raw[pos]
    run = 1
    while pos + run < len(raw) and raw[pos + run] == value and run < 255:
        run += 1
    rle.extend((run, value))
    pos += run
encoded = base64.b64encode(rle).decode("ascii")

header = '''/*
 * Rasterized Noto Sans Regular UI atlas, generated at 12 px.
 * Typeface: Noto Sans, SIL Open Font License 1.1.
 * The original font file is not included; this embeds only an ASCII glyph raster.
 * https://github.com/notofonts/latin-greek-cyrillic
 */
#ifndef VLENDER_FONT_ATLAS_H
#define VLENDER_FONT_ATLAS_H
#include <stdint.h>
#define VL_FONT_FIRST 32
#define VL_FONT_COUNT 96
#define VL_FONT_CELL_W 12
#define VL_FONT_CELL_H 16
#define VL_FONT_COLS 16
#define VL_FONT_ATLAS_W 192
#define VL_FONT_ATLAS_H 96
#define VL_FONT_RAW_SIZE (VL_FONT_ATLAS_W * VL_FONT_ATLAS_H)
static const char g_font_atlas_rle_b64[] =
'''
header += "".join('    "' + chunk + '"\n' for chunk in textwrap.wrap(encoded, 96))
header += ''';\ntypedef struct { uint8_t width; uint8_t height; int8_t left; int8_t top; uint8_t advance_x4; } VLFontGlyph;
static const VLFontGlyph g_font_glyphs[VL_FONT_COUNT] = {
'''
for start in range(0, len(metrics), 4):
    header += "    " + ", ".join("{" + ",".join(map(str, item)) + "}" for item in metrics[start:start + 4])
    header += ",\n" if start + 4 < len(metrics) else "\n"
header += "};\n#endif\n"

out = Path("src/font_atlas.h")
out.write_text(header, encoding="ascii")
print("Generated", out, "with", len(raw), "raster bytes and", len(rle), "RLE bytes")
