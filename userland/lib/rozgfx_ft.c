/* rozOS — librozgfx: FreeType দিয়ে মসৃণ লেখা (ধাপ ২৩ক)
 *
 * সিস্টেম ফন্ট দুটো, /etc/rozos.conf থেকে (না থাকলে ডিফল্ট):
 *   ui_font=/usr/share/fonts/Inter-Regular.ttf          ui_font_size=14
 *   mono_font=/usr/share/fonts/JetBrainsMono-Regular.ttf mono_font_size=15
 * gfx_text(s, NULL, ...) এখন UI ফন্টে আঁকে (আনুপাতিক, ১৬ px লাইনের বাক্সে বসানো — আগের
 * 8×16 PSF-এর জায়গায়, তাই পুরোনো লেআউট মোটামুটি একই থাকে)। টার্মিনাল আর Code
 * gfx_font_mono() নেয় — সমান চওড়া ঘর (JetBrains Mono 15 px ≈ 9×20)।
 *
 * আঁকা: FT_LOAD_TARGET_LIGHT (শুধু খাড়া দিকে hinting — অক্ষরের আকার ঠিক থাকে, তবু ঝকঝকে),
 * ৮-বিট ধূসর কভারেজ → রঙের alpha; গাঢ় পটভূমিতে সাদা লেখা পাতলা না দেখাতে হালকা
 * contrast বাড়ানো। প্রতি ফন্টে ৫১২ ঘরের glyph cache। অক্ষর না পেলে fallback ফন্ট
 * (DejaVu Sans / DejaVu Sans Mono — অনেক চিহ্ন আছে)।
 *
 * FreeType না থাকলে (ports/freetype/src নেই) এই ফাইল বিল্ড হয় না, আর rozgfx.c আগের
 * মতো PSF দিয়ে আঁকে। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rozgfx.h"
#include "rozgfx_ft.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef ROZOS_HAVE_KBTS
#include "rozgfx_shape.h"                     /* ধাপ ২৩খ: বাংলা shaping */
#endif

#define CACHE_SLOTS 512
#define PROBE       8

struct ft_glyph {
    int cp;                                   /* -1 = খালি */
    short w, h, left, top, adv;
    unsigned char *bits;
    struct rozft *src;                        /* কোন ফন্ট থেকে (fallback হলে অন্যটা) */
};

struct rozft {
    FT_Face face;
    int px;                                   /* em মাপ (পিক্সেল) */
    int mono;
    int asc, desc, line;                      /* পিক্সেল; desc ধনাত্মক */
    int cap;                                  /* বড় হাতের অক্ষরের উচ্চতা */
    int cell_w;                               /* mono: এক ঘরের চওড়া */
    struct rozft *fallback;
    int fallback_tried;
    /* ধাপ ২৩খ: বাংলা — আলাদা ফন্ট (Noto Sans Bengali) আর shaping-এর context */
    char *path;
    struct rozft *bengali;
    int bengali_tried;
    void *shape;                              /* struct rozshape * */
    int shape_tried;
    struct ft_glyph cache[CACHE_SLOTS];
};

static FT_Library lib;
static int lib_state;                         /* 0 = চেষ্টা হয়নি, 1 = আছে, -1 = নেই */
static unsigned char boost[256];              /* কভারেজ → alpha (contrast) */

static int ft_lib(void)
{
    int i;
    if (lib_state == 0) {
        lib_state = FT_Init_FreeType(&lib) == 0 ? 1 : -1;
        for (i = 0; i < 256; i++) {
            /* মাঝের মান একটু বাড়ানো: a + a(255-a)/400 — গাঢ় থিমে পাতলা লেখা এড়াতে */
            int v = i + i * (255 - i) / 400;
            boost[i] = (unsigned char)(v > 255 ? 255 : v);
        }
    }
    return lib_state == 1;
}

int rozft_available(void) { return ft_lib(); }

