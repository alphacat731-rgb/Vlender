#!/usr/bin/env python3
"""Generate Vlender's default Vita LiveArea assets using only Python's standard library."""

import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "branding")

COLORS = {
    "bg": (20, 23, 29),
    "panel": (28, 32, 40),
    "panel2": (37, 42, 51),
    "edge": (60, 68, 79),
    "text": (230, 235, 240),
    "muted": (126, 139, 153),
    "orange": (255, 132, 48),
    "orange2": (232, 91, 28),
    "lime": (133, 255, 111),
    "blue": (76, 176, 244),
    "red": (247, 83, 91),
    "green": (78, 215, 137),
}

FONT = {
    "A": ("01110","10001","10001","11111","10001","10001","10001"),
    "B": ("11110","10001","10001","11110","10001","10001","11110"),
    "C": ("01111","10000","10000","10000","10000","10000","01111"),
    "D": ("11110","10001","10001","10001","10001","10001","11110"),
    "E": ("11111","10000","10000","11110","10000","10000","11111"),
    "F": ("11111","10000","10000","11110","10000","10000","10000"),
    "G": ("01111","10000","10000","10111","10001","10001","01111"),
    "H": ("10001","10001","10001","11111","10001","10001","10001"),
    "I": ("11111","00100","00100","00100","00100","00100","11111"),
    "J": ("00111","00010","00010","00010","10010","10010","01100"),
    "K": ("10001","10010","10100","11000","10100","10010","10001"),
    "L": ("10000","10000","10000","10000","10000","10000","11111"),
    "M": ("10001","11011","10101","10101","10001","10001","10001"),
    "N": ("10001","11001","10101","10011","10001","10001","10001"),
    "O": ("01110","10001","10001","10001","10001","10001","01110"),
    "P": ("11110","10001","10001","11110","10000","10000","10000"),
    "Q": ("01110","10001","10001","10001","10101","10010","01101"),
    "R": ("11110","10001","10001","11110","10100","10010","10001"),
    "S": ("01111","10000","10000","01110","00001","00001","11110"),
    "T": ("11111","00100","00100","00100","00100","00100","00100"),
    "U": ("10001","10001","10001","10001","10001","10001","01110"),
    "V": ("10001","10001","10001","10001","10001","01010","00100"),
    "W": ("10001","10001","10001","10101","10101","10101","01010"),
    "X": ("10001","10001","01010","00100","01010","10001","10001"),
    "Y": ("10001","10001","01010","00100","00100","00100","00100"),
    "Z": ("11111","00001","00010","00100","01000","10000","11111"),
    "0": ("01110","10001","10011","10101","11001","10001","01110"),
    "1": ("00100","01100","00100","00100","00100","00100","01110"),
    "2": ("01110","10001","00001","00010","00100","01000","11111"),
    "3": ("11110","00001","00001","00110","00001","00001","11110"),
    "4": ("00010","00110","01010","10010","11111","00010","00010"),
    "5": ("11111","10000","10000","11110","00001","00001","11110"),
    "6": ("01110","10000","10000","11110","10001","10001","01110"),
    "7": ("11111","00001","00010","00100","01000","01000","01000"),
    "8": ("01110","10001","10001","01110","10001","10001","01110"),
    "9": ("01110","10001","10001","01111","00001","00001","01110"),
    ".": ("00000","00000","00000","00000","00000","00110","00110"),
    ":": ("00000","00110","00110","00000","00110","00110","00000"),
    "-": ("00000","00000","00000","11111","00000","00000","00000"),
    "/": ("00001","00010","00010","00100","01000","01000","10000"),
    "+": ("00000","00100","00100","11111","00100","00100","00000"),
    "_": ("00000","00000","00000","00000","00000","00000","11111"),
    " ": ("00000",) * 7,
}

