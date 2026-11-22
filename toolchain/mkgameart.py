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

# ======================================================================
# ধাপ ১৮খ: Tetris আর platformer-এর ছবি
# ======================================================================
random.seed(1802)


def bevel_block(size, color):
    """Tetris-এর ব্লক: আলো-ছায়ার ধার, মাঝে হালকা ঢাল"""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 255))
    px = img.load()
    r0, g0, b0 = color
    e = max(2, size // 8)
    for y in range(size):
        for x in range(size):
            if x < e and y < size - x or y < e and x < size - y:
                k = 1.45                                  # ওপর/বাঁ ধার আলো
            elif x >= size - e or y >= size - e:
                k = 0.55                                  # নিচ/ডান ধার ছায়া
            else:
                k = 1.15 - 0.3 * (x + y) / (2 * size)
            px[x, y] = (min(255, int(r0 * k)), min(255, int(g0 * k)), min(255, int(b0 * k)), 255)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, size - 1, size - 1], outline=(0, 0, 0, 255))
    d.line([e + 1, e + 2, e + 4, e + 2], fill=(255, 255, 255, 200))
    return img


# I O T S Z J L — প্রচলিত রং; শেষে একটা ধূসর (মোছা লাইন/ভরা দেখানোর জন্য)
tet_colors = [(40, 200, 230), (240, 210, 40), (170, 70, 220), (70, 200, 80), (230, 60, 60), (50, 90, 230), (240, 140, 40),
              (130, 130, 140)]
B = 24
sheet = Image.new("RGBA", (B * len(tet_colors), B), (0, 0, 0, 0))
for i, c in enumerate(tet_colors):
    sheet.paste(bevel_block(B, c), (i * B, 0))
sheet.save(os.path.join(OUT, "tetris-blocks.png"))

# পটভূমি: গাঢ় নীল ঢাল + হালকা জ্যামিতিক নকশা
W, H = 640, 480
bg = Image.new("RGB", (W, H))
d = ImageDraw.Draw(bg)
for y in range(H):
    t = y / H
    d.line([0, y, W, y], fill=(int(14 + 20 * t), int(16 + 10 * t), int(40 + 30 * t)))
for _ in range(40):
    x, y, s = random.randint(0, W), random.randint(0, H), random.randint(10, 60)
    v = random.randint(30, 60)
    d.rectangle([x, y, x + s, y + s], outline=(v, v, v + 40))
bg = bg.filter(ImageFilter.GaussianBlur(1.2))
bg.save(os.path.join(OUT, "tetris-bg.png"))

# ---------- platformer ----------
T = 32


def tile_dirt(top_grass):
    img = Image.new("RGBA", (T, T), (120, 78, 44, 255))
    td = ImageDraw.Draw(img)
    for _ in range(14):
        x, y = random.randint(0, T - 3), random.randint(0, T - 3)
        c = random.choice([(100, 64, 36), (140, 92, 54), (90, 58, 30)])
        td.ellipse([x, y, x + random.randint(2, 4), y + random.randint(2, 3)], fill=c + (255,))
    if top_grass:
        td.rectangle([0, 0, T - 1, 8], fill=(70, 170, 60, 255))
        for x in range(0, T, 3):
            td.line([x, 8, x + 1, 8 + random.randint(1, 4)], fill=(70, 170, 60, 255), width=2)
        td.line([0, 1, T - 1, 1], fill=(130, 220, 100, 255))
    return img


def tile_stone():
    img = Image.new("RGBA", (T, T), (110, 110, 120, 255))
    td = ImageDraw.Draw(img)
    td.rectangle([0, 0, T - 1, T - 1], outline=(70, 70, 80, 255))
    td.line([1, 1, T - 2, 1], fill=(160, 160, 170, 255))
    td.line([1, 1, 1, T - 2], fill=(160, 160, 170, 255))
    for _ in range(6):
        x, y = random.randint(4, T - 6), random.randint(4, T - 6)
        td.line([x, y, x + random.randint(-3, 3), y + random.randint(2, 5)], fill=(85, 85, 95, 255))
    return img


def tile_brick():
    img = Image.new("RGBA", (T, T), (170, 70, 50, 255))
    td = ImageDraw.Draw(img)
    for row in range(4):
        y = row * 8
        td.line([0, y, T, y], fill=(220, 200, 180, 255))
        off = 0 if row % 2 == 0 else 8
        for x in range(off, T, 16):
            td.line([x, y, x, y + 7], fill=(220, 200, 180, 255))
    return img


def tile_spikes():
    img = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    td = ImageDraw.Draw(img)
    for i in range(4):
        x = i * 8
        td.polygon([(x, T - 1), (x + 4, T - 14), (x + 8, T - 1)], fill=(200, 200, 210, 255), outline=(90, 90, 100, 255))
    return img


