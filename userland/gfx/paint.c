/* rozOS — paint: মাউস দিয়ে আঁকা (ধাপ ১৩)
 *   বাঁ বোতাম টেনে আঁকা, ডান বোতাম মোছা (সাদা)। ওপরের সারি: রঙ, তুলির মাপ, বালতি (ভরাট),
 *   মুছে ফেলা, সংরক্ষণ।  কী: 1-9 রঙ, [ ] তুলি, f ভরাট, c মোছা, s সংরক্ষণ, q বের হওয়া
 *   paint FILE.bmp    ওই ছবি খুলে আঁকা (সংরক্ষণ সেখানেই); নইলে ~/drawing.bmp
 *   ধাপ ১৪খ: প্রথম সংরক্ষণে (বা S = Save as) ফাইল-ডায়ালগে নাম/ফোল্ডার বাছাই, তারপর
 *   ঠিক হল কিনা বার্তা-বাক্সে; o = অন্য ছবি খোলা (.bmp .png .jpg ...)। সংরক্ষণ BMP-তে।
 *
 * ধাপ ১৯ঘ: Windows Paint-এর মতো (সাদামাটা) — rozui দিয়ে নতুন করে:
 *   মেনু: File (New, Open, Save, Save as, Quit), Edit (Undo, Redo, Clear),
 *         Image (Flip, Rotate, Canvas size), Help
 *   বাঁ পাশে টুল: পেনসিল, তুলি, ইরেজার, বালতি, রং-তোলা, রেখা, আয়তক্ষেত্র, উপবৃত্ত, লেখা;
 *         নিচে মাপ (১/৩/৫/৮; লেখায় ১২–৪৮) আর আকারের ফাঁপা/ভরাট
 *   নিচে ২০টা রঙ: বাঁ-ক্লিক = রং ১, ডান-ক্লিক = রং ২ (ডিফল্ট সাদা — তাই ডান বোতামে
 *         আঁকা = আগের মতো মোছা)
 *   Undo/Redo (Ctrl+Z / Ctrl+Y, ১৬ ধাপ), স্ট্যাটাস বার, বেরোনোর আগে "Save changes?"
 *   সংরক্ষণ: নাম .png হলে PNG, .bmp হলে BMP (ডিফল্ট drawing.png)
 *   পুরোনো সব কী আগের মতো: 1-9 [ ] f c s S o q Esc, চাকা = তুলির মাপ;
 *   নতুন: p পেনসিল, b তুলি, e ইরেজার, i রং-তোলা, l রেখা, r আয়তক্ষেত্র, g উপবৃত্ত, t লেখা,
 *   তীর = বড় ছবি সরানো। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <rozgfx.h>
#include <rozui.h>
#include <errno.h>
#include <math.h>

#define TOOLBAR_W 64
#define PALETTE_H 48
#define STATUS_H  22
#define UNDO_MAX  16

enum { T_PENCIL, T_BRUSH, T_ERASER, T_FILL, T_PICK, T_LINE, T_RECT, T_ELLIPSE, T_TEXT, T_COUNT };
static const char *tool_names[T_COUNT] = {
    "Pencil", "Brush", "Eraser", "Fill", "Pick colour", "Line", "Rectangle", "Ellipse", "Text"
};

/* প্রথম ৯টা আগের প্যালেটের মতো (1-9 কী) */
static const uint32_t palette[20] = {
    GFX_RGB(0, 0, 0), GFX_RGB(255, 255, 255), GFX_RGB(220, 40, 40), GFX_RGB(250, 150, 30),
    GFX_RGB(250, 220, 40), GFX_RGB(40, 180, 70), GFX_RGB(40, 120, 230), GFX_RGB(140, 70, 200),
    GFX_RGB(120, 80, 40), GFX_RGB(128, 128, 128),
    GFX_RGB(64, 64, 64), GFX_RGB(192, 192, 192), GFX_RGB(255, 150, 160), GFX_RGB(255, 200, 140),
    GFX_RGB(255, 245, 160), GFX_RGB(170, 230, 120), GFX_RGB(130, 210, 240), GFX_RGB(110, 110, 220),
    GFX_RGB(200, 150, 220), GFX_RGB(0, 90, 70)
};
static const int sizes[4] = { 1, 3, 5, 8 };
static const int text_sizes[5] = { 12, 18, 24, 36, 48 };