struct rozft *rozft_open(const char *path, int px, int mono)
{
    struct rozft *f;
    int i;
    if (!path || px < 4 || px > 400 || !ft_lib()) return NULL;
    f = (struct rozft *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    if (FT_New_Face(lib, path, 0, &f->face) != 0) {
        free(f);
        return NULL;
    }
    if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)px) != 0) {
        FT_Done_Face(f->face);
        free(f);
        return NULL;
    }
    f->px = px;
    f->mono = mono;
    f->asc = (int)((f->face->size->metrics.ascender + 63) >> 6);
    f->desc = (int)((-f->face->size->metrics.descender + 63) >> 6);
    f->line = f->asc + f->desc;
    if (FT_Load_Char(f->face, 'H', FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) == 0)
        f->cap = (int)((f->face->glyph->metrics.horiBearingY + 32) >> 6);
    if (f->cap <= 0) f->cap = px * 7 / 10;
    if (FT_Load_Char(f->face, 'M', FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) == 0)
        f->cell_w = (int)((f->face->glyph->advance.x + 32) >> 6);
    if (f->cell_w <= 0) f->cell_w = px * 6 / 10;
    for (i = 0; i < CACHE_SLOTS; i++) f->cache[i].cp = -1;
    f->path = (char *)malloc(strlen(path) + 1);
    if (f->path) strcpy(f->path, path);
    return f;
}

void rozft_close(struct rozft *f)
{
    int i;
    if (!f) return;
    for (i = 0; i < CACHE_SLOTS; i++) free(f->cache[i].bits);
    if (f->fallback) rozft_close(f->fallback);
    if (f->bengali) rozft_close(f->bengali);
#ifdef ROZOS_HAVE_KBTS
    if (f->shape) rozshape_close((struct rozshape *)f->shape);
#endif
    free(f->path);
    FT_Done_Face(f->face);
    free(f);
}

int rozft_cell_w(const struct rozft *f) { return f->cell_w; }
int rozft_line_h(const struct rozft *f) { return f->line; }
int rozft_px(const struct rozft *f)     { return f->px; }

/* ---------- glyph ---------- */

static struct rozft *fallback_of(struct rozft *f)
{
    if (!f->fallback_tried) {
        f->fallback_tried = 1;
        f->fallback = rozft_open(f->mono ? "/usr/share/fonts/DejaVuSansMono.ttf"
                                         : "/usr/share/fonts/DejaVuSans.ttf", f->px, f->mono);
    }
    return f->fallback;
}

/* ধাপ ২৩খ: বাংলা ফন্ট (/etc/rozos.conf-এর bengali_font, ডিফল্ট Noto Sans Bengali), একই মাপে */
static void conf_get(const char *key, char *out, size_t size);

static struct rozft *bengali_of(struct rozft *f)
{
    if (!f->bengali_tried) {
        char path[256] = "/usr/share/fonts/NotoSansBengali.ttf";
        f->bengali_tried = 1;
        conf_get("bengali_font", path, sizeof(path));
        f->bengali = rozft_open(path, f->px, 0);
    }
    return f->bengali;
}

/* cache-এ key-এর ঘর (না থাকলে খালি করে দেয়, *fresh = 1) */
static struct ft_glyph *cache_slot(struct rozft *f, int key, int *fresh)
{
    unsigned h = ((unsigned)key * 2654435761u) >> 23;         /* ৯ বিট = ৫১২ ঘর */
    struct ft_glyph *g = NULL, *slot;
    int i;
    *fresh = 0;
    for (i = 0; i < PROBE; i++) {
        slot = &f->cache[(h + (unsigned)i) % CACHE_SLOTS];
        if (slot->cp == key) return slot;
        if (slot->cp == -1 && !g) g = slot;
    }
    if (!g) g = &f->cache[h % CACHE_SLOTS];                    /* ভরা — প্রথমটা বদলাও */
    free(g->bits);
    memset(g, 0, sizeof(*g));
    g->cp = key;
    *fresh = 1;
    return g;
}

static void load_glyph(struct rozft *f, struct ft_glyph *g, struct rozft *src, FT_UInt idx);

static struct ft_glyph *get_glyph(struct rozft *f, int cp)
{
    struct ft_glyph *g;
    struct rozft *src = f;
    FT_UInt idx;
    int fresh;