def tile_crate():
    img = Image.new("RGBA", (T, T), (170, 120, 60, 255))
    td = ImageDraw.Draw(img)
    td.rectangle([0, 0, T - 1, T - 1], outline=(90, 60, 30, 255), width=2)
    td.rectangle([4, 4, T - 5, T - 5], outline=(120, 80, 40, 255), width=2)
    td.line([5, 5, T - 6, T - 6], fill=(120, 80, 40, 255), width=3)
    return img


def tile_flag():
    img = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    td = ImageDraw.Draw(img)
    td.rectangle([6, 0, 9, T - 1], fill=(220, 220, 220, 255))
    td.polygon([(10, 2), (30, 9), (10, 16)], fill=(230, 50, 60, 255))
    td.ellipse([4, 0, 11, 5], fill=(250, 210, 60, 255))
    return img


def tile_pole():
    img = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    ImageDraw.Draw(img).rectangle([6, 0, 9, T - 1], fill=(220, 220, 220, 255))
    return img


def tile_bush():
    img = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    td = ImageDraw.Draw(img)
    for cx, cy, r in [(9, 22, 9), (20, 20, 11), (27, 25, 6)]:
        td.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(50, 140, 60, 255))
    td.ellipse([14, 13, 20, 18], fill=(90, 190, 90, 255))
    return img


# ফ্রেম: 0 ঘাস, 1 মাটি, 2 পাথর, 3 ইট, 4 কাঁটা, 5 বাক্স, 6 পতাকা, 7 খুঁটি, 8 ঝোপ
tiles = [tile_dirt(True), tile_dirt(False), tile_stone(), tile_brick(), tile_spikes(), tile_crate(), tile_flag(),
         tile_pole(), tile_bush()]
ts = Image.new("RGBA", (T * len(tiles), T), (0, 0, 0, 0))
for i, t in enumerate(tiles):
    ts.paste(t, (i * T, 0), t)
ts.save(os.path.join(OUT, "platformer-tiles.png"))