static gfx_surface *canvas;
static uint32_t color = GFX_RGB(0, 0, 0), color2 = GFX_RGB(255, 255, 255);
static int brush = 3, tool = T_BRUSH, prev_tool = T_BRUSH, shape_fill, text_size = 24;
static char status[160] = "left: colour 1   right: colour 2 (white = erase)   Ctrl+Z undo";
static char path[512] = "/home/drawing.png";
static int named;                       /* ধাপ ১৪খ: নাম বাছাই হয়েছে (বা কমান্ডে দেওয়া) */
static int dirty;                       /* ধাপ ১৯ঘ: সংরক্ষণ না করা বদল */
static int scroll_x, scroll_y;
static gfx_ttf *font;

static gfx_surface *undo_st[UNDO_MAX], *redo_st[UNDO_MAX];
static int undo_n, redo_n;

/* লেখা টুল: কোথায়, কী লেখা হচ্ছে */
static int text_on, text_x, text_y;
static char text_buf[200];

/* ---------- surface সাহায্য ---------- */

static gfx_surface *copy_surface(const gfx_surface *src)
{
    gfx_surface *d = gfx_surface_new(src->w, src->h);
    if (d) gfx_blit(d, 0, 0, src, 0, 0, -1, -1);
    return d;
}

static void free_stack(gfx_surface **st, int *n)
{
    while (*n > 0) gfx_surface_free(st[--*n]);
}

/* বদলানোর ঠিক আগে ডাকো */
static void push_undo(void)
{
    gfx_surface *c = copy_surface(canvas);
    if (!c) return;
    if (undo_n == UNDO_MAX) {
        gfx_surface_free(undo_st[0]);
        memmove(undo_st, undo_st + 1, sizeof(undo_st[0]) * (UNDO_MAX - 1));
        undo_n--;
    }
    undo_st[undo_n++] = c;
    free_stack(redo_st, &redo_n);
    dirty = 1;
}

static void undo(void)
{
    if (!undo_n) { snprintf(status, sizeof(status), "nothing to undo"); return; }
    if (redo_n == UNDO_MAX) { gfx_surface_free(redo_st[0]); memmove(redo_st, redo_st + 1, sizeof(redo_st[0]) * (UNDO_MAX - 1)); redo_n--; }
    redo_st[redo_n++] = canvas;
    canvas = undo_st[--undo_n];
    dirty = 1;
    snprintf(status, sizeof(status), "undo (%d more)", undo_n);
}

static void redo(void)
{
    if (!redo_n) { snprintf(status, sizeof(status), "nothing to redo"); return; }
    if (undo_n == UNDO_MAX) { gfx_surface_free(undo_st[0]); memmove(undo_st, undo_st + 1, sizeof(undo_st[0]) * (UNDO_MAX - 1)); undo_n--; }
    undo_st[undo_n++] = canvas;
    canvas = redo_st[--redo_n];
    dirty = 1;
    snprintf(status, sizeof(status), "redo");
}

static void replace_canvas(gfx_surface *n)
{
    gfx_surface_free(canvas);
    canvas = n;
    scroll_x = scroll_y = 0;
}

/* ---------- আঁকার টুল ---------- */

static void fill_ellipse(gfx_surface *s, int cx, int cy, int rx, int ry, uint32_t c)
{
    int y;
    if (rx < 1 || ry < 1) { gfx_fill_rect(s, cx - rx, cy - ry, 2 * rx + 1, 2 * ry + 1, c); return; }
    for (y = -ry; y <= ry; y++) {
        double t = 1.0 - (double)y * y / ((double)ry * ry);
        int hw = t > 0 ? (int)(rx * sqrt(t) + 0.5) : 0;
        gfx_hline(s, cx - hw, cy + y, 2 * hw + 1, c);
    }
}

