/* rozOS — librozgfx: জটিল লিপির shaping (ধাপ ২৩খ), kb_text_shape দিয়ে
 *
 * বাংলা লেখা শুধু অক্ষর পাশাপাশি বসালে হয় না: ি-কার আগে যায়, রেফ (র্) ওপরে ওঠে,
 * ক্ষ/ন্ত-র মতো যুক্তাক্ষর একটা glyph হয়। kb_text_shape ফন্টের OpenType নিয়ম (GSUB/GPOS)
 * পড়ে সেটা করে। দুটো ফন্ট একসাথে: মূল ফন্ট (Inter ইত্যাদি) আর বাংলা ফন্ট
 * (Noto Sans Bengali) — যে অক্ষর মূল ফন্টে নেই, সেটা বাংলা ফন্ট থেকে।
 * rozgfx_ft.c শুধু তখনই এটা ডাকে যখন লেখায় বাংলা (U+0980–U+09FF) আছে।
 * ফলাফল callback-এ: কোন ফন্ট (0 মূল, 1 বাংলা), glyph নম্বর, x/y (ফন্ট-একক)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rozgfx_shape.h"

#define KB_TEXT_SHAPE_IMPLEMENTATION
#define KB_TEXT_SHAPE_STATIC
#include "kb_text_shape.h"

/* gcc __builtin_popcount → __popcountdi2 (libgcc-এ থাকে, rozOS-এর tcc-র কাছে নেই) — তাই এখানেই,
 * যাতে rozOS-এর ভেতরে tcc দিয়ে -lrozgfx লিংক হয় */
int __popcountdi2(unsigned long long x);
int __popcountdi2(unsigned long long x)
{
    int n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}

struct rozshape {
    kbts_shape_context *ctx;
    kbts_font *font[2];
};

struct rozshape *rozshape_open(const char *primary, const char *bengali)
{
    struct rozshape *s = (struct rozshape *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx = kbts_CreateShapeContext(0, 0);
    if (!s->ctx) { free(s); return NULL; }
    /* স্ট্যাকে আগে fallback (বাংলা), শেষে মূল — শেষেরটার অগ্রাধিকার বেশি */
    if (bengali) s->font[1] = kbts_ShapePushFontFromFile(s->ctx, bengali, 0);
    if (primary) s->font[0] = kbts_ShapePushFontFromFile(s->ctx, primary, 0);
    if (!s->font[0] && !s->font[1]) {
        kbts_DestroyShapeContext(s->ctx);
        free(s);
        return NULL;
    }
    return s;
}

void rozshape_close(struct rozshape *s)
{
    if (!s) return;
    kbts_DestroyShapeContext(s->ctx);
    free(s);
}

int rozshape_has_complex(const char *str, int len)
{
    const unsigned char *p = (const unsigned char *)str;
    int i;
    /* বাংলা ব্লক U+0980–U+09FF-এর UTF-8: E0 A6 xx বা E0 A7 xx */
    for (i = 0; i + 1 < len; i++)
        if (p[i] == 0xE0 && (p[i + 1] == 0xA6 || p[i + 1] == 0xA7)) return 1;
    return 0;
}

long rozshape_run(struct rozshape *s, const char *str, int len, int px, const int upem[2],
                  rozshape_cb cb, void *ud)
{
    kbts_run run;
    long x = 0;
    if (!s || len <= 0) return 0;
    kbts_ShapeBegin(s->ctx, KBTS_DIRECTION_DONT_KNOW, KBTS_LANGUAGE_DONT_KNOW);
    kbts_ShapeUtf8(s->ctx, str, len, KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
    kbts_ShapeEnd(s->ctx);
    while (kbts_ShapeRun(s->ctx, &run)) {
        kbts_glyph *g;
        int which = run.Font == s->font[0] ? 0 : 1;
        long scale = upem[which] > 0 ? upem[which] : 1000;      /* ফন্ট-একক → 26.6 পিক্সেল */
        while (kbts_GlyphIteratorNext(&run.Glyphs, &g)) {
            if (cb) cb(ud, which, g->Id, x + (long)g->OffsetX * px * 64 / scale,
                       (long)g->OffsetY * px * 64 / scale, (int)g->Codepoint);
            x += (long)g->AdvanceX * px * 64 / scale;
        }
    }
    return x;
}
