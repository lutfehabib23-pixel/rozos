/* rozOS — rozui (ধাপ ১৪, ১৪খ), rozui.h দেখুন */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include "rozui.h"

/* ধাপ ১৪খ: ব্যবহারকারীর থিম — Ebony, Obsidian, Black, Dark Charcoal, Onyx, Jet Black,
 * White, Grey (আগের ধাপ ১৪ক-এর রং ছিল ওয়ালপেপার থেকে: #282C35 / #0D031B) */
static const struct ui_theme dark3d = {
    GFX_RGB(11, 18, 21),      /* desktop — Obsidian #0B1215 */
    GFX_RGB(0, 0, 0),         /* panel — Black */
    GFX_RGB(0, 0, 0),         /* taskbar — Black */
    GFX_RGB(128, 128, 128),   /* line — Grey #808080 */
    GFX_RGB(53, 56, 57),      /* face — Onyx #353839 */
    GFX_RGB(68, 72, 73),      /* face_hover */
    GFX_RGB(40, 42, 43),      /* face_down */
    GFX_RGB(128, 128, 128),   /* light — Grey */
    GFX_RGB(82, 86, 87),      /* light2 */
    GFX_RGB(0, 0, 0),         /* dark — Black */
    GFX_RGB(30, 32, 33),      /* dark2 */
    GFX_RGB(255, 255, 255),   /* text — White */
    GFX_RGB(150, 150, 150),   /* text_dim — Grey */
    GFX_RGB(164, 176, 152),   /* accent — হালকা Ebony */
    GFX_RGB(85, 93, 80),      /* title_a1 — Ebony #555D50 */
    GFX_RGB(42, 46, 40),      /* title_a2 — গাঢ় Ebony */
    GFX_RGB(52, 52, 52),      /* title_i1 — Jet Black #343434 */
    GFX_RGB(34, 34, 34),      /* title_i2 */
    GFX_RGB(196, 43, 43),     /* close_hover */
    GFX_RGB(51, 51, 51),      /* window_bg — Dark Charcoal #333333 */
    GFX_RGB(85, 93, 80),      /* select — Ebony */
    GFX_RGB(11, 18, 21),      /* field — Obsidian */
    GFX_RGB(240, 240, 240),   /* field_text */
    GFX_RGB(52, 52, 52),      /* menubar — Jet Black */
};

const struct ui_theme *ui_theme_get(void) { return &dark3d; }

#define FW 8                     /* ডিফল্ট ফন্টের ঘর */
#define FH 16

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
    if (selected) gfx_fill_rect(s, x, y, w, h, t->select);
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
    /* ধাপ ১৪খ */
    case 'd':                                    /* সাধারণ ফাইল: ভাঁজ করা কোণের কাগজ */
    case 'c':                                    /* C সোর্স: কাগজে নীল "C" */
        gfx_fill_rect(s, x + 3, y + 1, 8, 14, GFX_RGB(230, 230, 232));
        gfx_fill_rect(s, x + 11, y + 5, 3, 10, GFX_RGB(230, 230, 232));
        gfx_fill_triangle(s, x + 11, y + 1, x + 11, y + 5, x + 14, y + 5, GFX_RGB(170, 170, 176));
        if (k == 'c') gfx_text(s, NULL, x + 5, y, "C", GFX_RGB(40, 90, 200));
        else { gfx_hline(s, x + 5, y + 7, 7, GFX_RGB(140, 140, 150)); gfx_hline(s, x + 5, y + 10, 7, GFX_RGB(140, 140, 150)); }
        break;
    case 'i':                                    /* ছবি: ফ্রেমে পাহাড় আর সূর্য */
        gfx_fill_rect(s, x + 1, y + 2, 14, 12, GFX_RGB(70, 120, 180));
        gfx_rect(s, x + 1, y + 2, 14, 12, GFX_RGB(220, 220, 220));
        gfx_fill_circle(s, x + 11, y + 6, 2, GFX_RGB(250, 210, 60));
        gfx_fill_triangle(s, x + 2, y + 13, x + 7, y + 7, x + 12, y + 13, GFX_RGB(60, 160, 80));
        break;
    case 'u':                                    /* ওপরে ↑ */
        gfx_fill_triangle(s, x + 8, y + 2, x + 2, y + 9, x + 14, y + 9, c);
        gfx_fill_rect(s, x + 6, y + 9, 4, 5, c);
        break;
    case 'b':                                    /* পেছনে ← */
        gfx_fill_triangle(s, x + 2, y + 8, x + 9, y + 2, x + 9, y + 14, c);
        gfx_fill_rect(s, x + 9, y + 6, 5, 4, c);
        break;
    case 'h':                                    /* বাড়ি */
        gfx_fill_triangle(s, x + 8, y + 1, x + 1, y + 8, x + 15, y + 8, c);
        gfx_fill_rect(s, x + 3, y + 8, 10, 7, c);
        gfx_fill_rect(s, x + 7, y + 10, 3, 5, t->face);
        break;
    case 'l':                                    /* ধাপ ১৯ছ: লক করা ফোল্ডার — ফোল্ডার + তালা */
        gfx_fill_rect(s, x + 1, y + 3, 6, 3, GFX_RGB(230, 180, 70));
        gfx_fill_rect(s, x + 1, y + 5, 14, 9, GFX_RGB(240, 196, 90));
        gfx_circle(s, x + 11, y + 7, 2, GFX_RGB(60, 60, 60));
        gfx_fill_rect(s, x + 8, y + 8, 7, 6, GFX_RGB(60, 60, 60));
        gfx_fill_rect(s, x + 11, y + 10, 1, 2, GFX_RGB(240, 196, 90));
        break;
    case 'k':                                    /* নতুন ফোল্ডার: ফোল্ডার + সবুজ যোগ */
        gfx_fill_rect(s, x + 1, y + 3, 6, 3, GFX_RGB(230, 180, 70));
        gfx_fill_rect(s, x + 1, y + 5, 14, 9, GFX_RGB(240, 196, 90));
        gfx_fill_rect(s, x + 9, y + 7, 6, 2, GFX_RGB(40, 150, 60));
        gfx_fill_rect(s, x + 11, y + 5, 2, 6, GFX_RGB(40, 150, 60));
        break;
    case 'm':                                    /* মুছুন: লাল ঝুড়ি */
        gfx_fill_rect(s, x + 3, y + 3, 10, 2, GFX_RGB(210, 70, 70));
        gfx_fill_rect(s, x + 6, y + 1, 4, 2, GFX_RGB(210, 70, 70));
        gfx_fill_rect(s, x + 4, y + 6, 8, 9, GFX_RGB(190, 60, 60));
        gfx_vline(s, x + 6, y + 7, 7, GFX_RGB(240, 150, 150));
        gfx_vline(s, x + 9, y + 7, 7, GFX_RGB(240, 150, 150));
        break;
    case 'v':                                    /* সেভ: ফ্লপি */
        gfx_fill_rect(s, x + 2, y + 2, 12, 12, GFX_RGB(70, 110, 190));
        gfx_fill_rect(s, x + 5, y + 2, 6, 4, GFX_RGB(220, 220, 225));
        gfx_fill_rect(s, x + 4, y + 9, 8, 5, GFX_RGB(235, 235, 240));
        break;
    case 'y':                                    /* চালাও ▶ */
        gfx_fill_triangle(s, x + 4, y + 2, x + 4, y + 14, x + 14, y + 8, GFX_RGB(80, 200, 110));
        break;
    case 'w':                                    /* কম্পাইল: হাতুড়ি-গিয়ার */
        gfx_fill_circle(s, x + 8, y + 8, 6, GFX_RGB(200, 170, 80));
        gfx_fill_circle(s, x + 8, y + 8, 3, t->face);
        gfx_fill_rect(s, x + 7, y + 1, 2, 3, GFX_RGB(200, 170, 80));
        gfx_fill_rect(s, x + 7, y + 12, 2, 3, GFX_RGB(200, 170, 80));
        break;
    default:                                     /* প্রোগ্রাম: ছোট উইন্ডো */
        gfx_fill_rect(s, x + 1, y + 2, 14, 12, t->window_bg);
        gfx_fill_rect(s, x + 1, y + 2, 14, 3, t->title_a1);
        gfx_rect(s, x + 1, y + 2, 14, 12, t->light);
        break;
    }
}