    g = cache_slot(f, cp, &fresh);
    if (!fresh) return g;
    idx = FT_Get_Char_Index(f->face, (FT_ULong)cp);
    if (idx == 0 && cp > 32) {
        struct rozft *fb = fallback_of(f);
        FT_UInt fi = fb ? FT_Get_Char_Index(fb->face, (FT_ULong)cp) : 0;
        if (fi) { src = fb; idx = fi; }
        else if (cp >= 0x0980 && cp <= 0x09FF && bengali_of(f)) {   /* ধাপ ২৩খ */
            fi = FT_Get_Char_Index(f->bengali->face, (FT_ULong)cp);
            if (fi) { src = f->bengali; idx = fi; }
        }
    }
    load_glyph(f, g, src, idx);
    return g;
}

/* shaping-এর glyph নম্বর ধরে (which 0 = এই ফন্ট, 1 = বাংলা ফন্ট) */
static struct ft_glyph *get_glyph_id(struct rozft *f, int which, unsigned id)
{
    int fresh;
    struct ft_glyph *g = cache_slot(f, 0x40000000 | (which << 20) | (int)(id & 0xFFFFF), &fresh);
    if (fresh) load_glyph(f, g, which && bengali_of(f) ? f->bengali : f, id);
    return g;
}

static void load_glyph(struct rozft *f, struct ft_glyph *g, struct rozft *src, FT_UInt idx)
{
    FT_GlyphSlot gs;
    int size;
    g->src = src;
    if (FT_Load_Glyph(src->face, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
        g->adv = (short)(f->mono ? f->cell_w : f->px / 3);
        return;
    }
    gs = src->face->glyph;
    g->adv = (short)((gs->advance.x + 32) >> 6);
    g->left = (short)gs->bitmap_left;
    g->top = (short)gs->bitmap_top;
    g->w = (short)gs->bitmap.width;
    g->h = (short)gs->bitmap.rows;
    size = g->w * g->h;
    if (size > 0 && gs->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        g->bits = (unsigned char *)malloc((size_t)size);
        if (g->bits) {
            int y;
            for (y = 0; y < g->h; y++)
                memcpy(g->bits + y * g->w, gs->bitmap.buffer + y * gs->bitmap.pitch, (size_t)g->w);
        }
    }
    if (!g->bits) g->w = g->h = 0;
}

/* বিটম্যাপ মেশানো (clip মেনে) */
static void blit_glyph(gfx_surface *s, const struct ft_glyph *g, int ox, int oy, uint32_t color)
{
    int x0 = ox + g->left, y0 = oy - g->top, i, j;
    int cx0 = s->clip_x, cy0 = s->clip_y, cx1 = s->clip_x + s->clip_w, cy1 = s->clip_y + s->clip_h;
    uint32_t ca = GFX_A(color), cr = GFX_R(color), cg = GFX_G(color), cb = GFX_B(color);
    if (!g->bits) return;
    for (j = 0; j < g->h; j++) {
        int y = y0 + j;
        uint32_t *row;
        const unsigned char *src;
        if (y < cy0 || y >= cy1) continue;
        row = s->px + (size_t)y * s->stride;
        src = g->bits + j * g->w;
        for (i = 0; i < g->w; i++) {
            int x = x0 + i;
            uint32_t a, d;
            if (!src[i] || x < cx0 || x >= cx1) continue;
            a = (uint32_t)boost[src[i]] * ca / 255;
            if (a >= 255) { row[x] = (color & 0x00FFFFFFu) | 0xFF000000u; continue; }
            d = row[x];
            row[x] = 0xFF000000u |
                     (((cr * a + GFX_R(d) * (255 - a)) / 255) << 16) |
                     (((cg * a + GFX_G(d) * (255 - a)) / 255) << 8) |
                     ((cb * a + GFX_B(d) * (255 - a)) / 255);
        }
    }
}

static int utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int c = *p++, extra = 0;
    if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else c = 0xFFFD;
        while (extra-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    }
    *pp = (const char *)p;
    return c;
}

/* লেখার বেসলাইন: box_h-উঁচু লাইনের বাক্সে বড় হাতের অক্ষর মাঝখানে (UI), বা
 * ঘরের ওপরে ascent (mono — ঘরের উচ্চতা = line) */
static int baseline_in(const struct rozft *f, int box_h)
{
    if (f->mono && box_h >= f->line) return (box_h - f->line) / 2 + f->asc;
    return (box_h + f->cap + 1) / 2;
}

