/* rozOS — rozui (ধাপ ১৪), rozui.h দেখুন */

#include <string.h>
#include "rozui.h"

static const struct ui_theme dark3d = {
    GFX_RGB(40, 44, 53),      /* desktop — ওয়ালপেপার #282C35 */
    GFX_RGB(40, 44, 53),      /* panel */
    GFX_RGB(13, 3, 27),       /* taskbar — #0D031B */
    GFX_RGB(127, 127, 127),   /* line — #7F7F7F */
    GFX_RGB(52, 56, 68),      /* face */
    GFX_RGB(64, 69, 84),      /* face_hover */
    GFX_RGB(34, 37, 46),      /* face_down */
    GFX_RGB(112, 118, 136),   /* light */
    GFX_RGB(78, 83, 99),      /* light2 */
    GFX_RGB(8, 6, 14),        /* dark */
    GFX_RGB(26, 28, 36),      /* dark2 */
    GFX_RGB(222, 225, 232),   /* text */
    GFX_RGB(140, 145, 160),   /* text_dim */
    GFX_RGB(97, 175, 239),    /* accent */
    GFX_RGB(74, 52, 128),     /* title_a1 */
    GFX_RGB(32, 18, 62),      /* title_a2 */
    GFX_RGB(58, 62, 74),      /* title_i1 */
    GFX_RGB(42, 45, 55),      /* title_i2 */
    GFX_RGB(196, 43, 43),     /* close_hover */
    GFX_RGB(30, 33, 40),      /* window_bg */
};

const struct ui_theme *ui_theme_get(void) { return &dark3d; }

void ui_bevel(gfx_surface *s, int x, int y, int w, int h, int raised)
{
    const struct ui_theme *t = &dark3d;
    uint32_t tl = raised ? t->light : t->dark, br = raised ? t->dark : t->light;
    uint32_t tl2 = raised ? t->light2 : t->dark2, br2 = raised ? t->dark2 : t->light2;
    if (w < 2 || h < 2) return;
    gfx_hline(s, x, y, w - 1, tl);
    gfx_vline(s, x, y, h - 1, tl);
    gfx_hline(s, x, y + h - 1, w, br);
    gfx_vline(s, x + w - 1, y, h, br);
    if (w > 4 && h > 4) {
        gfx_hline(s, x + 1, y + 1, w - 3, tl2);
        gfx_vline(s, x + 1, y + 1, h - 3, tl2);
        gfx_hline(s, x + 1, y + h - 2, w - 2, br2);
        gfx_vline(s, x + w - 2, y + 1, h - 2, br2);
    }
}

void ui_panel(gfx_surface *s, int x, int y, int w, int h, uint32_t face, int raised)
{
    gfx_fill_rect(s, x, y, w, h, face);
    ui_bevel(s, x, y, w, h, raised);
}

void ui_sunken(gfx_surface *s, int x, int y, int w, int h, uint32_t inside)
{
    gfx_fill_rect(s, x, y, w, h, inside);
    ui_bevel(s, x, y, w, h, 0);
}

void ui_label(gfx_surface *s, int x, int y, const char *text, uint32_t color)
{
    gfx_text(s, NULL, x, y, text, color);
}

void ui_label_center(gfx_surface *s, int x, int y, int w, int h, const char *text, uint32_t color)
{
    int tw = gfx_text_width(NULL, text), th = gfx_font_height(gfx_font_default());
    gfx_text(s, NULL, x + (w - tw) / 2, y + (h - th) / 2, text, color);
}

void ui_button(gfx_surface *s, int x, int y, int w, int h, const char *label, int state)
{
    const struct ui_theme *t = &dark3d;
    uint32_t face = state == UI_DOWN ? t->face_down : state == UI_HOVER ? t->face_hover : t->face;
    int off = state == UI_DOWN ? 1 : 0;
    ui_panel(s, x, y, w, h, face, state != UI_DOWN);
    if (label) ui_label_center(s, x + off, y + off, w, h, label,
                               state == UI_DISABLED ? t->text_dim : t->text);
}