def hero(frame):
    """খেলোয়াড় (32x32, ৪ গুণ বড় এঁকে ছোট): 0-1 দাঁড়ানো, 2-5 দৌড়, 6 লাফ, 7 পড়া"""
    S = 4
    img = Image.new("RGBA", (32 * S, 32 * S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(img)
    bob = [0, 1, 0, -1, 0, -1, -2, 1][frame]
    legs = [(0, 0), (0, 0), (-5, 5), (0, 0), (5, -5), (0, 0), (-4, 4), (3, -3)][frame]
    body_y = 10 + bob
    # পা
    for i, dx in enumerate(legs):
        x = 12 + i * 8 + dx
        hd.rounded_rectangle([x * S, 24 * S, (x + 5) * S, 31 * S], radius=2 * S, fill=(50, 60, 120, 255))
    # শরীর
    hd.rounded_rectangle([7 * S, body_y * S, 25 * S, (body_y + 16) * S], radius=7 * S, fill=(240, 120, 50, 255))
    hd.rounded_rectangle([9 * S, (body_y + 2) * S, 16 * S, (body_y + 6) * S], radius=2 * S, fill=(255, 180, 120, 255))
    # মাথা আর চোখ
    hd.ellipse([9 * S, (body_y - 9) * S, 25 * S, (body_y + 6) * S], fill=(250, 210, 170, 255))
    eye_y = body_y - 3 + (1 if frame == 7 else 0)
    for ex in (17, 21):
        hd.ellipse([ex * S, eye_y * S, (ex + 3) * S, (eye_y + 4) * S], fill=(255, 255, 255, 255))
        hd.ellipse([(ex + 1) * S, (eye_y + 1) * S, (ex + 3) * S, (eye_y + 3) * S], fill=(20, 20, 30, 255))
    # টুপি
    hd.rounded_rectangle([8 * S, (body_y - 11) * S, 25 * S, (body_y - 5) * S], radius=3 * S, fill=(40, 150, 220, 255))
    hd.rectangle([20 * S, (body_y - 7) * S, 29 * S, (body_y - 5) * S], fill=(40, 150, 220, 255))
    return img.resize((32, 32), Image.LANCZOS)


hs = Image.new("RGBA", (32 * 8, 32), (0, 0, 0, 0))
for f in range(8):
    h = hero(f)
    hs.paste(h, (f * 32, 0), h)
hs.save(os.path.join(OUT, "platformer-hero.png"))

# মুদ্রা: ঘোরার মতো চ্যাপ্টা (৬ ফ্রেম, 20x20)
cs = Image.new("RGBA", (20 * 6, 20), (0, 0, 0, 0))
for f, wscale in enumerate([1.0, 0.75, 0.4, 0.15, 0.4, 0.75]):
    S = 4
    img = Image.new("RGBA", (20 * S, 20 * S), (0, 0, 0, 0))
    cd = ImageDraw.Draw(img)
    hw = max(1, int(8 * S * wscale))
    cx = 10 * S
    cd.ellipse([cx - hw, 2 * S, cx + hw, 18 * S], fill=(240, 190, 40, 255), outline=(170, 120, 20, 255), width=S)
    if wscale > 0.5:
        cd.ellipse([cx - hw // 2, 6 * S, cx + hw // 2, 14 * S], outline=(255, 230, 120, 255), width=S)
    img = img.resize((20, 20), Image.LANCZOS)
    cs.paste(img, (f * 20, 0), img)
cs.save(os.path.join(OUT, "platformer-coin.png"))

# শত্রু (slime): ২ ফ্রেম, 32x24
ss = Image.new("RGBA", (64, 24), (0, 0, 0, 0))
for f in range(2):
    S = 4
    img = Image.new("RGBA", (32 * S, 24 * S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(img)
    top = 6 if f == 0 else 9
    sd.rounded_rectangle([2 * S, top * S, 30 * S, 23 * S], radius=10 * S, fill=(150, 60, 200, 255))
    sd.ellipse([6 * S, (top + 3) * S, 12 * S, (top + 7) * S], fill=(210, 150, 255, 255))
    for ex in (12, 19):
        sd.ellipse([ex * S, (top + 5) * S, (ex + 4) * S, (top + 10) * S], fill=(255, 255, 255, 255))
        sd.ellipse([(ex + 1) * S, (top + 7) * S, (ex + 3) * S, (top + 9) * S], fill=(20, 20, 30, 255))
    img = img.resize((32, 24), Image.LANCZOS)
    ss.paste(img, (f * 32, 0), img)
ss.save(os.path.join(OUT, "platformer-slime.png"))

# দূরের পাহাড় (parallax): 1280x480, ওপর স্বচ্ছ
hills = Image.new("RGBA", (1280, 480), (0, 0, 0, 0))
hd = ImageDraw.Draw(hills)
for layer, (base, amp, col) in enumerate([(300, 90, (120, 170, 200)), (360, 60, (90, 150, 120))]):
    pts = [(0, 480)]
    for x in range(0, 1281, 8):
        y = base - amp * (0.6 * math.sin(x / 1280 * 2 * math.pi * (2 + layer)) +
                          0.4 * math.sin(x / 1280 * 2 * math.pi * (5 + layer * 2) + layer))
        pts.append((x, y))
    pts.append((1280, 480))
    hd.polygon(pts, fill=col + (255,))
for _ in range(7):                                    # মেঘ
    x, y = random.randint(0, 1200), random.randint(30, 160)
    for i in range(4):
        r = random.randint(18, 34)
        hd.ellipse([x + i * 22 - r, y - r // 2, x + i * 22 + r, y + r // 2 + 6], fill=(255, 255, 255, 220))
hills = hills.filter(ImageFilter.GaussianBlur(0.8))
hills.save(os.path.join(OUT, "platformer-hills.png"))

# আইকন
ic = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
idr = ImageDraw.Draw(ic)
idr.rounded_rectangle([2, 2, 61, 61], radius=12, fill=(20, 24, 50, 255))
shapes = [[(0, 0), (1, 0), (2, 0), (1, 1)], [(0, 0), (0, 1), (1, 1), (2, 1)], [(0, 0), (1, 0), (0, 1), (1, 1)]]
for si, (shape, (ox, oy)) in enumerate(zip(shapes, [(10, 10), (28, 30), (14, 40)])):
    blk = bevel_block(10, tet_colors[[2, 5, 1][si]]).convert("RGBA")
    for (bx, by) in shape:
        ic.paste(blk, (ox + bx * 10, oy + by * 10))
ic.save(os.path.join(ICONS, "tetris.png"))

ic = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
idr = ImageDraw.Draw(ic)
idr.rounded_rectangle([2, 2, 61, 61], radius=12, fill=(120, 190, 240, 255))
for i in range(4):
    ic.paste(tiles[0].resize((14, 14)), (4 + i * 14, 46))
h = hero(6).resize((30, 30), Image.LANCZOS)
ic.paste(h, (16, 12), h)
c = cs.crop((0, 0, 20, 20)).resize((12, 12))
ic.paste(c, (44, 12), c)
ic.save(os.path.join(ICONS, "platformer.png"))
print("phase 18b game art written")
