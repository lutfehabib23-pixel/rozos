/* rozOS — librozgfx: TrueType লেখা (ধাপ ১৩), stb_truetype দিয়ে
 *
 * প্রতিটা অক্ষর stbtt-এর 8-বিট কভারেজ বিটম্যাপে আঁকা হয়, তারপর রঙের alpha হিসেবে মেশানো
 * (মসৃণ ধার)। ছোট ক্যাশ: শেষ ব্যবহৃত (অক্ষর, মাপ) জোড়ার বিটম্যাপ রাখা হয়, যাতে একই
 * লেখা বারবার আঁকা দ্রুত হয়। UTF-8 পড়ে; shaping (যুক্তাক্ষর) নেই। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "rozgfx.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define CACHE_SIZE 256

static int iround(float v) { return (int)floorf(v + 0.5f); }

struct glyph_cache {
    int cp;
    float size;
    int w, h, xoff, yoff, advance;
    unsigned char *bits;
};

struct gfx_ttf {
    unsigned char *data;
    stbtt_fontinfo info;
    struct glyph_cache cache[CACHE_SIZE];
};

gfx_ttf *gfx_ttf_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    gfx_ttf *t;
    long n;
    if (!f) return NULL;
    t = (gfx_ttf *)calloc(1, sizeof(*t));
    if (!t) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    t->data = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (!t->data || fread(t->data, 1, (size_t)n, f) != (size_t)n ||
        !stbtt_InitFont(&t->info, t->data, stbtt_GetFontOffsetForIndex(t->data, 0))) {
        fclose(f);
        free(t->data);
        free(t);
        errno = EINVAL;
        return NULL;
    }
    fclose(f);
    return t;
}

void gfx_ttf_free(gfx_ttf *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < CACHE_SIZE; i++) free(t->cache[i].bits);
    free(t->data);
    free(t);
}

static int utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int c = *p++, extra = 0;
    if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else c = '?';
        while (extra-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    }
    *pp = (const char *)p;
    return c;
}

static struct glyph_cache *get_glyph(gfx_ttf *t, int cp, float size)
{
    unsigned slot = ((unsigned)cp * 31u + (unsigned)(size * 4.0f)) % CACHE_SIZE;
    struct glyph_cache *g = &t->cache[slot];
    float scale;
    int adv, lsb;
    if (g->bits && g->cp == cp && g->size == size) return g;
    free(g->bits);
    memset(g, 0, sizeof(*g));
    scale = stbtt_ScaleForPixelHeight(&t->info, size);
    g->bits = stbtt_GetCodepointBitmap(&t->info, scale, scale, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    stbtt_GetCodepointHMetrics(&t->info, cp, &adv, &lsb);
    g->advance = (int)iround(adv * scale);
    g->cp = cp;
    g->size = size;
    if (!g->bits) {                  /* ফাঁকা অক্ষর (স্পেস) — শূন্য মাপের বিটম্যাপ */
        g->bits = (unsigned char *)malloc(1);
        g->w = g->h = 0;
    }
    return g;
}

int gfx_ttf_text(gfx_surface *s, gfx_ttf *t, float size, int x, int y, const char *str, uint32_t color)
{
    int ascent, descent, gap, baseline, x0 = x, prev = 0;
    float scale;
    if (!t || !str) return x;
    scale = stbtt_ScaleForPixelHeight(&t->info, size);
    stbtt_GetFontVMetrics(&t->info, &ascent, &descent, &gap);
    baseline = y + (int)iround(ascent * scale);
    while (*str) {
        int cp = utf8_next(&str), i, j;
        struct glyph_cache *g;
        if (cp == '\n') {
            x = x0;
            baseline += (int)iround((ascent - descent + gap) * scale);
            prev = 0;
            continue;
        }
        if (prev) x += (int)iround(stbtt_GetCodepointKernAdvance(&t->info, prev, cp) * scale);
        g = get_glyph(t, cp, size);
        for (j = 0; j < g->h; j++) {
            for (i = 0; i < g->w; i++) {
                unsigned a = g->bits[j * g->w + i];
                if (a) {
                    uint32_t ca = (uint32_t)(a * GFX_A(color) / 255);
                    gfx_blend_pixel(s, x + g->xoff + i, baseline + g->yoff + j,
                                    (color & 0x00FFFFFFu) | (ca << 24));
                }
            }
        }
        x += g->advance;
        prev = cp;
    }
    return x;
}

int gfx_ttf_width(gfx_ttf *t, float size, const char *str)
{
    int w = 0, best = 0, prev = 0;
    float scale;
    if (!t || !str) return 0;
    scale = stbtt_ScaleForPixelHeight(&t->info, size);
    while (*str) {
        int cp = utf8_next(&str), adv, lsb;
        if (cp == '\n') { if (w > best) best = w; w = 0; prev = 0; continue; }
        if (prev) w += (int)iround(stbtt_GetCodepointKernAdvance(&t->info, prev, cp) * scale);
        stbtt_GetCodepointHMetrics(&t->info, cp, &adv, &lsb);
        w += (int)iround(adv * scale);
        prev = cp;
    }
    return w > best ? w : best;
}