/* ---------- ধাপ ১৪খ: ছবি-আইকন ---------- */
#define ICON_CACHE 48
static struct { char name[96]; int size; gfx_surface *img; } icache[ICON_CACHE];
static int icache_n;

static gfx_surface *icon_try(const char *path, int size)
{
    gfx_surface *img = gfx_load_image(path), *sc;
    int i, opaque = 0;
    if (!img) return NULL;
    for (i = 0; i < img->w * img->h && !opaque; i++)
        if (GFX_A(img->px[(i / img->w) * img->stride + i % img->w])) opaque = 1;
    if (!opaque) {                               /* BMP-এর মতো alpha ছাড়া ছবি = পুরো অস্বচ্ছ */
        int x, y;
        for (y = 0; y < img->h; y++)
            for (x = 0; x < img->w; x++) img->px[y * img->stride + x] |= 0xFF000000u;
    }
    sc = gfx_surface_new(size, size);
    if (sc) gfx_blit_scaled(sc, 0, 0, size, size, img);
    gfx_surface_free(img);
    return sc;
}

gfx_surface *ui_icon_image(const char *image, int size)
{
    static const char *const ext[] = { ".png", ".bmp", ".jpg", ".gif", ".tga", ".ppm" };
    gfx_surface *img = NULL;
    int i;
    if (!image || !image[0] || size <= 0) return NULL;
    for (i = 0; i < icache_n; i++)
        if (icache[i].size == size && !strcmp(icache[i].name, image)) return icache[i].img;
    if (image[0] == '/') {
        img = icon_try(image, size);
    } else {
        char path[160];
        for (i = 0; i < 6 && !img; i++) {
            snprintf(path, sizeof(path), "/usr/share/icons/%s%s", image, ext[i]);
            if (access(path, R_OK) == 0) img = icon_try(path, size);
        }
    }
    if (icache_n < ICON_CACHE) {                 /* না পাওয়াও মনে রাখা — প্রতিবার ডিস্ক খোঁজা নয় */
        strncpy(icache[icache_n].name, image, sizeof(icache[0].name) - 1);
        icache[icache_n].size = size;
        icache[icache_n].img = img;
        icache_n++;
    }
    return img;
}

void ui_draw_icon(gfx_surface *s, int x, int y, int size, const char *image, char kind)
{
    gfx_surface *img = ui_icon_image(image, size);
    if (img) {
        gfx_blit_alpha(s, x, y, img, 0, 0, -1, -1);
        return;
    }
    if (size >= 24) {
        ui_panel(s, x, y, size, size, dark3d.face, 1);
        ui_icon(s, x + (size - 16) / 2, y + (size - 16) / 2, kind, dark3d.text);
    } else {
        ui_icon(s, x + (size - 16) / 2, y + (size - 16) / 2, kind, dark3d.text);
    }
}

void ui_progress(gfx_surface *s, int x, int y, int w, int h, int percent)
{
    char t[8];
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ui_sunken(s, x, y, w, h, dark3d.field);
    gfx_gradient_v(s, x + 2, y + 2, (w - 4) * percent / 100, h - 4, dark3d.accent, dark3d.title_a1);
    snprintf(t, sizeof(t), "%d%%", percent);
    ui_label_center(s, x, y, w, h, t, GFX_WHITE);
}

void ui_groupbox(gfx_surface *s, int x, int y, int w, int h, const char *title)
{
    int tw = title ? gfx_text_width(NULL, title) : 0;
    /* খোদাই করা রেখা: অন্ধকার + আলো */
    gfx_rect(s, x, y + 8, w - 1, h - 9, dark3d.dark);
    gfx_rect(s, x + 1, y + 9, w - 1, h - 9, dark3d.light2);
    if (title) {
        gfx_fill_rect(s, x + 8, y, tw + 8, FH, dark3d.window_bg);
        gfx_text(s, NULL, x + 12, y, title, dark3d.text);
    }
}

void ui_statusbar(gfx_surface *s, int x, int y, int w, int h, const char *text)
{
    ui_panel(s, x, y, w, h, dark3d.face, 1);
    ui_sunken(s, x + 2, y + 2, w - 4, h - 4, dark3d.face);
    if (text) {
        gfx_set_clip(s, x + 4, y, w - 8, h);
        gfx_text(s, NULL, x + 8, y + (h - FH) / 2, text, dark3d.text);
        gfx_reset_clip(s);
    }
}

/* ================= ধাপ ১৪খ: উইজেট ================= */

static gfx_surface *last_frame;          /* শেষ দেখানো ছবি — ডায়ালগের পেছনে */
static int force_frame;                  /* ডায়ালগ বন্ধ হল — অ্যাপ আবার আঁকুক */
static int close_flag;                   /* ডায়ালগ চলার সময় [X] চাপা হয়েছিল */

void ui_init(ui_ctx *u)
{
    memset(u, 0, sizeof(*u));
    u->s = gfx_screen();
    u->w = gfx_width();
    u->h = gfx_height();
    u->blink = 1;
    u->now = u->blink_at = gfx_ticks_ms();
    u->mx = u->my = -1;
}

void ui_quit(ui_ctx *u) { u->quit = 1; }
void ui_redraw(ui_ctx *u) { u->need_frame = 1; }
void ui_ask_before_close(ui_ctx *u, int on) { u->ask_close = on; }
void ui_focus_next(ui_ctx *u) { u->want_focus = 1; u->focus = 0; }

static void apply_event(ui_ctx *u, const gfx_event *ev)
{
    switch (ev->type) {
    case GFX_EV_KEY:
        u->key = ev->key;
        u->mods = ev->mods;
        u->blink = 1;
        u->blink_at = u->now;
        u->had_input = 1;
        break;
    case GFX_EV_MOUSE_MOVE:
        u->mx = ev->x; u->my = ev->y; u->buttons = ev->buttons;
        break;
    case GFX_EV_MOUSE_DOWN:
        u->mx = ev->x; u->my = ev->y; u->buttons = ev->buttons;
        if (ev->button == GFX_BUTTON_LEFT) {
            u->pressed = 1;
            if (u->now - u->last_click < 450 && abs(ev->x - u->last_cx) < 5 && abs(ev->y - u->last_cy) < 5) {
                u->dclick = 1;
                u->last_click = 0;
            } else {
                u->last_click = u->now;
                u->last_cx = ev->x;
                u->last_cy = ev->y;
            }
        } else if (ev->button == GFX_BUTTON_RIGHT) {
            u->rpressed = 1;
        }
        u->blink = 1;
        u->blink_at = u->now;
        u->had_input = 1;
        break;
    case GFX_EV_MOUSE_UP:
        u->mx = ev->x; u->my = ev->y; u->buttons = ev->buttons;
        if (ev->button == GFX_BUTTON_LEFT) u->released = 1;
        u->had_input = 1;
        break;
    case GFX_EV_MOUSE_WHEEL:
        u->wheel = ev->wheel;
        u->had_input = 1;
        break;
    case GFX_EV_CLOSE:
        if (u->ask_close) {
            gfx_keep_open();
            u->closing = 1;
            u->had_input = 1;
        } else {
            u->quit = 1;
        }
        break;
    case GFX_EV_RESIZE:
        u->resized = 1;
        u->had_input = 1;
        break;
    default:
        break;
    }
}

static void begin_frame(ui_ctx *u, const gfx_event *ev)
{
    u->s = gfx_screen();
    u->w = gfx_width();
    u->h = gfx_height();
    u->now = gfx_ticks_ms();
    u->pressed = u->released = u->rpressed = u->dclick = u->wheel = 0;
    u->key = u->mods = 0;
    u->closing = u->resized = 0;
    u->had_input = 0;
    u->id = 0;
    u->hot = 0;
    u->block = 0;
    u->focus_claimed = 0;
    u->auto_tick = 0;
    if (u->now - u->blink_at >= 500) {
        u->blink = !u->blink;
        u->blink_at = u->now;
    }
    if (close_flag) {                            /* ডায়ালগের সময় [X] */
        gfx_event c;
        close_flag = 0;
        memset(&c, 0, sizeof(c));
        c.type = GFX_EV_CLOSE;
        apply_event(u, &c);
    }
    if (ev) apply_event(u, ev);
}

