#!/usr/bin/env python3
# rozOS — ধাপ ১৮ক: গেমের ছবি বানানো (সব প্রোগ্রাম দিয়ে আঁকা, কারো কপিরাইট নেই)
# ব্যবহার:  python3 toolchain/mkgameart.py   (Pillow লাগে: pip install pillow)
# ফল: sysroot/usr/share/games/*.png|jpg আর sysroot/usr/share/icons/{snake,breakout}.png
import math, os, random
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(__file__), "..")
OUT = os.path.join(ROOT, "sysroot/usr/share/games")
ICONS = os.path.join(ROOT, "sysroot/usr/share/icons")
os.makedirs(OUT, exist_ok=True)
os.makedirs(ICONS, exist_ok=True)
random.seed(18)


def glossy_rect(w, h, color, radius=6):
    """চকচকে ইট/প্যাডেল: ওপরে হালকা, নিচে গাঢ়, ধার ঘেঁষে আলো"""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    base = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = base.load()
    r0, g0, b0 = color
    for y in range(h):
        t = y / max(1, h - 1)
        k = 1.35 - 0.75 * t
        if t < 0.45:
            k += 0.25 * (1 - t / 0.45)
        for x in range(w):
            px[x, y] = (min(255, int(r0 * k)), min(255, int(g0 * k)), min(255, int(b0 * k)), 255)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, fill=255)
    img.paste(base, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, outline=(255, 255, 255, 90))
    d.line([radius, 2, w - radius - 1, 2], fill=(255, 255, 255, 150))
    return img


def sphere(size, color, shine=(255, 255, 255)):
    """গোল বল: ৪ গুণ বড় করে এঁকে ছোট (মসৃণ ধার)"""
    S = size * 4
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    px = img.load()
    cx = cy = S / 2
    R = S / 2 - 2
    lx, ly = cx - R * 0.35, cy - R * 0.4
    for y in range(S):
        for x in range(S):
            d = math.hypot(x - cx, y - cy)
            if d > R:
                continue
            k = 0.45 + 0.75 * (1 - d / R) ** 0.6
            s = max(0.0, 1 - math.hypot(x - lx, y - ly) / (R * 0.55)) ** 2
            r = min(255, int(color[0] * k + shine[0] * s))
            g = min(255, int(color[1] * k + shine[1] * s))
            b = min(255, int(color[2] * k + shine[2] * s))
            px[x, y] = (r, g, b, 255)
    return img.resize((size, size), Image.LANCZOS)


# ---------- Breakout ----------
# পটভূমি: বড় (1280x960) "ফটো"-মতো নীহারিকা আর তারা — গেম নিজের মাপে মসৃণভাবে ছোট করে
W, H = 1280, 960
bg = Image.new("RGB", (W, H), (0, 0, 0))
px = bg.load()
blobs = [(random.uniform(0, W), random.uniform(0, H), random.uniform(180, 520),
          random.choice([(90, 40, 160), (30, 80, 170), (150, 40, 110), (20, 120, 140)])) for _ in range(9)]
for y in range(H):
    for x in range(W):
        r = 4 + 10 * y / H
        g = 6 + 6 * y / H
        b = 18 + 22 * y / H
        for bx, by, br, c in blobs:
            d2 = ((x - bx) ** 2 + (y - by) ** 2) / (br * br)
            if d2 < 4:
                k = math.exp(-d2 * 1.6) * 0.75
                r += c[0] * k
                g += c[1] * k
                b += c[2] * k
        px[x, y] = (min(255, int(r)), min(255, int(g)), min(255, int(b)))
bg = bg.filter(ImageFilter.GaussianBlur(6))
d = ImageDraw.Draw(bg)
for _ in range(900):
    x, y = random.uniform(0, W), random.uniform(0, H)
    v = random.randint(120, 255)
    s = random.choice([0.6, 0.8, 1.0, 1.0, 1.4, 2.2])
    d.ellipse([x - s, y - s, x + s, y + s], fill=(v, v, min(255, v + 20)))
bg = bg.filter(ImageFilter.GaussianBlur(0.6))
bg.save(os.path.join(OUT, "breakout-bg.jpg"), quality=88)

# ইট: ৬ রং পাশাপাশি (64x24 প্রতিটা) — sprite sheet
colors = [(220, 60, 60), (235, 140, 40), (230, 210, 50), (70, 190, 80), (60, 140, 230), (160, 80, 220)]
sheet = Image.new("RGBA", (64 * len(colors), 24), (0, 0, 0, 0))
for i, c in enumerate(colors):
    sheet.paste(glossy_rect(64, 24, c), (i * 64, 0))
sheet.save(os.path.join(OUT, "breakout-bricks.png"))
glossy_rect(104, 18, (200, 210, 230), radius=9).save(os.path.join(OUT, "breakout-paddle.png"))
sphere(16, (240, 240, 255)).save(os.path.join(OUT, "breakout-ball.png"))

# ---------- Snake ----------
# আপেল (32x32, স্বচ্ছ পটভূমি, পাতা সহ)
S = 128
apple = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ball = sphere(104, (210, 30, 40), shine=(255, 200, 200)).resize((S - 20, S - 20), Image.LANCZOS)
apple.paste(ball, (10, 18), ball)
ad = ImageDraw.Draw(apple)
ad.line([S / 2, 22, S / 2 + 6, 4], fill=(90, 60, 30, 255), width=7)
ad.ellipse([S / 2 + 4, 4, S / 2 + 36, 22], fill=(60, 170, 60, 255))
apple.resize((32, 32), Image.LANCZOS).save(os.path.join(OUT, "snake-apple.png"))

# ঘাসের টাইল (40x40), একটু দাগ — পুরো মাঠে বিছানো হয়
tile = Image.new("RGB", (40, 40), (36, 92, 44))
td = ImageDraw.Draw(tile)
for _ in range(60):
    x, y = random.randint(0, 39), random.randint(0, 39)
    g = random.randint(80, 130)
    td.line([x, y, x + random.randint(-1, 1), y - random.randint(1, 3)], fill=(30, g, 40))
tile.save(os.path.join(OUT, "snake-grass.png"))

# ---------- আইকন (Start মেনু/ডেস্কটপ) ----------
ic = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
idr = ImageDraw.Draw(ic)
idr.rounded_rectangle([2, 2, 61, 61], radius=12, fill=(30, 70, 40, 255))
pts = [(14, 46), (14, 30), (30, 30), (30, 18), (48, 18)]
idr.line(pts, fill=(120, 220, 90, 255), width=9, joint="curve")
idr.ellipse([42, 12, 54, 24], fill=(150, 240, 110, 255))
a = Image.open(os.path.join(OUT, "snake-apple.png")).resize((20, 20), Image.LANCZOS)
ic.paste(a, (36, 36), a)
ic.save(os.path.join(ICONS, "snake.png"))

ic = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
idr = ImageDraw.Draw(ic)
idr.rounded_rectangle([2, 2, 61, 61], radius=12, fill=(20, 24, 60, 255))
for i, c in enumerate(colors[:4]):
    b = glossy_rect(24, 9, c, radius=3)
    ic.paste(b, (6 + (i % 2) * 27, 8 + (i // 2) * 12), b)
p = glossy_rect(30, 7, (200, 210, 230), radius=3)
ic.paste(p, (17, 50), p)
bl = sphere(10, (240, 240, 255))
ic.paste(bl, (34, 36), bl)
ic.save(os.path.join(ICONS, "breakout.png"))
print("game art written to", OUT)