void ui_menu_item(gfx_surface *s, int x, int y, int w, int h, const char *label, int selected)
{
    const struct ui_theme *t = &dark3d;
    if (selected) gfx_fill_rect(s, x, y, w, h, t->title_a1);
    gfx_text(s, NULL, x + 8, y + (h - 16) / 2, label, selected ? GFX_WHITE : t->text);
}

void ui_separator(gfx_surface *s, int x, int y, int w)
{
    gfx_hline(s, x, y, w, dark3d.dark);
    gfx_hline(s, x, y + 1, w, dark3d.light2);
}

void ui_icon(gfx_surface *s, int x, int y, char k, uint32_t c)
{
    const struct ui_theme *t = &dark3d;
    switch (k) {
    case 't':                                    /* টার্মিনাল: কালো পর্দা, সবুজ প্রম্পট */
        gfx_fill_rect(s, x, y + 1, 16, 14, GFX_RGB(10, 10, 14));
        gfx_rect(s, x, y + 1, 16, 14, t->light);
        gfx_line(s, x + 3, y + 5, x + 6, y + 8, GFX_RGB(80, 220, 120));
        gfx_line(s, x + 6, y + 8, x + 3, y + 11, GFX_RGB(80, 220, 120));
        gfx_hline(s, x + 8, y + 11, 5, GFX_RGB(200, 200, 200));
        break;
    case 'g':                                    /* গ্রাফিক্স: রঙিন বৃত্ত-ত্রিভুজ */
        gfx_fill_circle(s, x + 6, y + 6, 5, GFX_RGB(240, 180, 40));
        gfx_fill_triangle(s, x + 6, y + 15, x + 11, y + 6, x + 15, y + 15, GFX_RGB(60, 190, 110));
        break;
    case 'f':                                    /* ফোল্ডার */
        gfx_fill_rect(s, x + 1, y + 3, 6, 3, GFX_RGB(230, 180, 70));
        gfx_fill_rect(s, x + 1, y + 5, 14, 9, GFX_RGB(240, 196, 90));
        break;
    case 'e':                                    /* এডিটর: কাগজ আর লাইন */
        gfx_fill_rect(s, x + 2, y + 1, 12, 14, GFX_RGB(235, 235, 240));
        gfx_hline(s, x + 4, y + 4, 8, GFX_RGB(80, 120, 200));
        gfx_hline(s, x + 4, y + 7, 6, GFX_RGB(120, 120, 130));
        gfx_hline(s, x + 4, y + 10, 8, GFX_RGB(120, 120, 130));
        break;
    case 's':                                    /* সিস্টেম: গিয়ারের মতো */
        gfx_fill_circle(s, x + 8, y + 8, 6, c);
        gfx_fill_circle(s, x + 8, y + 8, 2, t->dark);
        break;
    case 'n':                                    /* নেটওয়ার্ক: দুই বাক্স */
        gfx_fill_rect(s, x + 1, y + 2, 6, 5, c);
        gfx_fill_rect(s, x + 9, y + 9, 6, 5, c);
        gfx_line(s, x + 4, y + 7, x + 12, y + 9, c);
        break;
    case 'x':                                    /* বন্ধ */
        gfx_thick_line(s, x + 4, y + 4, x + 11, y + 11, 2, c);
        gfx_thick_line(s, x + 11, y + 4, x + 4, y + 11, 2, c);
        break;
    case '_':
        gfx_fill_rect(s, x + 4, y + 11, 8, 2, c);
        break;
    case 'o':
        gfx_rect(s, x + 3, y + 3, 10, 10, c);
        gfx_hline(s, x + 3, y + 4, 10, c);
        break;
    case 'r':
        gfx_rect(s, x + 5, y + 2, 8, 8, c);
        gfx_fill_rect(s, x + 2, y + 6, 8, 8, dark3d.face);
        gfx_rect(s, x + 2, y + 6, 8, 8, c);
        break;
    default:                                     /* প্রোগ্রাম: ছোট উইন্ডো */
        gfx_fill_rect(s, x + 1, y + 2, 14, 12, t->window_bg);
        gfx_fill_rect(s, x + 1, y + 2, 14, 3, t->title_a1);
        gfx_rect(s, x + 1, y + 2, 14, 12, t->light);
        break;
    }
}