static void draw_menu_overlay(ui_ctx *u);

static void end_frame(ui_ctx *u)
{
    draw_menu_overlay(u);
    if (u->pressed && !u->focus_claimed) u->focus = 0;      /* অন্য কোথাও ক্লিক = ফোকাস গেল */
    if (!(u->buttons & GFX_BUTTON_LEFT)) u->active = 0;
    /* ডায়ালগের পেছনে দেখানোর জন্য শেষ ছবিটা রাখা */
    if (!last_frame || last_frame->w != u->w || last_frame->h != u->h) {
        if (last_frame) gfx_surface_free(last_frame);
        last_frame = gfx_surface_new(u->w, u->h);
    }
    if (last_frame) gfx_blit(last_frame, 0, 0, u->s, 0, 0, u->w, u->h);
    gfx_present();
    if (u->had_input) u->need_frame = 1;         /* ক্লিক/কী-র পরে আরেকবার — নতুন অবস্থা দেখাতে */
}

int ui_run(ui_ctx *u)
{
    gfx_event ev;
    int got = 0;
    if (!u->started) {
        u->started = 1;
        begin_frame(u, NULL);
        return !u->quit;
    }
    end_frame(u);
    if (u->quit) return 0;
    if (u->has_pending) {
        ev = u->pending;
        u->has_pending = 0;
        got = 1;
    } else {
        int timeout = -1;
        if (u->need_frame || force_frame) timeout = 0;
        else {
            if (u->tick_ms > 0) timeout = u->tick_ms;
            if (u->auto_tick) {
                int left = 500 - (int)(gfx_ticks_ms() - u->blink_at);
                if (left < 1) left = 1;
                if (timeout < 0 || left < timeout) timeout = left;
            }
        }
        got = gfx_poll_event(&ev, timeout);
    }
    u->need_frame = 0;
    force_frame = 0;
    if (got && ev.type == GFX_EV_MOUSE_MOVE) {   /* অনেক নড়াচড়া একসাথে = একটা ফ্রেম */
        gfx_event nx;
        while (gfx_poll_event(&nx, 0)) {
            if (nx.type == GFX_EV_MOUSE_MOVE) { ev = nx; continue; }
            u->pending = nx;
            u->has_pending = 1;
            break;
        }
    }
    begin_frame(u, got ? &ev : NULL);
    return !u->quit;
}

int ui_hover(ui_ctx *u, int x, int y, int w, int h)
{
    return !u->block && u->mx >= x && u->mx < x + w && u->my >= y && u->my < y + h;
}

int ui_clicked(ui_ctx *u, int x, int y, int w, int h)
{
    return u->pressed && ui_hover(u, x, y, w, h);
}

/* সাধারণ বোতাম-আচরণ: চাপা + ভেতরে ছাড়া = ক্লিক */
static int button_logic(ui_ctx *u, int id, int x, int y, int w, int h, int *state)
{
    int over = ui_hover(u, x, y, w, h), click = 0;
    if (over) u->hot = id;
    if (over && u->pressed) u->active = id;
    if (u->active == id && u->released) {
        if (over) click = 1;
        u->active = 0;
    }
    *state = (u->active == id && over) ? UI_DOWN : over ? UI_HOVER : UI_NORMAL;
    return click;
}

int ui_do_button(ui_ctx *u, int x, int y, int w, int h, const char *label)
{
    int id = ++u->id, st, c = button_logic(u, id, x, y, w, h, &st);
    ui_button(u->s, x, y, w, h, label, st);
    return c;
}

int ui_do_tool(ui_ctx *u, int x, int y, int w, int h, const char *image, char kind, const char *label)
{
    int id = ++u->id, st, c = button_logic(u, id, x, y, w, h, &st), off = st == UI_DOWN;
    const struct ui_theme *t = &dark3d;
    if (st == UI_NORMAL) gfx_fill_rect(u->s, x, y, w, h, t->face);     /* টুলবারে সমতল, ছুঁলে 3D */
    else ui_button(u->s, x, y, w, h, NULL, st);
    if (label) {
        ui_draw_icon(u->s, x + 5 + off, y + (h - 16) / 2 + off, 16, image, kind);
        gfx_text(u->s, NULL, x + 25 + off, y + (h - FH) / 2 + off, label, t->text);
    } else {
        ui_draw_icon(u->s, x + (w - 16) / 2 + off, y + (h - 16) / 2 + off, 16, image, kind);
    }
    return c;
}

int ui_do_check(ui_ctx *u, int x, int y, const char *label, int *value)
{
    int id = ++u->id, st, w = 20 + (label ? gfx_text_width(NULL, label) + 6 : 0);
    int c = button_logic(u, id, x, y, w, 18, &st);
    ui_sunken(u->s, x, y + 1, 16, 16, st == UI_HOVER ? dark3d.face_hover : dark3d.field);
    if (*value) {
        gfx_thick_line(u->s, x + 4, y + 9, x + 7, y + 12, 2, dark3d.accent);
        gfx_thick_line(u->s, x + 7, y + 12, x + 12, y + 5, 2, dark3d.accent);
    }
    if (label) gfx_text(u->s, NULL, x + 22, y + 1, label, dark3d.text);
    if (c) *value = !*value;
    return c;
}

int ui_do_radio(ui_ctx *u, int x, int y, const char *label, int *value, int mine)
{
    int id = ++u->id, st, w = 20 + (label ? gfx_text_width(NULL, label) + 6 : 0);
    int c = button_logic(u, id, x, y, w, 18, &st);
    gfx_fill_circle(u->s, x + 8, y + 9, 7, dark3d.light);
    gfx_fill_circle(u->s, x + 8, y + 9, 6, st == UI_HOVER ? dark3d.face_hover : dark3d.field);
    if (*value == mine) gfx_fill_circle(u->s, x + 8, y + 9, 3, dark3d.accent);
    if (label) gfx_text(u->s, NULL, x + 22, y + 1, label, dark3d.text);
    if (c && *value != mine) { *value = mine; return 1; }
    return 0;
}

/* ফোকাস নেওয়া (ক্লিক বা Tab) */
static int take_focus(ui_ctx *u, int id, int over)
{
    if (over && u->pressed && u->focus != id) {
        u->focus = id;
        u->focus_claimed = 1;
        return 1;
    }
    if (u->want_focus && u->focus == 0) {
        u->want_focus = 0;
        u->focus = id;
        return 1;
    }
    if (u->focus == id && u->pressed && over) u->focus_claimed = 1;
    return 0;
}

static int pw_mask;                      /* ধাপ ১৯ছ: ui_do_password চলার সময় 1 */
static gfx_font *list_font;              /* ধাপ ২৩ক: ui_do_list-এর ফন্ট (NULL = UI ফন্ট) */

void ui_list_font(gfx_font *f) { list_font = f; }

/* ধাপ ২৩ক: UI ফন্ট আনুপাতিক — লেখার প্রথম n বাইটের পিক্সেল চওড়া (ঘরের লেখা ASCII) */
static int tw_n(const char *str, int n)
{
    char tmp[1024];
    if (n <= 0) return 0;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    memcpy(tmp, str, (size_t)n);
    tmp[n] = '\0';
    return gfx_text_width(NULL, tmp);
}

/* পিক্সেল px-এর সবচেয়ে কাছের অক্ষর-সীমা (from থেকে গুনে) */
static int tw_hit(const char *str, int from, int len, int px)
{
    int i, best = from, bd = 1 << 30;
    for (i = from; i <= len; i++) {
        int d = tw_n(str + from, i - from) - px;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
        else break;
    }
    return best;
}