class Image:
    def __init__(self, width, height, color):
        self.width = width
        self.height = height
        self.pixels = bytearray(color * (width * height))

    def pixel(self, x, y, color):
        if 0 <= x < self.width and 0 <= y < self.height:
            p = (y * self.width + x) * 3
            self.pixels[p:p+3] = bytes(color)

    def rect(self, x, y, w, h, color):
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.width, x + w), min(self.height, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(color) * (x1 - x0)
        for yy in range(y0, y1):
            p = (yy * self.width + x0) * 3
            self.pixels[p:p+len(row)] = row

    def line(self, x0, y0, x1, y1, color, width=1):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        radius = max(0, width // 2)
        while True:
            if width <= 1:
                self.pixel(x0, y0, color)
            else:
                self.rect(x0 - radius, y0 - radius, width, width, color)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def circle(self, cx, cy, r, color, fill=False, width=1):
        if fill:
            for y in range(-r, r + 1):
                remaining = r * r - y * y
                if remaining >= 0:
                    x = int(remaining ** 0.5)
                    self.line(cx-x, cy+y, cx+x, cy+y, color)
            return
        x, y, err = r, 0, 1-r
        while x >= y:
            for px, py in ((x,y),(y,x),(-y,x),(-x,y),(-x,-y),(-y,-x),(y,-x),(x,-y)):
                for dx in range(-(width//2), width//2+1):
                    for dy in range(-(width//2), width//2+1):
                        self.pixel(cx+px+dx, cy+py+dy, color)
            y += 1
            if err < 0:
                err += 2*y+1
            else:
                x -= 1
                err += 2*(y-x)+1

    def text(self, x, y, text, color, scale=1):
        cursor = x
        for char in text.upper():
            rows = FONT.get(char, FONT[" "])
            for row, pattern in enumerate(rows):
                for col, value in enumerate(pattern):
                    if value == "1":
                        self.rect(cursor+col*scale, y+row*scale, scale, scale, color)
            cursor += 6 * scale
        return cursor - x

    def save_png(self, path):
        def chunk(kind, data):
            return (struct.pack(">I", len(data)) + kind + data +
                    struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
        raw = bytearray()
        stride = self.width * 3
        for y in range(self.height):
            raw.append(0)
            raw.extend(self.pixels[y*stride:(y+1)*stride])
        data = b"\x89PNG\r\n\x1a\n"
        data += chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0))
        data += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        data += chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(data)

def draw_logo(im, cx, cy, scale):
    orange = COLORS["orange"]
    white = (250, 249, 246)
    dark = COLORS["bg"]
    lime = COLORS["lime"]
    im.circle(cx, cy, scale, orange, fill=True)
    im.circle(cx, cy, scale-3, COLORS["orange2"], fill=False, width=max(1, scale//12))
    # Geometric three-face cube mark.
    top = (cx, cy-scale//2)
    left = (cx-scale//2, cy-scale//5)
    right = (cx+scale//2, cy-scale//5)
    bottom = (cx, cy+scale//5)
    bottom_left = (cx-scale//2, cy+scale//2)
    bottom_right = (cx+scale//2, cy+scale//2)
    im.line(*top, *left, white, width=max(2,scale//10))
    im.line(*top, *right, white, width=max(2,scale//10))
    im.line(*left, *bottom, white, width=max(2,scale//10))
    im.line(*right, *bottom, white, width=max(2,scale//10))
    im.line(*left, *bottom_left, white, width=max(2,scale//10))
    im.line(*bottom_left, cx, cy+scale//2, white, width=max(2,scale//10))
    im.line(*bottom, cx, cy+scale//2, white, width=max(2,scale//10))
    im.line(*bottom, *bottom_right, white, width=max(2,scale//10))
    im.line(*right, *bottom_right, white, width=max(2,scale//10))
    im.line(cx-scale//2, cy+scale//2, cx, cy+scale//2, white, width=max(2,scale//10))
    im.line(cx, cy+scale//2, cx+scale//2, cy+scale//2, white, width=max(2,scale//10))
    im.circle(cx+scale*2//3, cy-scale*2//3, max(2,scale//7), lime, fill=True)

def render_icon():
    im = Image(128, 128, COLORS["bg"])
    im.circle(64, 64, 61, COLORS["panel"], fill=True)
    im.circle(64, 64, 58, COLORS["edge"], fill=False, width=2)
    draw_logo(im, 64, 58, 42)
    im.rect(24, 106, 80, 3, COLORS["lime"])
    return im

def render_banner():
    w, h = 840, 500
    im = Image(w, h, COLORS["bg"])
    # Subtle editor grid background.
    im.rect(0, 0, w, h, COLORS["bg"])
    for x in range(26, w, 28):
        im.line(x, 54, x, h-36, (29, 34, 42))
    for y in range(54, h-36, 28):
        im.line(24, y, w-24, y, (29, 34, 42))
    # Application chrome.
    im.rect(0, 0, w, 42, (25, 29, 36))
    im.rect(0, 41, w, 2, COLORS["edge"])
    draw_logo(im, 31, 21, 15)
    im.text(58, 16, "VLENDER", COLORS["text"], 2)
    im.text(196, 18, "3D RIG STUDIO", COLORS["muted"], 1)
    im.rect(26, 60, 520, 352, (24, 28, 35))
    im.rect(558, 60, 256, 160, (27, 31, 39))
    im.rect(558, 232, 256, 180, (27, 31, 39))
    # Perspective grid and colored axes.
    vanishing = (285, 212)
    for x in range(34, 544, 38):
        im.line(vanishing[0], vanishing[1], x, 402, (57, 66, 77))
    for y in (238, 257, 281, 311, 351, 402):
        im.line(30, y, 542, y, (48, 57, 68))
    im.line(38, 340, 535, 340, COLORS["red"], 2)
    im.line(286, 223, 286, 399, COLORS["green"], 2)
    im.line(286, 223, 174, 328, COLORS["blue"], 2)
    # Wireframe torso/limbs.
    bone_points = {
        "head": (286, 117), "neck": (286, 151), "chest": (286, 198),
        "hip": (286, 247), "shoulderL": (240, 170), "elbowL": (204, 218),
        "handL": (180, 253), "shoulderR": (332, 170), "elbowR": (366, 218),
        "handR": (390, 253), "kneeL": (263, 318), "footL": (246, 372),
        "kneeR": (309, 318), "footR": (326, 372)
    }
    links = [("head","neck"),("neck","chest"),("chest","hip"),
             ("chest","shoulderL"),("shoulderL","elbowL"),("elbowL","handL"),
             ("chest","shoulderR"),("shoulderR","elbowR"),("elbowR","handR"),
             ("hip","kneeL"),("kneeL","footL"),("hip","kneeR"),("kneeR","footR")]
    for a,b in links:
        p, q = bone_points[a], bone_points[b]
        color = COLORS["orange"] if a in ("chest","elbowR","kneeR") else COLORS["blue"]
        im.line(p[0],p[1],q[0],q[1],color,3)
    for name, (x,y) in bone_points.items():
        c = COLORS["orange"] if name == "elbowR" else COLORS["text"]
        im.circle(x,y,4,c,fill=True)
    im.circle(286, 117, 20, COLORS["text"], fill=False, width=2)
    im.line(270, 117, 302, 117, COLORS["edge"])
    # Right-side mock outliner and transform values.
    im.text(575, 76, "OUTLINER", COLORS["text"], 2)
    for i, line in enumerate(("SCENE COLLECTION", "CAMERA", "LIGHT", "ARMATURE", "  SPINE", "  HEAD", "  UPPERARM.L")):
        yy = 104 + i * 15
        if i == 6:
            im.rect(568, yy-3, 236, 13, (32, 57, 43))
        im.text(574, yy, line, COLORS["lime"] if i == 6 else COLORS["muted"], 1)
    im.text(575, 250, "TRANSFORM", COLORS["text"], 2)
    im.text(575, 280, "LOCATION", COLORS["muted"], 1)
    im.text(575, 304, "ROTATION", COLORS["muted"], 1)
    im.text(575, 328, "SCALE", COLORS["muted"], 1)
    for i, col in enumerate((COLORS["red"],COLORS["green"],COLORS["blue"])):
        im.circle(585+i*65, 356, 5, col, fill=True)
    im.text(575, 384, "POSE MODE", COLORS["lime"], 1)
    # Footer.
    im.rect(0, 444, w, 56, (15, 18, 23))
    im.rect(0, 444, w, 2, COLORS["edge"])
    im.text(28, 462, "PORTABLE 3D RIGGING WORKSPACE", COLORS["text"], 2)
    im.text(28, 484, "BLENDER PROJECT BRIDGE / TOUCH + CONTROLLER", COLORS["muted"], 1)
    return im

def render_startup():
    im = Image(280, 158, COLORS["bg"])
    draw_logo(im, 140, 62, 34)
    im.text(77, 111, "VLENDER", COLORS["text"], 2)
    im.text(83, 131, "3D RIG STUDIO", COLORS["muted"], 1)
    im.rect(92, 147, 96, 3, COLORS["lime"])
    return im

def main():
    livearea = os.path.join(OUT, "livearea")
    os.makedirs(livearea, exist_ok=True)
    render_icon().save_png(os.path.join(OUT, "icon0.png"))
    render_banner().save_png(os.path.join(livearea, "bg.png"))
    render_startup().save_png(os.path.join(livearea, "startup.png"))
    template = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""
    with open(os.path.join(livearea, "template.xml"), "w", encoding="utf-8") as stream:
        stream.write(template)
    print("Generated default Vlender icon and LiveArea artwork in assets/branding/")

if __name__ == "__main__":
    main()
