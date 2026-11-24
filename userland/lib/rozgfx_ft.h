/* rozOS — librozgfx-এর ভেতরের FreeType স্তর (ধাপ ২৩ক); প্রোগ্রামের জন্য নয় —
 * প্রোগ্রাম rozgfx.h-এর gfx_font_ui / gfx_font_mono / gfx_font_load_ttf ব্যবহার করে। */
#ifndef ROZGFX_FT_H
#define ROZGFX_FT_H

#include <stddef.h>
#include <stdint.h>

struct rozft;
struct gfx_surface;

int  rozft_available(void);
struct rozft *rozft_open(const char *path, int px, int mono);    /* px = em মাপ */
void rozft_close(struct rozft *f);
int  rozft_cell_w(const struct rozft *f);
int  rozft_line_h(const struct rozft *f);
int  rozft_px(const struct rozft *f);
/* y = box_h-উঁচু লাইনের ওপরের ধার; ফেরত শেষের x */
int  rozft_text(struct rozft *f, struct gfx_surface *s, int x, int y, int box_h, const char *str,
                uint32_t fg, uint32_t bg, int with_bg);
int  rozft_width(struct rozft *f, const char *str);
void rozft_cell(struct rozft *f, struct gfx_surface *s, int x, int y, int cell_h, int index,
                uint32_t fg, uint32_t bg);
/* /etc/rozos.conf-এর ui_font/ui_font_size (mono=0) বা mono_font/mono_font_size (mono=1) */
void rozft_system_font(int mono, char *path, size_t size, int *px);

#endif