int ui_do_textbox(ui_ctx *u, int x, int y, int w, char *buf, int size)
{
    int id = ++u->id, h = UI_TEXTBOX_H, over = ui_hover(u, x, y, w, h), len = (int)strlen(buf);
    int ret = 0, cur;
    const struct ui_theme *t = &dark3d;
    char stars[256];
    const char *disp = buf;                      /* ধাপ ২৩ক: যা দেখানো হয় (password হলে *) */
    if (pw_mask) {
        int i, n = len < (int)sizeof(stars) - 1 ? len : (int)sizeof(stars) - 1;
        for (i = 0; i < n; i++) stars[i] = '*';
        stars[n] = '\0';
        disp = stars;
    }
    if (take_focus(u, id, over)) {
        u->tb_cur = len;
        u->tb_off = 0;
        if (over && u->pressed)                  /* ক্লিকের জায়গায় কার্সর */
            u->tb_cur = tw_hit(disp, 0, len, u->mx - x - 5);
    } else if (u->focus == id && over && u->pressed) {
        u->tb_cur = tw_hit(disp, u->tb_off, len, u->mx - x - 5);
        u->focus_claimed = 1;
    }
    if (u->focus == id) {
        u->focus_claimed |= u->pressed && over;
        if (u->tb_cur > len) u->tb_cur = len;
        cur = u->tb_cur;
        if (u->key && !u->block) {
            int k = u->key;
            u->key = 0;
            if (k == '\n') ret = 2;
            else if (k == '\t') ui_focus_next(u);
            else if (k == '\b') {
                if (cur > 0) { memmove(buf + cur - 1, buf + cur, (size_t)(len - cur + 1)); cur--; ret = 1; }
            } else if (k == GFX_KEY_DELETE) {
                if (cur < len) { memmove(buf + cur, buf + cur + 1, (size_t)(len - cur)); ret = 1; }
            } else if (k == GFX_KEY_LEFT) { if (cur > 0) cur--; }
            else if (k == GFX_KEY_RIGHT) { if (cur < len) cur++; }
            else if (k == GFX_KEY_HOME) cur = 0;
            else if (k == GFX_KEY_END) cur = len;
            else if (k == 21) { buf[0] = '\0'; cur = 0; ret = 1; }          /* Ctrl+U: সব মোছো */
            else if (k == 3) { if (!pw_mask) ui_clip_set(buf, len); }       /* Ctrl+C (secret নয়) */
            else if (k == 22) {                                             /* Ctrl+V */
                int n = 0, i;
                char *c = ui_clip_get(&n);
                for (i = 0; c && i < n && len + 1 < size; i++) {
                    if (c[i] < 32 || c[i] > 126) continue;
                    memmove(buf + cur + 1, buf + cur, (size_t)(len - cur + 1));
                    buf[cur++] = c[i];
                    len++;
                    ret = 1;
                }
                free(c);
            } else if (k >= 32 && k < 127 && len + 1 < size) {
                memmove(buf + cur + 1, buf + cur, (size_t)(len - cur + 1));
                buf[cur++] = (char)k;
                ret = 1;
            } else {
                u->key = k;                      /* অন্য কী অ্যাপের জন্য রেখে দাও */
            }
            len = (int)strlen(buf);
            if (pw_mask) {
                int i, n = len < (int)sizeof(stars) - 1 ? len : (int)sizeof(stars) - 1;
                for (i = 0; i < n; i++) stars[i] = '*';
                stars[n] = '\0';
            }
        }
        u->tb_cur = cur;
        if (u->tb_off > len) u->tb_off = len;
        if (cur < u->tb_off) u->tb_off = cur;
        while (u->tb_off < cur && tw_n(disp + u->tb_off, cur - u->tb_off) > w - 12) u->tb_off++;
        u->auto_tick = 1;                        /* কার্সর ঝলকানো — ৫০০ ms পরপর ফ্রেম */
    }
    ui_sunken(u->s, x, y, w, h, t->field);
    gfx_set_clip(u->s, x + 3, y + 2, w - 6, h - 4);
    /* ধাপ ১৯ছ: password হলে লেখার বদলে * (disp) */
    gfx_text(u->s, NULL, x + 5, y + (h - FH) / 2, disp + (u->focus == id ? u->tb_off : 0), t->field_text);
    if (u->focus == id && u->blink)
        gfx_fill_rect(u->s, x + 5 + tw_n(disp + u->tb_off, u->tb_cur - u->tb_off), y + 4, 2, h - 8, GFX_WHITE);
    gfx_reset_clip(u->s);
    return ret;
}

/* ধাপ ১৯ছ: লেখা দেখা যায় না (*), কপি হয় না */
int ui_do_password(ui_ctx *u, int x, int y, int w, char *buf, int size)
{
    int r;
    pw_mask = 1;
    r = ui_do_textbox(u, x, y, w, buf, size);
    pw_mask = 0;
    return r;
}

int ui_do_vscroll(ui_ctx *u, int x, int y, int w, int h, int *pos, int total, int page)
{
    int id = ++u->id, old = *pos, track = h - 2 * w, th, ty, maxpos = total - page;
    int st;
    const struct ui_theme *t = &dark3d;
    if (maxpos < 0) maxpos = 0;
    /* ওপর-নিচের তীর-বোতাম */
    ++u->id;
    if (button_logic(u, id + 1, x, y, w, w, &st)) (*pos)--;
    ui_button(u->s, x, y, w, w, NULL, st);
    gfx_fill_triangle(u->s, x + w / 2, y + 5, x + 4, y + w - 6, x + w - 5, y + w - 6, t->text);
    ++u->id;
    if (button_logic(u, id + 2, x, y + h - w, w, w, &st)) (*pos)++;
    ui_button(u->s, x, y + h - w, w, w, NULL, st);
    gfx_fill_triangle(u->s, x + w / 2, y + h - 6, x + 4, y + h - w + 5, x + w - 5, y + h - w + 5, t->text);
    ui_sunken(u->s, x, y + w, w, track, t->dark2);
    th = total > 0 ? track * page / (total > page ? total : page) : track;
    if (th < 16) th = 16;
    if (th > track) th = track;
    ty = y + w + (maxpos > 0 ? (track - th) * *pos / maxpos : 0);
    if (ui_hover(u, x, ty, w, th) && u->pressed) { u->active = id; u->drag_off = u->my - ty; }
    else if (ui_hover(u, x, y + w, w, track) && u->pressed) *pos += u->my < ty ? -page : page;
    if (u->active == id && (u->buttons & GFX_BUTTON_LEFT) && track > th)
        *pos = (int)((long)(u->my - u->drag_off - y - w) * maxpos / (track - th));
    if (*pos > maxpos) *pos = maxpos;
    if (*pos < 0) *pos = 0;
    ty = y + w + (maxpos > 0 ? (track - th) * *pos / maxpos : 0);
    ui_panel(u->s, x, ty, w, th, u->active == id ? t->face_down : t->face, 1);
    return *pos != old;
}

int ui_do_list(ui_ctx *u, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *top)
{
    int id = ++u->id, rowh = 20, vis = (h - 4) / rowh, sb = n > vis ? 16 : 0, i, ret = 0, old = *sel;
    int over = ui_hover(u, x, y, w - sb, h);
    const struct ui_theme *t = &dark3d;
    take_focus(u, id, over);
    if (over && u->pressed) {
        int r = (u->my - y - 2) / rowh + *top;
        if (r >= 0 && r < n) {
            *sel = r;
            if (u->dclick) ret = 2;
        }
        u->focus_claimed = 1;
    }
    if (over && u->wheel) *top += u->wheel * 3;
    if (u->focus == id && u->key && !u->block) {
        int k = u->key;
        u->key = 0;
        if (k == GFX_KEY_UP) (*sel)--;
        else if (k == GFX_KEY_DOWN) (*sel)++;
        else if (k == GFX_KEY_PAGE_UP) *sel -= vis;
        else if (k == GFX_KEY_PAGE_DOWN) *sel += vis;
        else if (k == GFX_KEY_HOME) *sel = 0;
        else if (k == GFX_KEY_END) *sel = n - 1;
        else if (k == '\n' && *sel >= 0) ret = 2;
        else if (k == '\t') ui_focus_next(u);
        else u->key = k;
        if (*sel >= n) *sel = n - 1;
        if (*sel < 0 && n > 0) *sel = 0;
        if (*sel >= 0 && *sel < *top) *top = *sel;
        if (*sel >= *top + vis) *top = *sel - vis + 1;
    }
    if (*top > n - vis) *top = n - vis;
    if (*top < 0) *top = 0;
    ui_sunken(u->s, x, y, w - sb, h, t->field);
    gfx_set_clip(u->s, x + 2, y + 2, w - sb - 4, h - 4);
    for (i = 0; i < vis + 1 && *top + i < n; i++) {
        int r = *top + i, ry = y + 2 + i * rowh;
        if (r == *sel) gfx_fill_rect(u->s, x + 2, ry, w - sb - 4, rowh, t->select);
        if (r == *sel && u->focus == id) gfx_rect(u->s, x + 2, ry, w - sb - 4, rowh, t->accent);
        gfx_text(u->s, list_font, x + 6, ry + 2, items[r], r == *sel ? GFX_WHITE : t->field_text);
    }
    gfx_reset_clip(u->s);
    if (sb) ui_do_vscroll(u, x + w - sb, y, sb, h, top, n, vis);
    if (!ret && *sel != old) ret = 1;
    return ret;
}

