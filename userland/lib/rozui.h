/* rozOS — rozui: 3D ধাঁচের UI (ধাপ ১৪)
 *
 * থিম: গাঢ় (ব্যবহারকারীর ওয়ালপেপারের রং — স্লেট #282C35, প্রায়-কালো বেগুনি #0D031B,
 * ধূসর রেখা #7F7F7F), বোতাম আর ফ্রেম উঁচু/বসানো 3D ধারে। পরে থিম বদলানো যাবে —
 * সব রং এক জায়গায় (struct ui_theme, ui_theme_get())।
 *
 * ধাপ ১৪ক-এ: ধার (bevel), প্যানেল, বোতাম, লেবেল, মেনুর সারি, ছোট আইকন — rozwm আর
 * rozterm-এর জন্য। ধাপ ১৪খ-এ লেখার ঘর, তালিকা, ডায়ালগ ইত্যাদি (Code, Files-এর জন্য)।
 * লিংক: -lrozui -lrozgfx */
#ifndef ROZUI_H
#define ROZUI_H

#include <rozgfx.h>

struct ui_theme {
    uint32_t desktop;        /* ডেস্কটপের পটভূমি */
    uint32_t panel;          /* ওপরের প্যানেল */
    uint32_t taskbar;        /* নিচের taskbar */
    uint32_t line;           /* প্যানেলের সরু রেখা */
    uint32_t face;           /* বোতাম/ফ্রেমের মুখ */
    uint32_t face_hover;
    uint32_t face_down;
    uint32_t light;          /* 3D-র আলো-পড়া ধার */
    uint32_t light2;         /* ভেতরের আলো ধার */
    uint32_t dark;           /* ছায়া ধার */
    uint32_t dark2;          /* ভেতরের ছায়া */
    uint32_t text;
    uint32_t text_dim;
    uint32_t accent;         /* নির্বাচিত জিনিস */
    uint32_t title_a1, title_a2;   /* সক্রিয় উইন্ডোর শিরোনাম (ঢাল) */
    uint32_t title_i1, title_i2;   /* নিষ্ক্রিয় */
    uint32_t close_hover;    /* [X]-এর ওপর মাউস */
    uint32_t window_bg;      /* উইন্ডোর ভেতরের ডিফল্ট */
};

const struct ui_theme *ui_theme_get(void);

enum { UI_NORMAL = 0, UI_HOVER, UI_DOWN, UI_DISABLED };

void ui_bevel(gfx_surface *s, int x, int y, int w, int h, int raised);      /* দুই স্তরের 3D ধার */
void ui_panel(gfx_surface *s, int x, int y, int w, int h, uint32_t face, int raised);
void ui_sunken(gfx_surface *s, int x, int y, int w, int h, uint32_t inside);
void ui_button(gfx_surface *s, int x, int y, int w, int h, const char *label, int state);
void ui_label(gfx_surface *s, int x, int y, const char *text, uint32_t color);
void ui_label_center(gfx_surface *s, int x, int y, int w, int h, const char *text, uint32_t color);
void ui_menu_item(gfx_surface *s, int x, int y, int w, int h, const char *label, int selected);
void ui_separator(gfx_surface *s, int x, int y, int w);
/* ছোট ছবির-চিহ্ন (16x16): 't' টার্মিনাল, 'g' গ্রাফিক্স, 'f' ফোল্ডার, 'p' প্রোগ্রাম,
 * 'x' বন্ধ, '_' ছোট করা, 'o' বড় করা, 'r' আগের মাপ, 's' সিস্টেম, 'n' নেটওয়ার্ক, 'e' এডিটর */
void ui_icon(gfx_surface *s, int x, int y, char kind, uint32_t color);

#endif