/* (x0,y0)-(x1,y1)-এর আয়তক্ষেত্র/উপবৃত্ত/রেখা s-এ; ox,oy = স্থানাঙ্ক সরানো (প্রিভিউর জন্য) */
static void draw_shape(gfx_surface *s, int t, int x0, int y0, int x1, int y1, int ox, int oy, uint32_t c)
{
    int lx = (x0 < x1 ? x0 : x1) + ox, ty = (y0 < y1 ? y0 : y1) + oy;
    int w = abs(x1 - x0) + 1, h = abs(y1 - y0) + 1, k;
    if (t == T_LINE) {
        gfx_thick_line(s, x0 + ox, y0 + oy, x1 + ox, y1 + oy, brush, c);
        if (brush > 2) {
            gfx_fill_circle(s, x0 + ox, y0 + oy, brush / 2, c);
            gfx_fill_circle(s, x1 + ox, y1 + oy, brush / 2, c);
        }
    } else if (t == T_RECT) {
        if (shape_fill) gfx_fill_rect(s, lx, ty, w, h, c);
        else for (k = 0; k < brush && 2 * k < w && 2 * k < h; k++) gfx_rect(s, lx + k, ty + k, w - 2 * k, h - 2 * k, c);
    } else if (t == T_ELLIPSE) {
        int rx = w / 2, ry = h / 2, cx = lx + rx, cy = ty + ry;
        if (shape_fill) fill_ellipse(s, cx, cy, rx, ry, c);
        else for (k = 0; k < brush && k < rx && k < ry; k++) gfx_ellipse(s, cx, cy, rx - k, ry - k, c);
    }
}

static void stroke(int t, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (t == T_PENCIL) gfx_line(canvas, x0, y0, x1, y1, c);
    else if (t == T_ERASER) {
        int e = brush * 3 + 2;
        gfx_thick_line(canvas, x0, y0, x1, y1, e, color2);
        gfx_fill_rect(canvas, x1 - e / 2, y1 - e / 2, e, e, color2);
    } else {                                                /* তুলি: গোল মাথা */
        gfx_thick_line(canvas, x0, y0, x1, y1, brush, c);
        if (brush > 2) gfx_fill_circle(canvas, x1, y1, brush / 2, c);
    }
    dirty = 1;
}

static void commit_text(void)
{
    if (text_on && text_buf[0]) {
        push_undo();
        if (font) gfx_ttf_text(canvas, font, (float)text_size, text_x, text_y, text_buf, color);
        else gfx_text(canvas, NULL, text_x, text_y, text_buf, color);
        snprintf(status, sizeof(status), "text placed (Ctrl+Z takes it back)");
    }
    text_on = 0;
    text_buf[0] = '\0';
}

static void set_tool(int t)
{
    if (tool != T_TEXT || t != T_TEXT) commit_text();
    if (t != T_PICK) prev_tool = t;
    tool = t;
    snprintf(status, sizeof(status), "%s", tool_names[t]);
}

/* ---------- ছবি বদল (Image মেনু) ---------- */

static void flip(int horizontal)
{
    int x, y;
    push_undo();
    for (y = 0; y < canvas->h; y++) {
        uint32_t *r = canvas->px + (size_t)y * canvas->stride;
        if (horizontal) {
            for (x = 0; x < canvas->w / 2; x++) { uint32_t t = r[x]; r[x] = r[canvas->w - 1 - x]; r[canvas->w - 1 - x] = t; }
        } else if (y < canvas->h / 2) {
            uint32_t *o = canvas->px + (size_t)(canvas->h - 1 - y) * canvas->stride;
            for (x = 0; x < canvas->w; x++) { uint32_t t = r[x]; r[x] = o[x]; o[x] = t; }
        }
    }
}

static void rotate_right(void)
{
    gfx_surface *n = gfx_surface_new(canvas->h, canvas->w);
    int x, y;
    if (!n) return;
    push_undo();
    for (y = 0; y < canvas->h; y++)
        for (x = 0; x < canvas->w; x++)
            n->px[(size_t)x * n->stride + (canvas->h - 1 - y)] = canvas->px[(size_t)y * canvas->stride + x];
    replace_canvas(n);
}