int ui_do_slider(ui_ctx *u, int x, int y, int w, int *value, int min, int max)
{
    int id = ++u->id, old = *value, kx;
    const struct ui_theme *t = &dark3d;
    if (max <= min) max = min + 1;
    if (ui_hover(u, x, y, w, 20) && u->pressed) u->active = id;
    if (u->active == id && (u->buttons & GFX_BUTTON_LEFT))
        *value = min + (int)((long)(u->mx - x - 5) * (max - min) / (w - 10));
    if (*value < min) *value = min;
    if (*value > max) *value = max;
    ui_sunken(u->s, x, y + 8, w, 5, t->dark2);
    kx = x + 5 + (int)((long)(*value - min) * (w - 10) / (max - min));
    ui_panel(u->s, kx - 5, y, 11, 20, u->active == id ? t->face_down : t->face_hover, 1);
    return *value != old;
}

/* ---------- মেনু-ফিতা ---------- */

int ui_shortcut_key(const char *sc)
{
    if (!sc || !sc[0]) return 0;
    if (!strncmp(sc, "Ctrl+", 5) && sc[5] && !sc[6]) {
        char c = sc[5];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
        return 0;
    }
    if (sc[0] == 'F' && sc[1] >= '1' && sc[1] <= '9') {
        int n = atoi(sc + 1);
        if (n >= 1 && n <= 12) return GFX_KEY_F1 + n - 1;
    }
    if (!strcmp(sc, "Del")) return GFX_KEY_DELETE;
    if (!strcmp(sc, "Esc")) return GFX_KEY_ESC;
    return 0;
}

static int menu_title_x(ui_ctx *u, int k)
{
    int i, x = u->mb_x + 4;
    for (i = 0; i < k; i++) x += gfx_text_width(NULL, u->mb[i].title) + 16;
    return x;
}

static void menu_drop_rect(ui_ctx *u, int m, int *x, int *y, int *w, int *h)
{
    const struct ui_menu *mm = &u->mb[m];
    int i, lw = 0, sw = 0, hh = 6;
    for (i = 0; i < mm->n; i++) {
        const struct ui_menuitem *it = &mm->items[i];
        if (!strcmp(it->label, "-")) { hh += 8; continue; }
        if (gfx_text_width(NULL, it->label) > lw) lw = gfx_text_width(NULL, it->label);
        if (it->shortcut && gfx_text_width(NULL, it->shortcut) > sw) sw = gfx_text_width(NULL, it->shortcut);
        hh += 22;
    }
    *x = menu_title_x(u, m);
    *y = u->mb_y + UI_MENUBAR_H;
    *w = lw + sw + 48;
    *h = hh;
    if (*x + *w > u->w) *x = u->w - *w;
    if (*x < 0) *x = 0;
}

/* (মাউস) কোন আইটেমের ওপরে: index, নইলে -1 */
static int menu_item_at(ui_ctx *u, int m, int px, int py)
{
    int x, y, w, h, i, yy;
    menu_drop_rect(u, m, &x, &y, &w, &h);
    if (px < x || px >= x + w || py < y || py >= y + h) return -1;
    yy = y + 3;
    for (i = 0; i < u->mb[m].n; i++) {
        int ih = !strcmp(u->mb[m].items[i].label, "-") ? 8 : 22;
        if (py >= yy && py < yy + ih) return ih == 8 ? -1 : i;
        yy += ih;
    }
    return -1;
}

static int menu_next_item(ui_ctx *u, int m, int from, int dir)
{
    int n = u->mb[m].n, i, k = from;
    for (i = 0; i < n; i++) {
        k = (k + dir + n) % n;
        if (strcmp(u->mb[m].items[k].label, "-")) return k;
    }
    return from;
}

int ui_menubar(ui_ctx *u, int x, int y, int w, const struct ui_menu *menus, int n)
{
    const struct ui_theme *t = &dark3d;
    int i, result = 0, on_bar = u->mx >= x && u->mx < x + w && u->my >= y && u->my < y + UI_MENUBAR_H;
    u->mb = menus;
    u->mb_n = n;
    u->mb_x = x;
    u->mb_y = y;
    /* শর্টকাট (মেনু বন্ধ থাকলে) */
    if (!u->mb_open && u->key) {
        int m, k;
        if (u->key == GFX_KEY_F10 && n > 0) {
            u->mb_open = 1;
            u->mb_item = menu_next_item(u, 0, -1, 1);
            u->key = 0;
        }
        for (m = 0; m < n && u->key; m++)
            for (k = 0; k < menus[m].n; k++)
                if (menus[m].items[k].shortcut && ui_shortcut_key(menus[m].items[k].shortcut) == u->key) {
                    result = menus[m].items[k].id;
                    u->key = 0;
                    break;
                }
    }
    /* মাউস */
    if (on_bar) {
        int over = -1;
        for (i = 0; i < n; i++) {
            int tx = menu_title_x(u, i), tw = gfx_text_width(NULL, menus[i].title) + 16;
            if (u->mx >= tx - 4 && u->mx < tx - 4 + tw) over = i;
        }
        if (u->pressed) {
            if (over >= 0 && u->mb_open != over + 1) { u->mb_open = over + 1; u->mb_item = -1; }
            else u->mb_open = 0;
            u->pressed = 0;
            u->block = 1;
        } else if (u->mb_open && over >= 0 && u->mb_open != over + 1) {
            u->mb_open = over + 1;
            u->mb_item = -1;
        }
    } else if (u->mb_open) {
        int m = u->mb_open - 1, it = menu_item_at(u, m, u->mx, u->my);
        if (it >= 0) u->mb_item = it;
        if ((u->released || u->pressed) && it >= 0) {
            if (u->released) { result = menus[m].items[it].id; u->mb_open = 0; }
        } else if (u->pressed && it < 0) {
            int dx, dy, dw, dh;
            menu_drop_rect(u, m, &dx, &dy, &dw, &dh);
            if (!(u->mx >= dx && u->mx < dx + dw && u->my >= dy && u->my < dy + dh)) u->mb_open = 0;
        }
        u->block = 1;
    }
    /* কীবোর্ড (মেনু খোলা) */
    if (u->mb_open && u->key) {
        int m = u->mb_open - 1, k = u->key;
        u->key = 0;
        if (k == GFX_KEY_ESC || k == GFX_KEY_F10) u->mb_open = 0;
        else if (k == GFX_KEY_DOWN) u->mb_item = menu_next_item(u, m, u->mb_item, 1);
        else if (k == GFX_KEY_UP) u->mb_item = menu_next_item(u, m, u->mb_item < 0 ? 0 : u->mb_item, -1);
        else if (k == GFX_KEY_RIGHT || k == GFX_KEY_LEFT) {
            m = (m + (k == GFX_KEY_RIGHT ? 1 : n - 1)) % n;
            u->mb_open = m + 1;
            u->mb_item = menu_next_item(u, m, -1, 1);
        } else if (k == '\n' && u->mb_item >= 0) {
            result = menus[m].items[u->mb_item].id;
            u->mb_open = 0;
        }
    }
    if (u->mb_open) u->block = 1;
    if (result) u->block = 1;
    /* ফিতা আঁকা */
    gfx_fill_rect(u->s, x, y, w, UI_MENUBAR_H - 1, t->menubar);
    gfx_hline(u->s, x, y + UI_MENUBAR_H - 1, w, t->dark);
    for (i = 0; i < n; i++) {
        int tx = menu_title_x(u, i), tw = gfx_text_width(NULL, menus[i].title) + 16;
        int hov = on_bar && u->mx >= tx - 4 && u->mx < tx - 4 + tw;
        if (u->mb_open == i + 1) ui_sunken(u->s, tx - 4, y + 1, tw, UI_MENUBAR_H - 2, t->face_down);
        else if (hov) ui_panel(u->s, tx - 4, y + 1, tw, UI_MENUBAR_H - 2, t->face_hover, 1);
        gfx_text(u->s, NULL, tx + 4, y + (UI_MENUBAR_H - FH) / 2, menus[i].title, t->text);
    }
    return result;
}

