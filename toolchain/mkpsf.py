#!/usr/bin/env python3
# rozOS — TrueType থেকে PSF2 কনসোল ফন্ট বানানো (ধাপ ১৩)
#
#   python3 toolchain/mkpsf.py FONT.ttf WIDTH HEIGHT OUT.psf [PREVIEW.png]
#
# 256টা গ্লিফ, CP437 ক্রমে (কার্নেলের কনসোল বাইট = গ্লিফ নম্বর, VGA-র মতো — বক্স আঁকার
# অক্ষরও)। বক্স/ব্লক অক্ষর (░▒▓█ ─│┌ ...) ফন্ট থেকে নয়, ঘরের মাপে নিজে আঁকা, যাতে পাশাপাশি
# ঘর জোড়া লাগে। লাগে Pillow (pip install pillow) — শুধু নতুন করে বানাতে; তৈরি .psf
# ফাইলগুলো sysroot/usr/share/fonts-এ আছে।
import sys, struct
from PIL import Image, ImageDraw, ImageFont

CP437_LOW = "\u0000☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"

def cp437_char(i):
    if i < 32:
        return CP437_LOW[i]
    if i == 127:
        return "⌂"
    return bytes([i]).decode("cp437")

def fit_font(path, w, h):
    best = None
    for size in range(4, 200):
        f = ImageFont.truetype(path, size)
        asc, desc = f.getmetrics()
        adv = f.getlength("M")
        if asc + desc > h or adv > w + 0.01:
            break
        best = f
    return best

# বক্স-অক্ষর: (উপর, নিচ, বাম, ডান) রেখা: 0 নেই, 1 একক, 2 দ্বিগুণ
BOX = {
    0xB3: (1,1,0,0), 0xB4: (1,1,1,0), 0xB5: (1,1,2,0), 0xB6: (2,2,1,0), 0xB7: (0,2,1,0),
    0xB8: (0,1,2,0), 0xB9: (2,2,2,0), 0xBA: (2,2,0,0), 0xBB: (0,2,2,0), 0xBC: (2,0,2,0),
    0xBD: (2,0,1,0), 0xBE: (1,0,2,0), 0xBF: (0,1,1,0), 0xC0: (1,0,0,1), 0xC1: (1,0,1,1),
    0xC2: (0,1,1,1), 0xC3: (1,1,0,1), 0xC4: (0,0,1,1), 0xC5: (1,1,1,1), 0xC6: (1,1,0,2),
    0xC7: (2,2,0,1), 0xC8: (2,0,0,2), 0xC9: (0,2,0,2), 0xCA: (2,0,2,2), 0xCB: (0,2,2,2),
    0xCC: (2,2,0,2), 0xCD: (0,0,2,2), 0xCE: (2,2,2,2), 0xCF: (1,0,2,2), 0xD0: (2,0,1,1),
    0xD1: (0,1,2,2), 0xD2: (0,2,1,1), 0xD3: (2,0,0,1), 0xD4: (1,0,0,2), 0xD5: (0,1,0,2),
    0xD6: (0,2,0,1), 0xD7: (2,2,1,1), 0xD8: (1,1,2,2), 0xD9: (1,0,1,0), 0xDA: (0,1,0,1),
}

def draw_box(img, w, h, spec):
    up, down, left, right = spec
    px = img.load()
    cx, cy = w // 2, h // 2
    t = max(1, w // 8)
    def hline(y, x0, x1):
        for x in range(max(0, x0), min(w, x1)):
            for k in range(t):
                if 0 <= y + k < h: px[x, y + k] = 1
    def vline(x, y0, y1):
        for y in range(max(0, y0), min(h, y1)):
            for k in range(t):
                if 0 <= x + k < w: px[x + k, y] = 1
    g = max(2, t + 1)              # দ্বিগুণ রেখার ফাঁক
    def offsets(kind):
        return [0] if kind == 1 else [-g // 2 - t // 2, g - g // 2]
    # দ্বিগুণ লাইন কোণে ঠিক মিলুক — সরল নিয়ম: প্রতিটা দিক কেন্দ্র পর্যন্ত (অন্য রেখার পুরুত্বসহ)
    vk = max(up, down); hk = max(left, right)
    for o in offsets(up) if up else []:
        vline(cx + o, 0, cy + (t if not hk else (g if hk == 2 else t)))
    for o in offsets(down) if down else []:
        vline(cx + o, cy - (0 if not hk else (g // 2 if hk == 2 else 0)), h)
    for o in offsets(left) if left else []:
        hline(cy + o, 0, cx + (t if not vk else (g if vk == 2 else t)))
    for o in offsets(right) if right else []:
        hline(cy + o, cx - (0 if not vk else (g // 2 if vk == 2 else 0)), w)

def draw_block(img, w, h, i):
    px = img.load()
    for y in range(h):
        for x in range(w):
            on = 0
            if i == 0xDB: on = 1
            elif i == 0xDC: on = y >= h // 2
            elif i == 0xDD: on = x < w // 2
            elif i == 0xDE: on = x >= w // 2
            elif i == 0xDF: on = y < h // 2
            elif i == 0xB0: on = (x % 4 == 0 and y % 2 == 0) or (x % 4 == 2 and y % 2 == 1)
            elif i == 0xB1: on = (x + y) % 2 == 0
            elif i == 0xB2: on = not ((x % 4 == 0 and y % 2 == 0) or (x % 4 == 2 and y % 2 == 1))
            if on: px[x, y] = 1

def main():
    path, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    preview = sys.argv[5] if len(sys.argv) > 5 else None
    font = fit_font(path, w, h)
    asc, desc = font.getmetrics()
    top = (h - (asc + desc)) // 2
    adv = font.getlength("M")
    left = int(round((w - adv) / 2))
    bpr = (w + 7) // 8
    glyphs = []
    sheet = Image.new("L", (16 * (w + 2), 16 * (h + 2)), 40) if preview else None
    for i in range(256):
        img = Image.new("1", (w, h), 0)
        if i in BOX:
            draw_box(img, w, h, BOX[i])
        elif i in (0xB0, 0xB1, 0xB2, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF):
            draw_block(img, w, h, i)
        elif i != 0 and i != 32 and i != 255:
            g = Image.new("L", (w, h), 0)
            ImageDraw.Draw(g).text((left, top), cp437_char(i), font=font, fill=255)
            img = g.point(lambda v: 1 if v >= 100 else 0, "1")
        data = bytearray()
        px = img.load()
        for y in range(h):
            for b in range(bpr):
                byte = 0
                for bit in range(8):
                    x = b * 8 + bit
                    if x < w and px[x, y]:
                        byte |= 0x80 >> bit
                data.append(byte)
        glyphs.append(bytes(data))
        if sheet:
            sheet.paste(img.convert("L").point(lambda v: 255 if v else 0), ((i % 16) * (w + 2) + 1, (i // 16) * (h + 2) + 1))
    hdr = struct.pack("<IIIIIIII", 0x864AB572, 0, 32, 0, 256, bpr * h, h, w)
    with open(out, "wb") as f:
        f.write(hdr)
        for g in glyphs:
            f.write(g)
    if sheet:
        sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(preview)
    print(f"{out}: {w}x{h}, font size {font.size}, 256 glyphs")

main()