/* ---------- ধাপ ২৩খ: বাংলা লাইন (shaping) ---------- */
#ifdef ROZOS_HAVE_KBTS
struct shape_draw {
    struct rozft *f;
    gfx_surface *s;
    int x, base;
    uint32_t color;
};

static void shape_draw_cb(void *ud, int which, int glyph, long x, long y, int cp)
{
    struct shape_draw *d = (struct shape_draw *)ud;
    struct ft_glyph *g;
    (void)cp;
    if (!d->s) return;
    g = get_glyph_id(d->f, which, (unsigned)glyph);
    blit_glyph(d->s, g, d->x + (int)((x + 32) >> 6), d->base - (int)((y + 32) >> 6), d->color);
}

static struct rozshape *shaper_of(struct rozft *f)
{
    if (!f->shape_tried) {
        struct rozft *bn = bengali_of(f);
        f->shape_tried = 1;
        f->shape = rozshape_open(f->path, bn ? bn->path : NULL);
    }
    return (struct rozshape *)f->shape;
}

/* line-এর len বাইট সাজিয়ে আঁকা (s NULL = শুধু মাপা); ফেরত চওড়া (পিক্সেল), -1 = shaping নেই */
static int shaped_line(struct rozft *f, gfx_surface *s, int x, int base, const char *line, int len,
                       uint32_t color)
{
    struct rozshape *sh;
    struct shape_draw d;
    int upem[2];
    long w;
    if (!rozshape_has_complex(line, len) || !(sh = shaper_of(f))) return -1;
    upem[0] = f->face->units_per_EM;
    upem[1] = f->bengali ? f->bengali->face->units_per_EM : 1000;
    d.f = f;
    d.s = s;
    d.x = x;
    d.base = base;
    d.color = color;
    w = rozshape_run(sh, line, len, f->px, upem, shape_draw_cb, &d);
    return (int)((w + 32) >> 6);
}
#else
static int shaped_line(struct rozft *f, gfx_surface *s, int x, int base, const char *line, int len,
                       uint32_t color)
{
    (void)f; (void)s; (void)x; (void)base; (void)line; (void)len; (void)color;
    return -1;
}
#endif

int rozft_text(struct rozft *f, gfx_surface *s, int x, int y, int box_h, const char *str,
               uint32_t fg, uint32_t bg, int with_bg)
{
    int x0 = x, base = y + baseline_in(f, box_h);
    if (!str) return x;
    if (with_bg) {
        /* পটভূমি আগে — লাইন ধরে */
        const char *p = str;
        int yy = y;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            char tmp[512];
            if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
            memcpy(tmp, p, n);
            tmp[n] = '\0';
            gfx_fill_rect(s, x, yy, rozft_width(f, tmp), box_h, bg);
            if (!e) break;
            p = e + 1;
            yy += box_h;
        }
    }
    while (*str) {
        int cp, sw;
        struct ft_glyph *g;
        if (x == x0) {                               /* ধাপ ২৩খ: লাইনে বাংলা থাকলে সাজিয়ে আঁকা */
            const char *e = strchr(str, '\n');
            int n = e ? (int)(e - str) : (int)strlen(str);
            sw = shaped_line(f, s, x, base, str, n, fg);
            if (sw >= 0) {
                x += sw;
                str += n;
                if (*str == '\n') { str++; x = x0; base += box_h; }
                continue;
            }
        }
        cp = utf8_next(&str);
        if (cp == '\n') { x = x0; base += box_h; continue; }
        if (cp == '\t') cp = ' ';
        g = get_glyph(f, cp);
        if (f->mono) {
            blit_glyph(s, g, x + (f->cell_w - g->adv) / 2, base, fg);
            x += f->cell_w;
        } else {
            blit_glyph(s, g, x, base, fg);
            x += g->adv;
        }
    }
    return x;
}

int rozft_width(struct rozft *f, const char *str)
{
    int w = 0, best = 0, at_start = 1;
    if (!str) return 0;
    while (*str) {
        int cp, sw;
        if (at_start) {                              /* ধাপ ২৩খ */
            const char *e = strchr(str, '\n');
            int n = e ? (int)(e - str) : (int)strlen(str);
            at_start = 0;
            sw = shaped_line(f, NULL, 0, 0, str, n, 0);
            if (sw >= 0) {
                w = sw;
                str += n;
                continue;
            }
        }
        cp = utf8_next(&str);
        if (cp == '\n') { if (w > best) best = w; w = 0; at_start = 1; continue; }
        if (cp == '\t') cp = ' ';
        w += f->mono ? f->cell_w : get_glyph(f, cp)->adv;
    }
    return w > best ? w : best;
}