static void draw_menu_overlay(ui_ctx *u)
{
    const struct ui_theme *t = &dark3d;
    int m, x, y, w, h, i, yy;
    if (!u->mb_open || !u->mb || u->mb_open > u->mb_n) return;
    m = u->mb_open - 1;
    menu_drop_rect(u, m, &x, &y, &w, &h);
    ui_panel(u->s, x, y, w, h, t->face, 1);
    yy = y + 3;
    for (i = 0; i < u->mb[m].n; i++) {
        const struct ui_menuitem *it = &u->mb[m].items[i];
        if (!strcmp(it->label, "-")) {
            ui_separator(u->s, x + 4, yy + 3, w - 8);
            yy += 8;
            continue;
        }
        ui_menu_item(u->s, x + 3, yy, w - 6, 22, it->label, i == u->mb_item);
        if (it->shortcut)
            gfx_text(u->s, NULL, x + w - 10 - gfx_text_width(NULL, it->shortcut), yy + 3, it->shortcut,
                     i == u->mb_item ? GFX_WHITE : t->text_dim);
        yy += 22;
    }
}

/* ================= ধাপ ১৪খ: ডায়ালগ ================= */

struct dlg {
    ui_ctx u;
    gfx_surface *snap;
    int x, y, w, h;
};

static void dlg_open(struct dlg *d, int w, int h)
{
    gfx_surface *scr = gfx_screen();
    ui_init(&d->u);
    ui_ask_before_close(&d->u, 1);
    d->snap = gfx_surface_new(scr->w, scr->h);
    if (d->snap) {
        if (last_frame && last_frame->w == scr->w && last_frame->h == scr->h)
            gfx_blit(d->snap, 0, 0, last_frame, 0, 0, -1, -1);
        else
            gfx_blit(d->snap, 0, 0, scr, 0, 0, -1, -1);
    }
    d->w = w;
    d->h = h;
}

/* প্রতি ফ্রেমে: পেছনের ছবি (একটু অন্ধকার), তারপর ডায়ালগের ফ্রেম আর শিরোনাম */
static void dlg_frame(struct dlg *d, const char *title)
{
    const struct ui_theme *t = &dark3d;
    gfx_surface *s = d->u.s;
    if (d->w > d->u.w - 8) d->w = d->u.w - 8;
    if (d->h > d->u.h - 8) d->h = d->u.h - 8;
    d->x = (d->u.w - d->w) / 2;
    d->y = (d->u.h - d->h) / 2;
    if (d->snap) gfx_blit(s, 0, 0, d->snap, 0, 0, -1, -1);
    else gfx_clear(s, t->window_bg);
    gfx_fill_rect_alpha(s, 0, 0, d->u.w, d->u.h, GFX_RGBA(0, 0, 0, 90));
    ui_panel(s, d->x, d->y, d->w, d->h, t->face, 1);
    gfx_gradient_v(s, d->x + 3, d->y + 3, d->w - 6, 22, t->title_a1, t->title_a2);
    gfx_set_clip(s, d->x + 3, d->y + 3, d->w - 6, 22);
    gfx_text(s, NULL, d->x + 10, d->y + 6, title ? title : "", GFX_WHITE);
    gfx_reset_clip(s);
}

static void dlg_close(struct dlg *d)
{
    if (d->snap) {
        gfx_blit(gfx_screen(), 0, 0, d->snap, 0, 0, -1, -1);
        /* পরের ডায়ালগের পেছনে যেন এই ডায়ালগের ছবি না থাকে */
        if (last_frame && last_frame->w == d->snap->w && last_frame->h == d->snap->h)
            gfx_blit(last_frame, 0, 0, d->snap, 0, 0, -1, -1);
        gfx_surface_free(d->snap);
    }
    gfx_present();
    force_frame = 1;
}

/* বার্তা-বাক্সের ছোট চিহ্ন */
static void mb_symbol(gfx_surface *s, int x, int y, int type)
{
    uint32_t c = (type & UI_MB_ERROR) ? GFX_RGB(200, 50, 50) : (type & UI_MB_WARN) ? GFX_RGB(220, 170, 40)
               : (type & UI_MB_QUESTION) ? GFX_RGB(85, 120, 170) : GFX_RGB(85, 93, 80);
    const char *ch = (type & UI_MB_ERROR) ? "x" : (type & UI_MB_WARN) ? "!" : (type & UI_MB_QUESTION) ? "?" : "i";
    if (type & UI_MB_WARN) {
        gfx_fill_triangle(s, x + 16, y + 1, x + 1, y + 30, x + 31, y + 30, c);
        gfx_text(s, NULL, x + 12, y + 12, ch, GFX_BLACK);
    } else {
        gfx_fill_circle(s, x + 16, y + 16, 15, c);
        gfx_circle(s, x + 16, y + 16, 15, GFX_RGB(220, 220, 220));
        gfx_text(s, NULL, x + 12, y + 8, ch, GFX_WHITE);
    }
}

int ui_msgbox(const char *title, const char *text, int type)
{
    static const char *const names[4][3] = {
        { "OK", NULL, NULL }, { "OK", "Cancel", NULL }, { "Yes", "No", NULL }, { "Yes", "No", "Cancel" }
    };
    static const int vals[4][3] = {
        { UI_OK, 0, 0 }, { UI_OK, UI_CANCEL, 0 }, { UI_YES, UI_NO, 0 }, { UI_YES, UI_NO, UI_CANCEL }
    };
    struct dlg d;
    int kind = type & 3, nb = kind == 0 ? 1 : kind == 3 ? 3 : 2, icon = (type & 0xF0) != 0;
    int lines = 1, longest = 0, cur = 0, answer = -1, i;
    const char *p;
    if (!text) text = "";
    for (p = text; *p; p++) {                    /* লাইন গোনা (৭০ অক্ষরে ভাঙা) */
        if (*p == '\n' || cur >= 70) { lines++; cur = 0; if (*p == '\n') continue; }
        cur++;
        if (cur > longest) longest = cur;
    }
    dlg_open(&d, (longest * FW + 60 + (icon ? 48 : 0)) > nb * 92 + 30 ? longest * FW + 60 + (icon ? 48 : 0) : nb * 92 + 30,
             lines * 18 + 100);
    if (d.w < 300) d.w = 300;
    while (answer < 0 && ui_run(&d.u)) {
        int tx, ty, bx;
        dlg_frame(&d, title);
        tx = d.x + 20 + (icon ? 48 : 0);
        ty = d.y + 44;
        if (icon) mb_symbol(d.u.s, d.x + 18, d.y + 40, type);
        {
            char line[80];
            int n = 0;
            for (p = text;; p++) {
                if (*p == '\n' || *p == '\0' || n >= 70) {
                    line[n] = '\0';
                    gfx_text(d.u.s, NULL, tx, ty, line, GFX_WHITE);
                    ty += 18;
                    n = 0;
                    if (*p == '\0') break;
                    if (*p == '\n') continue;
                }
                line[n++] = *p;
            }
        }
        bx = d.x + d.w - nb * 92 - 10;
        for (i = 0; i < nb; i++)
            if (ui_do_button(&d.u, bx + i * 92, d.y + d.h - 40, 84, 28, names[kind][i])) answer = vals[kind][i];
        if (d.u.key == '\n') answer = vals[kind][0];
        else if (d.u.key == GFX_KEY_ESC) answer = kind == 0 ? UI_OK : kind == 2 ? UI_NO : UI_CANCEL;
        else if ((d.u.key == 'y' || d.u.key == 'Y') && kind >= 2) answer = UI_YES;
        else if ((d.u.key == 'n' || d.u.key == 'N') && kind >= 2) answer = UI_NO;
        if (d.u.closing) { close_flag = 1; answer = kind == 0 ? UI_OK : kind == 2 ? UI_NO : UI_CANCEL; }
    }
    if (answer < 0) answer = UI_CANCEL;
    dlg_close(&d);
    return answer;
}