static void canvas_size(void)
{
    char b[40];
    int w, h;
    gfx_surface *n;
    snprintf(b, sizeof(b), "%dx%d", canvas->w, canvas->h);
    if (!ui_inputbox("Canvas size", "Width x Height (e.g. 800x600):", b, sizeof(b))) return;
    if (sscanf(b, "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 4096 || h > 4096) {
        ui_msgbox("Paint", "Give the size like 800x600 (1 to 4096).", UI_MB_OK | UI_MB_WARN);
        return;
    }
    n = gfx_surface_new(w, h);
    if (!n) return;
    push_undo();
    gfx_clear(n, color2);
    gfx_blit(n, 0, 0, canvas, 0, 0, -1, -1);
    replace_canvas(n);
}

/* ---------- ফাইল ---------- */

/* ধাপ ১৪খ: .bmp না থাকলে যোগ করা; ধাপ ১৯ঘ: .png-ও চলে (অন্য সব নাম → .png) */
static void force_bmp(char *p, size_t size)
{
    size_t n = strlen(p);
    if (n >= 4 && (!strcasecmp(p + n - 4, ".bmp") || !strcasecmp(p + n - 4, ".png"))) return;
    {
        char *dot = strrchr(p, '.'), *sl = strrchr(p, '/');
        if (dot && (!sl || dot > sl)) *dot = '\0';
        strncat(p, ".png", size - strlen(p) - 1);
    }
}

static int is_png(const char *p)
{
    size_t n = strlen(p);
    return n >= 4 && !strcasecmp(p + n - 4, ".png");
}

static int save(int ask)
{
    char m[700];
    int r;
    commit_text();
    if (ask || !named) {
        char p[512];
        snprintf(p, sizeof(p), "%s", path);
        if (!ui_filedlg("Save picture (.png or .bmp)", p, sizeof(p), 1)) { snprintf(status, sizeof(status), "not saved"); return -1; }
        force_bmp(p, sizeof(p));
        snprintf(path, sizeof(path), "%s", p);
        named = 1;
    }
    r = is_png(path) ? gfx_save_png(canvas, path) : gfx_save_bmp(canvas, path);
    if (r == 0) {
        dirty = 0;
        snprintf(status, sizeof(status), "saved %s", path);
        snprintf(m, sizeof(m), "Saved:\n%s (%dx%d)", path, canvas->w, canvas->h);
        ui_msgbox("Paint", m, UI_MB_OK | UI_MB_INFO);
        return 0;
    }
    snprintf(status, sizeof(status), "cannot save %s", path);
    snprintf(m, sizeof(m), "Cannot save %s:\n%s", path, strerror(errno));
    ui_msgbox("Paint", m, UI_MB_OK | UI_MB_ERROR);
    return -1;
}

/* বদল থাকলে জিজ্ঞেস করা; 1 = এগোনো যায় */
static int may_discard(void)
{
    int a;
    commit_text();
    if (!dirty) return 1;
    a = ui_msgbox("Paint", "Save changes to the picture?", UI_MB_YESNOCANCEL | UI_MB_QUESTION);
    if (a == UI_YES) return save(0) == 0;
    return a == UI_NO;
}

static void load_into_canvas(gfx_surface *img)
{
    gfx_surface *n = gfx_surface_new(img->w > canvas->w ? img->w : canvas->w, img->h > canvas->h ? img->h : canvas->h);
    if (!n) return;
    gfx_clear(n, GFX_WHITE);
    gfx_blit(n, 0, 0, img, 0, 0, -1, -1);
    replace_canvas(n);
    free_stack(undo_st, &undo_n);
    free_stack(redo_st, &redo_n);
}

static void open_picture(void)
{
    char p[512];
    gfx_surface *img;
    if (!may_discard()) return;
    snprintf(p, sizeof(p), "%s", path);
    if (!ui_filedlg("Open picture", p, sizeof(p), 0)) return;
    img = gfx_load_image(p);
    if (!img) {
        char m[700];
        snprintf(m, sizeof(m), "Cannot open %s:\n%s", p, gfx_image_error() ? gfx_image_error() : "unknown format");
        ui_msgbox("Paint", m, UI_MB_OK | UI_MB_ERROR);
        return;
    }
    load_into_canvas(img);
    gfx_surface_free(img);
    snprintf(path, sizeof(path), "%s", p);
    force_bmp(path, sizeof(path));             /* .jpg খুললে সংরক্ষণ হবে একই নামের .png-তে */
    named = 1;
    dirty = 0;
    snprintf(status, sizeof(status), "opened %s", p);
}

static void new_picture(int w, int h)
{
    gfx_surface *n;
    if (!may_discard()) return;
    n = gfx_surface_new(w > 0 ? w : 1, h > 0 ? h : 1);
    if (!n) return;
    gfx_clear(n, GFX_WHITE);
    replace_canvas(n);
    free_stack(undo_st, &undo_n);
    free_stack(redo_st, &redo_n);
    snprintf(path, sizeof(path), "/home/drawing.png");
    named = 0;
    dirty = 0;
    snprintf(status, sizeof(status), "new picture %dx%d", n->w, n->h);
}

static void clear_picture(void)
{
    commit_text();
    push_undo();
    gfx_clear(canvas, GFX_WHITE);
    snprintf(status, sizeof(status), "cleared (Ctrl+Z brings it back)");
}

/* ---------- টুলবারের ছোট ছবি ---------- */

static void tool_icon(gfx_surface *s, int t, int x, int y, uint32_t c)
{
    switch (t) {
    case T_PENCIL:  gfx_line(s, x + 4, y + 16, x + 16, y + 4, c); gfx_line(s, x + 5, y + 16, x + 16, y + 5, c);
                    gfx_fill_triangle(s, x + 3, y + 17, x + 3, y + 14, x + 6, y + 17, c); break;
    case T_BRUSH:   gfx_thick_line(s, x + 9, y + 11, x + 16, y + 3, 3, c); gfx_fill_circle(s, x + 6, y + 14, 4, c); break;
    case T_ERASER:  gfx_fill_rect(s, x + 4, y + 8, 12, 8, c); gfx_rect(s, x + 4, y + 4, 12, 12, c); break;
    case T_FILL:    gfx_rect(s, x + 3, y + 7, 10, 9, c); gfx_fill_rect(s, x + 4, y + 11, 8, 5, c);
                    gfx_fill_circle(s, x + 16, y + 13, 2, c); break;
    case T_PICK:    gfx_thick_line(s, x + 4, y + 16, x + 13, y + 7, 2, c); gfx_fill_circle(s, x + 15, y + 5, 3, c); break;
    case T_LINE:    gfx_thick_line(s, x + 3, y + 16, x + 17, y + 4, 2, c); break;
    case T_RECT:    gfx_rect(s, x + 3, y + 5, 14, 10, c); gfx_rect(s, x + 4, y + 6, 12, 8, c); break;
    case T_ELLIPSE: gfx_ellipse(s, x + 10, y + 10, 7, 5, c); gfx_ellipse(s, x + 10, y + 10, 6, 4, c); break;
    case T_TEXT:    gfx_text(s, NULL, x + 6, y + 2, "A", c); gfx_hline(s, x + 4, y + 17, 12, c); break;
    }
}

/* ---------- মেনু ---------- */

enum { M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_QUIT, M_UNDO, M_REDO, M_CLEAR,
       M_FLIPH, M_FLIPV, M_ROT, M_SIZE, M_KEYS };
static const struct ui_menuitem file_items[] = {
    { "New", "Ctrl+N", M_NEW }, { "Open...", "Ctrl+O", M_OPEN }, { "Save", "Ctrl+S", M_SAVE },
    { "Save as...", NULL, M_SAVEAS }, { "-", NULL, 0 }, { "Quit", "Ctrl+Q", M_QUIT }
};
static const struct ui_menuitem edit_items[] = {
    { "Undo", "Ctrl+Z", M_UNDO }, { "Redo", "Ctrl+Y", M_REDO }, { "-", NULL, 0 }, { "Clear picture", NULL, M_CLEAR }
};
static const struct ui_menuitem image_items[] = {
    { "Flip horizontal", NULL, M_FLIPH }, { "Flip vertical", NULL, M_FLIPV },
    { "Rotate 90 right", NULL, M_ROT }, { "-", NULL, 0 }, { "Canvas size...", NULL, M_SIZE }
};
static const struct ui_menuitem help_items[] = { { "Keys and tools", "F1", M_KEYS } };
static const struct ui_menu menus[] = {
    { "File", file_items, 6 }, { "Edit", edit_items, 4 }, { "Image", image_items, 5 }, { "Help", help_items, 1 }
};

static const char *keys_text =
    "Left button: colour 1, right button: colour 2 (white = erase).\n"
    "Tools: p pencil  b brush  e eraser  f fill  i pick colour\n"
    "       l line  r rectangle  g ellipse  t text\n"
    "1-9 colour 1   [ ] or wheel: size   arrows: move a big picture\n"
    "c clear   s save   S save as   o open   q / Esc quit\n"
    "Ctrl+Z undo   Ctrl+Y redo   Ctrl+N new   Ctrl+S save\n"
    "Text: click, type, Enter (or click elsewhere) to place it.\n"
    "Save as name.png (PNG) or name.bmp (BMP).";

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    ui_ctx ui;
    int running = 1, drag = 0, sx0 = 0, sy0 = 0, lx = 0, ly = 0;
    uint32_t drag_col = 0;

    if (argc > 1) { snprintf(path, sizeof(path), "%s", argv[1]); named = 1; }
    if (gfx_init_window("Paint", 0, 0) < 0) {
        perror("paint: /dev/fb0");
        return 1;
    }
    ui_init(&ui);
    ui_ask_before_close(&ui, 1);
    font = gfx_ttf_load("/usr/share/fonts/DejaVuSans.ttf");
    {
        int vw = gfx_width() - TOOLBAR_W, vh = gfx_height() - UI_MENUBAR_H - PALETTE_H - STATUS_H;
        canvas = gfx_surface_new(vw > 1 ? vw : 1, vh > 1 ? vh : 1);
    }
    if (!canvas) { gfx_quit(); return 1; }
    gfx_clear(canvas, GFX_WHITE);
    if (argc > 1) {
        gfx_surface *img = gfx_load_image(path);
        if (img) {
            load_into_canvas(img);
            gfx_surface_free(img);
        }
        force_bmp(path, sizeof(path));
    }

    while (ui_run(&ui)) {
        const struct ui_theme *T = ui_theme_get();
        gfx_surface *s = ui.s;
        int W = ui.w, H = ui.h, x0 = TOOLBAR_W, y0 = UI_MENUBAR_H;
        int vw = W - TOOLBAR_W, vh = H - UI_MENUBAR_H - PALETTE_H - STATUS_H, py = y0 + vh;
        int act, i, cx, cy, over, k = 0;
        char st[200];

        if (vw < 1) vw = 1;
        if (vh < 1) vh = 1;
        /* ধাপ ১৪: উইন্ডো বড় হলে কাগজও বড় (সাদা দিয়ে) */
        if (ui.resized && (vw > canvas->w || vh > canvas->h)) {
            gfx_surface *n = gfx_surface_new(vw > canvas->w ? vw : canvas->w, vh > canvas->h ? vh : canvas->h);
            if (n) {
                gfx_clear(n, GFX_WHITE);
                gfx_blit(n, 0, 0, canvas, 0, 0, -1, -1);
                gfx_surface_free(canvas);
                canvas = n;
            }
        }
        if (scroll_x > canvas->w - vw) scroll_x = canvas->w - vw > 0 ? canvas->w - vw : 0;
        if (scroll_y > canvas->h - vh) scroll_y = canvas->h - vh > 0 ? canvas->h - vh : 0;

        if (ui.closing) {
            if (may_discard()) running = 0;
            ui.closing = 0;
        }

        gfx_clear(s, T->window_bg);
        act = ui_menubar(&ui, 0, 0, W, menus, 4);

        /* ----- লেখা টুলে কী (মেনুর পরে, অন্য সবকিছুর আগে) ----- */
        if (text_on && ui.key && !(ui.mods & GFX_MOD_CTRL)) {
            size_t n = strlen(text_buf);
            if (ui.key == GFX_KEY_ENTER) commit_text();
            else if (ui.key == GFX_KEY_ESC) { text_on = 0; text_buf[0] = '\0'; }
            else if (ui.key == GFX_KEY_BACKSPACE) { if (n) text_buf[n - 1] = '\0'; }
            else if (ui.key >= 32 && ui.key < 127 && n < sizeof(text_buf) - 1) { text_buf[n] = (char)ui.key; text_buf[n + 1] = '\0'; }
            ui.key = 0;
        }

        /* ----- কীবোর্ড (আগের সব কী + নতুন) ----- */
        k = ui.key;
        if (k) {
            if (k == 'q' || k == GFX_KEY_ESC || k == 17) { if (may_discard()) running = 0; }
            else if (k >= '1' && k <= '9') color = palette[k - '1'];
            else if (k == '[') brush = brush > 1 ? brush - 1 : 1;
            else if (k == ']') brush = brush < 40 ? brush + 1 : 40;
            else if (k == 'f') set_tool(tool == T_FILL ? prev_tool : T_FILL);
            else if (k == 'c') clear_picture();
            else if (k == 's') save(0);
            else if (k == 'S') save(1);            /* ধাপ ১৪খ: অন্য নামে */
            else if (k == 'o') open_picture();
            else if (k == 'p') set_tool(T_PENCIL);
            else if (k == 'b') set_tool(T_BRUSH);
            else if (k == 'e') set_tool(T_ERASER);
            else if (k == 'i') set_tool(T_PICK);
            else if (k == 'l') set_tool(T_LINE);
            else if (k == 'r') set_tool(T_RECT);
            else if (k == 'g') set_tool(T_ELLIPSE);
            else if (k == 't') set_tool(T_TEXT);
            else if (k == GFX_KEY_LEFT) scroll_x = scroll_x > 40 ? scroll_x - 40 : 0;
            else if (k == GFX_KEY_RIGHT) scroll_x += 40;
            else if (k == GFX_KEY_UP) scroll_y = scroll_y > 40 ? scroll_y - 40 : 0;
            else if (k == GFX_KEY_DOWN) scroll_y += 40;
        }
        if (ui.wheel && !ui_hover(&ui, 0, py, W, PALETTE_H + STATUS_H)) {
            brush += ui.wheel < 0 ? 1 : -1;
            if (brush < 1) brush = 1;
            if (brush > 40) brush = 40;
        }

        switch (act) {
        case M_NEW: new_picture(vw, vh); break;
        case M_OPEN: open_picture(); break;
        case M_SAVE: save(0); break;
        case M_SAVEAS: save(1); break;
        case M_QUIT: if (may_discard()) running = 0; break;
        case M_UNDO: commit_text(); undo(); break;
        case M_REDO: redo(); break;
        case M_CLEAR: clear_picture(); break;
        case M_FLIPH: commit_text(); flip(1); break;
        case M_FLIPV: commit_text(); flip(0); break;
        case M_ROT: commit_text(); rotate_right(); break;
        case M_SIZE: commit_text(); canvas_size(); break;
        case M_KEYS: ui_msgbox("Paint - keys", keys_text, UI_MB_OK | UI_MB_INFO); break;
        }

        /* ----- কাগজ ----- */
        gfx_fill_rect(s, x0, y0, vw, vh, GFX_RGB(120, 124, 130));
        gfx_set_clip(s, x0, y0, vw, vh);
        gfx_blit(s, x0, y0, canvas, scroll_x, scroll_y, canvas->w - scroll_x, canvas->h - scroll_y);
        over = ui_hover(&ui, x0, y0, vw, vh);
        cx = ui.mx - x0 + scroll_x;
        cy = ui.my - y0 + scroll_y;

        /* চাপ শুরু */
        if (!drag && over && (ui.pressed || ui.rpressed)) {
            int right = ui.rpressed && !ui.pressed;
            uint32_t c = right ? color2 : color;
            if (tool == T_FILL) {
                push_undo();
                gfx_flood_fill(canvas, cx, cy, c);
            } else if (tool == T_PICK) {
                uint32_t p = gfx_get_pixel(canvas, cx, cy) | 0xFF000000u;
                if (right) color2 = p; else color = p;
                set_tool(prev_tool);
            } else if (tool == T_TEXT) {
                commit_text();
                text_on = 1;
                text_x = cx;
                text_y = cy;
                text_buf[0] = '\0';
                snprintf(status, sizeof(status), "type the text, Enter places it, Esc cancels");
            } else {
                push_undo();
                drag = right ? GFX_BUTTON_RIGHT : GFX_BUTTON_LEFT;
                drag_col = c;
                sx0 = lx = cx;
                sy0 = ly = cy;
                if (tool == T_PENCIL || tool == T_BRUSH || tool == T_ERASER) stroke(tool, cx, cy, cx, cy, c);
            }
        } else if (drag && (ui.buttons & drag)) {               /* টানা চলছে */
            if (tool == T_PENCIL || tool == T_BRUSH || tool == T_ERASER) {
                if (cx != lx || cy != ly) stroke(tool, lx, ly, cx, cy, drag_col);
                lx = cx;
                ly = cy;
            } else {
                draw_shape(s, tool, sx0, sy0, cx, cy, x0 - scroll_x, y0 - scroll_y, drag_col);   /* প্রিভিউ */
            }
        } else if (drag) {                                      /* ছাড়া হলো */
            if (tool == T_LINE || tool == T_RECT || tool == T_ELLIPSE) {
                draw_shape(canvas, tool, sx0, sy0, cx, cy, 0, 0, drag_col);
                dirty = 1;
            }
            drag = 0;
        }
        if (text_on) {                                          /* লেখার প্রিভিউ + কার্সর */
            int ex = x0 - scroll_x + text_x, ey = y0 - scroll_y + text_y, tw;
            if (font) tw = gfx_ttf_text(s, font, (float)text_size, ex, ey, text_buf, color);
            else tw = gfx_text(s, NULL, ex, ey, text_buf, color);
            if (ui.blink) gfx_vline(s, tw + 1, ey, text_size, GFX_RGB(255, 0, 0));
            gfx_rect(s, ex - 2, ey - 2, (tw - ex) + 6, text_size + 4, GFX_RGB(80, 80, 200));
        }
        gfx_reset_clip(s);

        /* ----- বাঁ পাশের টুলবার ----- */
        ui_panel(s, 0, y0, TOOLBAR_W, vh, T->face, 1);
        for (i = 0; i < T_COUNT; i++) {
            int bx = 4 + (i % 2) * 29, by = y0 + 4 + (i / 2) * 29;
            if (ui_do_button(&ui, bx, by, 27, 27, "")) set_tool(i);
            if (i == tool) {
                gfx_fill_rect(s, bx + 2, by + 2, 23, 23, T->select);
                gfx_rect(s, bx, by, 27, 27, T->accent);
            }
            tool_icon(s, i, bx + 3, by + 3, T->text);
        }
        {
            int oy = y0 + 4 + ((T_COUNT + 1) / 2) * 29 + 6;
            ui_separator(s, 4, oy - 4, TOOLBAR_W - 8);
            if (tool == T_TEXT) {
                for (i = 0; i < 5; i++) {
                    char b[8];
                    int by = oy + i * 20;
                    snprintf(b, sizeof(b), "%d", text_sizes[i]);
                    if (ui_clicked(&ui, 4, by, TOOLBAR_W - 8, 18)) text_size = text_sizes[i];
                    if (text_size == text_sizes[i]) gfx_fill_rect(s, 4, by, TOOLBAR_W - 8, 18, T->select);
                    ui_label(s, 12, by + 1, b, T->text);
                }
            } else {
                for (i = 0; i < 4; i++) {
                    int by = oy + i * 18;
                    if (ui_clicked(&ui, 4, by, TOOLBAR_W - 8, 16)) brush = sizes[i];
                    if (brush == sizes[i]) gfx_fill_rect(s, 4, by, TOOLBAR_W - 8, 16, T->select);
                    gfx_fill_rect(s, 10, by + 8 - sizes[i] / 2, TOOLBAR_W - 20, sizes[i], T->text);
                }
                if (tool == T_RECT || tool == T_ELLIPSE) {
                    int by = oy + 4 * 18 + 6;
                    if (ui_clicked(&ui, 4, by, 26, 22)) shape_fill = 0;
                    if (ui_clicked(&ui, 34, by, 26, 22)) shape_fill = 1;
                    gfx_fill_rect(s, shape_fill ? 34 : 4, by, 26, 22, T->select);
                    gfx_rect(s, 8, by + 4, 18, 14, T->text);
                    gfx_fill_rect(s, 38, by + 4, 18, 14, T->text);
                }
            }
        }

        /* ----- নিচে রঙ ----- */
        ui_panel(s, 0, py, W, PALETTE_H, T->face, 1);
        gfx_fill_rect(s, 20, py + 18, 22, 22, color2);
        gfx_rect(s, 20, py + 18, 22, 22, T->dark);
        gfx_fill_rect(s, 8, py + 6, 22, 22, color);
        gfx_rect(s, 8, py + 6, 22, 22, T->light);
        for (i = 0; i < 20; i++) {
            int bx = 54 + (i % 10) * 21, by = py + 4 + (i / 10) * 21;
            gfx_fill_rect(s, bx, by, 19, 19, palette[i]);
            gfx_rect(s, bx, by, 19, 19, T->dark);
            if (ui_hover(&ui, bx, by, 19, 19)) {
                if (ui.pressed) color = palette[i];
                if (ui.rpressed) color2 = palette[i];
            }
        }
        snprintf(st, sizeof(st), "size %d", tool == T_TEXT ? text_size : brush);
        ui_label(s, 54 + 10 * 21 + 12, py + 16, st, T->text);

        /* ----- স্ট্যাটাস ----- */
        {
            const char *name = strrchr(path, '/');
            if (over) snprintf(st, sizeof(st), "%d,%d   %dx%d   %s%s   %s", cx, cy, canvas->w, canvas->h,
                               name ? name + 1 : path, dirty ? " *" : "", status);
            else snprintf(st, sizeof(st), "%dx%d   %s%s   %s", canvas->w, canvas->h, name ? name + 1 : path,
                          dirty ? " *" : "", status);
            ui_statusbar(s, 0, H - STATUS_H, W, STATUS_H, st);
        }
        if (!gfx_windowed()) gfx_cursor_draw(s, ui.mx, ui.my);   /* পুরো পর্দায় নিজের মাউস-তীর (আগের মতো) */
        if (!running) ui_quit(&ui);
    }
    free_stack(undo_st, &undo_n);
    free_stack(redo_st, &redo_n);
    gfx_ttf_free(font);
    gfx_surface_free(canvas);
    gfx_quit();
    return 0;
}
