/* rozOS — librozgfx-এর ভেতরের shaping স্তর (ধাপ ২৩খ, kb_text_shape); প্রোগ্রামের জন্য নয় */
#ifndef ROZGFX_SHAPE_H
#define ROZGFX_SHAPE_H

struct rozshape;

/* font: 0 মূল ফন্ট, 1 বাংলা ফন্ট; x/y পিক্সেলের 1/64 এককে (26.6), y ওপরের দিকে ধনাত্মক */
typedef void (*rozshape_cb)(void *ud, int font, int glyph, long x, long y, int codepoint);

struct rozshape *rozshape_open(const char *primary, const char *bengali);
void rozshape_close(struct rozshape *s);
int  rozshape_has_complex(const char *str, int len);           /* বাংলা আছে কিনা */
/* এক লাইন (\n ছাড়া) সাজানো; px = em মাপ, upem = দুই ফন্টের units per em;
 * ফেরত মোট advance (26.6) */
long rozshape_run(struct rozshape *s, const char *str, int len, int px, const int upem[2],
                  rozshape_cb cb, void *ud);

#endif