int ui_inputbox(const char *title, const char *prompt, char *buf, int size)
{
    struct dlg d;
    int answer = -1;
    dlg_open(&d, 420, 140);
    d.u.want_focus = 1;
    while (answer < 0 && ui_run(&d.u)) {
        int r;
        dlg_frame(&d, title);
        gfx_text(d.u.s, NULL, d.x + 16, d.y + 38, prompt ? prompt : "", GFX_WHITE);
        r = ui_do_textbox(&d.u, d.x + 16, d.y + 60, d.w - 32, buf, size);
        if (r == 2) answer = 1;
        if (ui_do_button(&d.u, d.x + d.w - 192, d.y + d.h - 40, 84, 28, "OK")) answer = 1;
        if (ui_do_button(&d.u, d.x + d.w - 100, d.y + d.h - 40, 84, 28, "Cancel")) answer = 0;
        if (d.u.key == GFX_KEY_ESC) answer = 0;
        if (d.u.key == '\n') answer = 1;
        if (d.u.closing) { close_flag = 1; answer = 0; }
    }
    if (answer < 0) answer = 0;
    dlg_close(&d);
    return answer;
}

/* ধাপ ১৯ছ: secret চাওয়া; confirm = 1 হলে দুবার (মিলতে হবে) */
int ui_passwordbox(const char *title, const char *prompt, char *buf, int size, int confirm)
{
    struct dlg d;
    char again[256] = "";
    int answer = -1, bad = 0;
    dlg_open(&d, 420, confirm ? 200 : 150);
    d.u.want_focus = 1;
    buf[0] = '\0';
    while (answer < 0 && ui_run(&d.u)) {
        int r1, r2 = 0, ok = 0, yy;
        dlg_frame(&d, title);
        gfx_text(d.u.s, NULL, d.x + 16, d.y + 38, prompt ? prompt : "Secret:", GFX_WHITE);
        r1 = ui_do_password(&d.u, d.x + 16, d.y + 58, d.w - 32, buf, size);
        yy = d.y + 88;
        if (confirm) {
            gfx_text(d.u.s, NULL, d.x + 16, yy, "Again:", GFX_WHITE);
            r2 = ui_do_password(&d.u, d.x + 16, yy + 20, d.w - 32, again, (int)sizeof(again));
            yy += 50;
            if (r1 == 2) ui_focus_next(&d.u);
        }
        if (bad) gfx_text(d.u.s, NULL, d.x + 16, yy, bad == 1 ? "The two are not the same." : "Write something first.",
                          GFX_RGB(240, 110, 100));
        if (ui_do_button(&d.u, d.x + d.w - 192, d.y + d.h - 40, 84, 28, "OK")) ok = 1;
        if (ui_do_button(&d.u, d.x + d.w - 100, d.y + d.h - 40, 84, 28, "Cancel")) answer = 0;
        if ((confirm ? r2 : r1) == 2 || d.u.key == '\n') ok = 1;
        if (d.u.key == GFX_KEY_ESC) answer = 0;
        if (d.u.closing) { close_flag = 1; answer = 0; }
        if (ok && answer < 0) {
            if (!buf[0]) bad = 2;
            else if (confirm && strcmp(buf, again)) { bad = 1; again[0] = '\0'; }
            else answer = 1;
            ui_redraw(&d.u);
        }
    }
    memset(again, 0, sizeof(again));
    if (answer < 0) answer = 0;
    if (!answer) memset(buf, 0, (size_t)size);
    dlg_close(&d);
    return answer;
}

/* ধাপ ১৯ছ: ডান-ক্লিকের মেনু (x, y = উইন্ডোর ভেতরে, সাধারণত u.mx/u.my)। বাছাই করা আইটেমের
 * id ফেরত, বাইরে ক্লিক বা Esc = 0। id < 0 = ধূসর (চাপা যায় না), label "-" = রেখা */
int ui_popup_menu(const struct ui_menuitem *items, int n, int x, int y)
{
    struct dlg d;
    const struct ui_theme *t = &dark3d;
    int answer = -1, sel = -1, w = 0, h = 6, i, first = 1;
    for (i = 0; i < n; i++) {
        int tw = gfx_text_width(NULL, items[i].label) + (items[i].shortcut ? gfx_text_width(NULL, items[i].shortcut) + 24 : 0);
        if (tw > w) w = tw;
        h += !strcmp(items[i].label, "-") ? 8 : 22;
    }
    w += 40;
    dlg_open(&d, w, h);
    while (answer < 0 && ui_run(&d.u)) {
        int px = x, py = y, yy, over = -1;
        gfx_surface *s = d.u.s;
        if (px + w > d.u.w) px = d.u.w - w;
        if (py + h > d.u.h) py = d.u.h - h;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (d.snap) gfx_blit(s, 0, 0, d.snap, 0, 0, -1, -1);
        yy = py + 3;
        for (i = 0; i < n; i++) {
            int ih = !strcmp(items[i].label, "-") ? 8 : 22;
            if (ih == 22 && items[i].id > 0 && d.u.mx >= px && d.u.mx < px + w && d.u.my >= yy && d.u.my < yy + ih) over = i;
            yy += ih;
        }
        if (over >= 0) sel = over;
        if (d.u.key == GFX_KEY_DOWN || d.u.key == GFX_KEY_UP) {
            int k, dir = d.u.key == GFX_KEY_DOWN ? 1 : -1;
            for (k = 0; k < n; k++) {
                sel = (sel + dir + n) % n;
                if (strcmp(items[sel].label, "-") && items[sel].id > 0) break;
            }
        }
        if (d.u.key == '\n' && sel >= 0) answer = items[sel].id;
        if (d.u.key == GFX_KEY_ESC) answer = 0;
        /* প্রথম ফ্রেমের ঘটনা = যে ডান-ক্লিকে মেনু খুলেছে, সেটা গোনা নয় */
        if (!first && (d.u.pressed || d.u.rpressed)) {
            if (over >= 0 && d.u.pressed) answer = items[over].id;
            else if (!(d.u.mx >= px && d.u.mx < px + w && d.u.my >= py && d.u.my < py + h)) answer = 0;
        }
        if (d.u.closing) { close_flag = 1; answer = 0; }
        first = 0;
        ui_panel(s, px, py, w, h, t->face, 1);
        yy = py + 3;
        for (i = 0; i < n; i++) {
            if (!strcmp(items[i].label, "-")) {
                ui_separator(s, px + 4, yy + 3, w - 8);
                yy += 8;
                continue;
            }
            if (items[i].id > 0) ui_menu_item(s, px + 3, yy, w - 6, 22, items[i].label, i == sel);
            else gfx_text(s, NULL, px + 13, yy + 3, items[i].label, t->text_dim);
            if (items[i].shortcut)
                gfx_text(s, NULL, px + w - 10 - gfx_text_width(NULL, items[i].shortcut), yy + 3, items[i].shortcut,
                         i == sel ? GFX_WHITE : t->text_dim);
            yy += 22;
        }
    }
    if (answer < 0) answer = 0;
    dlg_close(&d);
    return answer;
}

/* ---------- ফাইল বাছাই ---------- */

#define FD_MAX 1024
struct fd_list {
    char *names[FD_MAX];
    const char *shown[FD_MAX];
    char isdir[FD_MAX];
    int n;
};

static void fd_free(struct fd_list *l)
{
    int i;
    for (i = 0; i < l->n; i++) free(l->names[i]);
    l->n = 0;
}

static int fd_cmp(const struct fd_list *l, int a, int b)
{
    if (l->isdir[a] != l->isdir[b]) return l->isdir[b] - l->isdir[a];
    return strcmp(l->names[a], l->names[b]);
}