/* CP437 (PSF ফন্টের ক্রম — rozterm ঘরে এটাই রাখে) → Unicode */
static const unsigned short cp437_hi[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
};

/* ঘরে একটা অক্ষর (পটভূমি সহ) — box/ব্লক চিহ্ন ঘর ভরে আঁকা, যাতে লাইন জোড়া লাগে */
void rozft_cell(struct rozft *f, gfx_surface *s, int x, int y, int cell_h, int index,
                uint32_t fg, uint32_t bg)
{
    int cp = index < 0 ? '?' : index < 128 ? index : index < 256 ? cp437_hi[index - 128] : index;
    struct ft_glyph *g;
    gfx_fill_rect(s, x, y, f->cell_w, cell_h, bg);
    if (cp <= 32) return;
    if (cp == 0x2588) { gfx_fill_rect(s, x, y, f->cell_w, cell_h, fg); return; }        /* █ */
    if (cp == 0x2580) { gfx_fill_rect(s, x, y, f->cell_w, cell_h / 2, fg); return; }    /* ▀ */
    if (cp == 0x2584) { gfx_fill_rect(s, x, y + cell_h / 2, f->cell_w, cell_h - cell_h / 2, fg); return; }
    if (cp >= 0x2500 && cp <= 0x257F) {                       /* বক্স-রেখা: ঘরের মাঝ দিয়ে */
        int mx = x + f->cell_w / 2, my = y + cell_h / 2;
        int l = 0, r = 0, u = 0, d = 0;
        switch (cp) {
        case 0x2500: case 0x2550: l = r = 1; break;
        case 0x2502: case 0x2551: u = d = 1; break;
        case 0x250C: case 0x2554: r = d = 1; break;
        case 0x2510: case 0x2557: l = d = 1; break;
        case 0x2514: case 0x255A: r = u = 1; break;
        case 0x2518: case 0x255D: l = u = 1; break;
        case 0x251C: case 0x2560: u = d = r = 1; break;
        case 0x2524: case 0x2563: u = d = l = 1; break;
        case 0x252C: case 0x2566: l = r = d = 1; break;
        case 0x2534: case 0x2569: l = r = u = 1; break;
        case 0x253C: case 0x256C: l = r = u = d = 1; break;
        default: break;
        }
        if (l | r | u | d) {
            if (l) gfx_fill_rect(s, x, my, mx - x + 1, 1, fg);
            if (r) gfx_fill_rect(s, mx, my, x + f->cell_w - mx, 1, fg);
            if (u) gfx_fill_rect(s, mx, y, 1, my - y + 1, fg);
            if (d) gfx_fill_rect(s, mx, my, 1, y + cell_h - my, fg);
            return;
        }
    }
    g = get_glyph(f, cp);
    blit_glyph(s, g, x + (f->cell_w - g->adv) / 2, y + baseline_in(f, cell_h), fg);
}

/* ---------- /etc/rozos.conf ---------- */

static void conf_get(const char *key, char *out, size_t size)
{
    FILE *fp = fopen("/etc/rozos.conf", "r");
    char line[256];
    size_t kl = strlen(key);
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1, *e = v + strlen(v);
            while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = '\0';
            if (*v) {
                strncpy(out, v, size - 1);
                out[size - 1] = '\0';
            }
            break;
        }
    }
    fclose(fp);
}

void rozft_system_font(int mono, char *path, size_t size, int *px)
{
    char v[32] = "";
    strncpy(path, mono ? "/usr/share/fonts/JetBrainsMono-Regular.ttf"
                       : "/usr/share/fonts/Inter-Regular.ttf", size - 1);
    path[size - 1] = '\0';
    *px = mono ? 15 : 14;
    conf_get(mono ? "mono_font" : "ui_font", path, size);
    conf_get(mono ? "mono_font_size" : "ui_font_size", v, sizeof(v));
    if (v[0]) {
        int n = atoi(v);
        if (n >= 8 && n <= 48) *px = n;
    }
}