static void fd_read(struct fd_list *l, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int i, j;
    fd_free(l);
    if (!d) return;
    while ((e = readdir(d)) && l->n < FD_MAX) {
        char p[512], *nm;
        struct stat st;
        size_t len;
        if (!strcmp(e->d_name, ".") || e->d_name[0] == '\0') continue;
        if (e->d_name[0] == '.' && strcmp(e->d_name, "..")) continue;     /* লুকানো ফাইল বাদ */
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        len = strlen(e->d_name);
        nm = (char *)malloc(len + 2);
        if (!nm) break;
        strcpy(nm, e->d_name);
        l->isdir[l->n] = (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) || !strcmp(e->d_name, "..");
        if (l->isdir[l->n]) strcat(nm, "/");
        l->names[l->n++] = nm;
    }
    closedir(d);
    if (strcmp(dir, "/")) {                      /* ".." সবসময় ওপরে (readdir না দিলেও) */
        int have = 0;
        for (i = 0; i < l->n; i++) if (!strcmp(l->names[i], "../")) have = 1;
        if (!have && l->n < FD_MAX) { l->names[l->n] = strdup("../"); l->isdir[l->n] = 1; l->n++; }
    }
    for (i = 0; i < l->n; i++)                   /* সাধারণ বাছাই: ফোল্ডার আগে */
        for (j = i + 1; j < l->n; j++) {
            int up_i = !strcmp(l->names[i], "../"), up_j = !strcmp(l->names[j], "../");
            if (up_j || (!up_i && fd_cmp(l, i, j) > 0)) {
                char *t = l->names[i]; char td = l->isdir[i];
                l->names[i] = l->names[j]; l->isdir[i] = l->isdir[j];
                l->names[j] = t; l->isdir[j] = td;
            }
        }
    for (i = 0; i < l->n; i++) l->shown[i] = l->names[i];
}

/* dir + name → out (".." আর "." বুঝে) */
static void path_join(char *out, int size, const char *dir, const char *name)
{
    char tmp[512];
    if (name[0] == '/') snprintf(tmp, sizeof(tmp), "%s", name);
    else snprintf(tmp, sizeof(tmp), "%s/%s", strcmp(dir, "/") ? dir : "", name);
    /* সরলীকরণ: // . .. */
    {
        char *parts[64], *q, *save = tmp + 1;
        int np = 0, i, o = 0;
        while ((q = strtok_r(np == 0 ? save : NULL, "/", &save)) && np < 64) {
            if (!strcmp(q, ".")) continue;
            if (!strcmp(q, "..")) { if (np > 0) np--; continue; }
            parts[np++] = q;
        }
        if (size > 0) out[0] = '\0';
        for (i = 0; i < np && o < size - 1; i++) o += snprintf(out + o, (size_t)(size - o), "/%s", parts[i]);
        if (np == 0 && size > 1) strcpy(out, "/");
    }
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int ui_filedlg(const char *title, char *path, int size, int save)
{
    static struct fd_list l;
    struct dlg d;
    char dir[512], name[256], dirbox[512];
    int sel = -1, top = 0, answer = -1, reload = 1;
    if (path && path[0] && is_dir(path)) {
        path_join(dir, sizeof(dir), "/", path);
        name[0] = '\0';
    } else if (path && path[0] && strchr(path, '/')) {
        const char *sl = strrchr(path, '/');
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%.*s", (int)(sl - path), path);
        if (!tmp[0]) strcpy(tmp, "/");
        if (tmp[0] != '/') { char cwd[256]; getcwd(cwd, sizeof(cwd)); path_join(dir, sizeof(dir), cwd, tmp); }
        else path_join(dir, sizeof(dir), "/", tmp);
        snprintf(name, sizeof(name), "%s", sl + 1);
    } else {
        if (!getcwd(dir, sizeof(dir))) strcpy(dir, "/");
        snprintf(name, sizeof(name), "%s", path ? path : "");
    }
    dlg_open(&d, 600, 440);
    d.u.want_focus = 0;
    while (answer < 0 && ui_run(&d.u)) {
        int r, lx, ly, lw, lh, accept = 0;
        if (reload) {
            fd_read(&l, dir);
            snprintf(dirbox, sizeof(dirbox), "%s", dir);
            sel = -1;
            top = 0;
            reload = 0;
        }
        dlg_frame(&d, title);
        gfx_text(d.u.s, NULL, d.x + 14, d.y + 40, "Folder:", GFX_WHITE);
        r = ui_do_textbox(&d.u, d.x + 80, d.y + 36, d.w - 80 - 52, dirbox, sizeof(dirbox));
        if (r == 2) {
            char nd[512];
            path_join(nd, sizeof(nd), dir, dirbox);
            if (is_dir(nd)) { snprintf(dir, sizeof(dir), "%s", nd); reload = 1; }
            else ui_msgbox("Folder", "No such folder.", UI_MB_OK | UI_MB_ERROR);
        }
        if (ui_do_tool(&d.u, d.x + d.w - 46, d.y + 36, 32, UI_TEXTBOX_H, "up", 'u', NULL) && strcmp(dir, "/")) {
            char nd[512];
            path_join(nd, sizeof(nd), dir, "..");
            snprintf(dir, sizeof(dir), "%s", nd);
            reload = 1;
        }
        lx = d.x + 14; ly = d.y + 70; lw = d.w - 28; lh = d.h - 70 - 86;
        r = ui_do_list(&d.u, lx, ly, lw, lh, l.shown, l.n, &sel, &top);
        if (r && sel >= 0 && sel < l.n && !l.isdir[sel]) snprintf(name, sizeof(name), "%s", l.names[sel]);
        if (r == 2 && sel >= 0 && sel < l.n) {
            if (l.isdir[sel]) {
                char nd[512], nm[256];
                snprintf(nm, sizeof(nm), "%s", l.names[sel]);
                nm[strlen(nm) - 1] = '\0';
                path_join(nd, sizeof(nd), dir, nm);
                snprintf(dir, sizeof(dir), "%s", nd);
                reload = 1;
            } else {
                accept = 1;
            }
        }
        gfx_text(d.u.s, NULL, d.x + 14, d.y + d.h - 76, "Name:", GFX_WHITE);
        if (ui_do_textbox(&d.u, d.x + 80, d.y + d.h - 80, d.w - 94, name, sizeof(name)) == 2) accept = 1;
        if (ui_do_button(&d.u, d.x + d.w - 192, d.y + d.h - 40, 84, 28, save ? "Save" : "Open")) accept = 1;
        if (ui_do_button(&d.u, d.x + d.w - 100, d.y + d.h - 40, 84, 28, "Cancel")) answer = 0;
        if (d.u.key == GFX_KEY_ESC) answer = 0;
        if (d.u.key == '\n') accept = 1;              /* কোনো ঘরে ফোকাস নেই: Enter = Open/Save */
        if (d.u.closing) { close_flag = 1; answer = 0; }
        if (accept && name[0]) {
            char full[512];
            path_join(full, sizeof(full), dir, name);
            if (is_dir(full)) {
                snprintf(dir, sizeof(dir), "%s", full);
                name[0] = '\0';
                reload = 1;
            } else if (!save && access(full, F_OK) != 0) {
                ui_msgbox(title, "File not found.", UI_MB_OK | UI_MB_ERROR);
            } else if (save && access(full, F_OK) == 0 &&
                       ui_msgbox(title, "The file already exists.\nReplace it?", UI_MB_YESNO | UI_MB_WARN) != UI_YES) {
                /* না — আবার বাছাই */
            } else {
                snprintf(path, (size_t)size, "%s", full);
                answer = 1;
            }
        }
    }
    fd_free(&l);
    if (answer < 0) answer = 0;
    dlg_close(&d);
    return answer;
}

/* ================= ধাপ ১৪খ: সাহায্য ================= */

int ui_launch(const char *const argv[], const char *dir)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd;
        setsid();
        for (fd = 3; fd < 64; fd++) close(fd);
        if (dir) chdir(dir);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    return 0;
}

#define CLIPFILE "/tmp/.clipboard"

int ui_clip_set(const char *text, int len)
{
    int fd = open(CLIPFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644), done = 0;
    if (fd < 0) return -1;
    while (done < len) {
        ssize_t n = write(fd, text + done, (size_t)(len - done));
        if (n <= 0) break;
        done += (int)n;
    }
    close(fd);
    return done == len ? 0 : -1;
}

char *ui_clip_get(int *len)
{
    FILE *f = fopen(CLIPFILE, "rb");
    char *b = NULL;
    long n;
    *len = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (1 << 22)) {
        b = (char *)malloc((size_t)n + 1);
        if (b) {
            *len = (int)fread(b, 1, (size_t)n, f);
            b[*len] = '\0';
        }
    }
    fclose(f);
    if (b && *len == 0) { free(b); b = NULL; }
    return b;
}
