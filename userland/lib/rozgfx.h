/* rozOS — librozgfx: সহজ গ্রাফিক্স লাইব্রেরি (ধাপ ১৩)
 *
 *   #include <rozgfx.h>
 *   int main(void) {
 *       gfx_surface *s;
 *       gfx_event ev;
 *       if (gfx_init() < 0) return 1;            // /dev/fb0 খোলা, পর্দা গ্রাফিক্স মোডে
 *       s = gfx_screen();                        // আঁকার জায়গা (back buffer)
 *       gfx_clear(s, GFX_RGB(20, 30, 60));
 *       gfx_fill_circle(s, 200, 150, 80, GFX_RGB(255, 200, 0));
 *       gfx_text(s, NULL, 20, 20, "Hello, rozOS!", GFX_WHITE);
 *       gfx_present();                           // back buffer → পর্দা
 *       while (gfx_wait_event(&ev, -1) && !(ev.type == GFX_EV_KEY && ev.key == 'q')) { }
 *       gfx_quit();                              // টেক্সট কনসোল ফেরত
 *       return 0;
 *   }
 *   কম্পাইল (rozOS-এ):  tcc demo.c -lrozgfx -o demo
 *
 * রঙ: 0xAARRGGBB (GFX_RGB / GFX_RGBA)। alpha শুধু *_alpha ফাংশন আর TrueType-এ মানে রাখে;
 * সাধারণ আঁকায় উপেক্ষিত। সব আঁকা surface-এর clip আয়তক্ষেত্রে কাটা হয় (বাইরে কিছু হয় না)।
 * পর্দার পিক্সেল-বিন্যাস (RGB/BGR) gfx_present নিজে মেলায়।
 *
 * ইনপুট: কীবোর্ড /dev/console থেকে (QEMU-র উইন্ডো আর serial টার্মিনাল দুটোই), মাউস
 * /dev/mouse থেকে। gfx_poll_event / gfx_wait_event দুটোই poll() দিয়ে অপেক্ষা করে।
 * প্রোগ্রাম শেষ হলে (crash হলেও) কার্নেল টেক্সট কনসোল নিজেই ফেরায়।
 *
 * ধাপ ১৪: ডেস্কটপ (rozwm) চললে gfx_init() পুরো পর্দা নয়, একটা উইন্ডো খোলে — একই কোড
 * দুই জায়গাতেই চলে। [X] চাপলে GFX_EV_CLOSE আসে (৩ সেকেন্ডে বন্ধ না হলে rozwm বন্ধ করে)।
 * উইন্ডোর মাপ বদলালে GFX_EV_RESIZE — তারপর gfx_width()/gfx_height() নতুন মাপ দেয়।
 *
 * ছবি: BMP (24/32 বিট, সংকোচনহীন) আর PPM (P6/P3) পড়া; BMP লেখা। ধাপ ১৪: JPEG, PNG, GIF,
 * TGA-ও (stb_image)।
 * লেখা: PSF বিটম্যাপ ফন্ট (gfx_text, ডিফল্ট /usr/share/fonts/rozos-mono-16.psf) আর
 * TrueType (gfx_ttf_*, stb_truetype — যেকোনো মাপে, মসৃণ ধার)। UTF-8 পড়ে, কিন্তু বাংলার মতো
 * যুক্তাক্ষর সাজানো (shaping) এখনো নেই। */
#ifndef ROZGFX_H
#define ROZGFX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_RGBA(r, g, b, a) ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define GFX_RGB(r, g, b)     GFX_RGBA(r, g, b, 255)
#define GFX_R(c) (((c) >> 16) & 0xFF)
#define GFX_G(c) (((c) >> 8) & 0xFF)
#define GFX_B(c) ((c) & 0xFF)
#define GFX_A(c) (((c) >> 24) & 0xFF)

#define GFX_BLACK   GFX_RGB(0, 0, 0)
#define GFX_WHITE   GFX_RGB(255, 255, 255)
#define GFX_RED     GFX_RGB(220, 40, 40)
#define GFX_GREEN   GFX_RGB(40, 180, 70)
#define GFX_BLUE    GFX_RGB(40, 90, 220)
#define GFX_YELLOW  GFX_RGB(250, 210, 40)
#define GFX_GRAY    GFX_RGB(128, 128, 128)

typedef struct gfx_surface {
    int w, h;
    int stride;                    /* এক লাইনে কয়টা পিক্সেল (≥ w) */
    uint32_t *px;                  /* px[y * stride + x] = 0xAARRGGBB */
    int clip_x, clip_y, clip_w, clip_h;
    int owns_pixels;
} gfx_surface;

/* ---------- পর্দা ---------- */
int  gfx_init(void);               /* 0 = ঠিক আছে, -1 = /dev/fb0 নেই (errno) */
/* ধাপ ১৪: শিরোনাম আর মাপসহ উইন্ডো (ডেস্কটপ না থাকলে পুরো পর্দা; w/h 0 = নিজে বাছো) */
int  gfx_init_window(const char *title, int w, int h);
int  gfx_init_fullscreen(void);    /* সবসময় সরাসরি /dev/fb0 (rozwm নিজে এটা ব্যবহার করে) */
int  gfx_windowed(void);           /* 1 = rozwm-এর উইন্ডোতে আছি */
void gfx_set_title(const char *title);
/* ধাপ ১৪খ: GFX_EV_CLOSE পেয়ে যদি আগে প্রশ্ন করো ("Save changes?"), এটা ডাকো — নইলে
 * rozwm ৩ সেকেন্ড পরে প্রোগ্রাম বন্ধ করে দেয়। পরে নিজে gfx_quit()/exit করো। */
void gfx_keep_open(void);
/* poll()-এ নিজের fd-এর সাথে নজর রাখার জন্য (যেমন টার্মিনালের PTY): ঘটনার fd-গুলো;
 * এগুলোর কোনোটায় POLLIN হলে gfx_poll_event(&ev, 0) দিয়ে ঘটনা তুলে নাও। ফেরত: কয়টা */
int  gfx_event_fds(int *fds, int max);
/* ধাপ ১৪ (পুরো-পর্দা মোডে): কী শুধু PS/2 কিবোর্ড থেকে (/dev/kbd), serial বাদ — rozwm-কে
 * init এভাবে চালায়, যাতে serial-এ আলাদা shell থাকতে পারে। gfx_init-এর আগে ডাকো। */
void gfx_input_keyboard_only(int on);
void gfx_quit(void);
gfx_surface *gfx_screen(void);     /* back buffer — আঁকা দেখাতে gfx_present() */
int  gfx_width(void);
int  gfx_height(void);
void gfx_present(void);
void gfx_present_rect(int x, int y, int w, int h);

/* ---------- surface ---------- */
gfx_surface *gfx_surface_new(int w, int h);              /* শূন্য (কালো, alpha 0) */
gfx_surface *gfx_surface_wrap(uint32_t *px, int w, int h, int stride);   /* নিজের মেমোরি */
void gfx_surface_free(gfx_surface *s);
void gfx_set_clip(gfx_surface *s, int x, int y, int w, int h);
void gfx_reset_clip(gfx_surface *s);

/* ---------- আঁকা ---------- */
void     gfx_clear(gfx_surface *s, uint32_t color);
void     gfx_pixel(gfx_surface *s, int x, int y, uint32_t color);
uint32_t gfx_get_pixel(const gfx_surface *s, int x, int y);  /* বাইরে হলে 0 */
void     gfx_blend_pixel(gfx_surface *s, int x, int y, uint32_t color);   /* alpha মেশানো */
void gfx_hline(gfx_surface *s, int x, int y, int w, uint32_t color);
void gfx_vline(gfx_surface *s, int x, int y, int h, uint32_t color);
void gfx_line(gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t color);
void gfx_thick_line(gfx_surface *s, int x0, int y0, int x1, int y1, int width, uint32_t color);
void gfx_rect(gfx_surface *s, int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect(gfx_surface *s, int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect_alpha(gfx_surface *s, int x, int y, int w, int h, uint32_t color);
void gfx_round_rect(gfx_surface *s, int x, int y, int w, int h, int r, uint32_t color);
void gfx_fill_round_rect(gfx_surface *s, int x, int y, int w, int h, int r, uint32_t color);
void gfx_gradient_v(gfx_surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_circle(gfx_surface *s, int cx, int cy, int r, uint32_t color);
void gfx_fill_circle(gfx_surface *s, int cx, int cy, int r, uint32_t color);
void gfx_ellipse(gfx_surface *s, int cx, int cy, int rx, int ry, uint32_t color);
void gfx_fill_triangle(gfx_surface *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);
void gfx_flood_fill(gfx_surface *s, int x, int y, uint32_t color);

/* src-এর (sx,sy,w,h) অংশ dst-এর (dx,dy)-তে; w/h < 0 = পুরো src */
void gfx_blit(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h);
void gfx_blit_alpha(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h);
void gfx_blit_scaled(gfx_surface *dst, int dx, int dy, int dw, int dh, const gfx_surface *src);

/* ---------- ছবি ---------- */
gfx_surface *gfx_load_image(const char *path);   /* .bmp .ppm .jpg .png .gif .tga; ব্যর্থ হলে NULL */
const char *gfx_image_error(void);               /* ধাপ ১৪: শেষ ব্যর্থতার কারণ (JPEG/PNG ইত্যাদি) */
gfx_surface *gfx_load_image_stb(const unsigned char *data, long size);   /* ভেতরের কাজে */
int gfx_save_bmp(const gfx_surface *s, const char *path);   /* 0 = ঠিক আছে */
int gfx_save_png(const gfx_surface *s, const char *path);   /* ধাপ ১৯ঘ: PNG (RGB, alpha নয়); 0 = ঠিক আছে */

/* ---------- বিটম্যাপ ফন্ট (PSF) ---------- */
typedef struct gfx_font gfx_font;
gfx_font *gfx_font_load(const char *path);       /* .psf (PSF1/PSF2) */
gfx_font *gfx_font_default(void);                /* rozos-mono-16 (প্রথমবার পড়ে নেয়) */
void gfx_font_free(gfx_font *f);
int  gfx_font_width(const gfx_font *f);          /* একটা অক্ষরের চওড়া */
int  gfx_font_height(const gfx_font *f);
/* font NULL = ডিফল্ট; '\n' নতুন লাইন; ফেরত: শেষ x */
int  gfx_text(gfx_surface *s, gfx_font *font, int x, int y, const char *str, uint32_t color);
int  gfx_text_bg(gfx_surface *s, gfx_font *font, int x, int y, const char *str, uint32_t fg, uint32_t bg);
int  gfx_text_width(gfx_font *font, const char *str);
/* ধাপ ১৪: একটা গ্লিফ সরাসরি নম্বর দিয়ে (0-255, CP437 — টার্মিনালের জন্য), পেছনে bg */
void gfx_glyph(gfx_surface *s, gfx_font *font, int x, int y, int index, uint32_t fg, uint32_t bg);

/* ---------- TrueType ---------- */
typedef struct gfx_ttf gfx_ttf;
gfx_ttf *gfx_ttf_load(const char *path);         /* যেমন /usr/share/fonts/DejaVuSans.ttf */
void gfx_ttf_free(gfx_ttf *t);
/* (x, y) = লেখার বাঁ-উপর কোণ; size = পিক্সেলে লাইনের উচ্চতা; UTF-8; ফেরত: শেষ x */
int  gfx_ttf_text(gfx_surface *s, gfx_ttf *t, float size, int x, int y, const char *utf8, uint32_t color);
int  gfx_ttf_width(gfx_ttf *t, float size, const char *utf8);

/* ---------- ইনপুট ---------- */
enum {
    GFX_EV_NONE = 0,
    GFX_EV_KEY,            /* key: ASCII বা GFX_KEY_* */
    GFX_EV_MOUSE_MOVE,     /* x, y, dx, dy, buttons */
    GFX_EV_MOUSE_DOWN,     /* button: চাপা বোতাম (GFX_BUTTON_*) */
    GFX_EV_MOUSE_UP,
    GFX_EV_MOUSE_WHEEL,    /* wheel: −1 ওপরে, +1 নিচে */
    GFX_EV_CLOSE,          /* ধাপ ১৪: উইন্ডোর [X] — প্রোগ্রাম শেষ করো */
    GFX_EV_RESIZE,         /* ধাপ ১৪: নতুন মাপ x, y-তে (= gfx_width/gfx_height) */
    GFX_EV_FOCUS,          /* ধাপ ১৪: key = 1 সামনে এল, 0 পেছনে গেল */
    GFX_EV_KEY_DOWN,       /* ধাপ ১৮ক: কাঁচা কী চাপা (gfx_raw_keys(1) দিলে) — key = মূল কোড,
                              repeat = 1 মানে চেপে রাখায় আবার এল */
    GFX_EV_KEY_UP          /* ধাপ ১৮ক: কাঁচা কী ছাড়া — key = মূল কোড */
};
enum {
    GFX_KEY_UP = 0x100, GFX_KEY_DOWN, GFX_KEY_LEFT, GFX_KEY_RIGHT,
    GFX_KEY_HOME, GFX_KEY_END, GFX_KEY_PAGE_UP, GFX_KEY_PAGE_DOWN,
    GFX_KEY_INSERT, GFX_KEY_DELETE,
    GFX_KEY_F1, GFX_KEY_F2, GFX_KEY_F3, GFX_KEY_F4, GFX_KEY_F5, GFX_KEY_F6,
    GFX_KEY_F7, GFX_KEY_F8, GFX_KEY_F9, GFX_KEY_F10, GFX_KEY_F11, GFX_KEY_F12
};
/* ধাপ ১৮ক: শুধু GFX_EV_KEY_DOWN/UP-এ আসে (modifier আর lock কী নিজেরাই কী) */
enum {
    GFX_KEY_LSHIFT = 0x120, GFX_KEY_RSHIFT, GFX_KEY_LCTRL, GFX_KEY_RCTRL,
    GFX_KEY_LALT, GFX_KEY_RALT, GFX_KEY_CAPSLOCK, GFX_KEY_NUMLOCK, GFX_KEY_SCROLLLOCK
};
#define GFX_KEY_SPACE ' '
#define GFX_KEY_COUNT 0x140         /* সব কাঁচা কোড এর নিচে (কী-অবস্থার টেবিলের মাপ) */
#define GFX_KEY_ESC       27
#define GFX_KEY_ENTER     '\n'
#define GFX_KEY_BACKSPACE '\b'
#define GFX_KEY_TAB       '\t'
#define GFX_BUTTON_LEFT   0x01
#define GFX_BUTTON_RIGHT  0x02
#define GFX_BUTTON_MIDDLE 0x04

typedef struct gfx_event {
    int type;
    int key;                       /* GFX_EV_KEY */
    int x, y, dx, dy;              /* মাউস: পিক্সেলে */
    int buttons;                   /* এখন চাপা আছে যেগুলো */
    int button;                    /* DOWN/UP: কোনটা বদলাল */
    int wheel;
    int mods;                      /* ধাপ ১৪: GFX_KEY: GFX_MOD_SHIFT/ALT/CTRL (তীর ইত্যাদির সাথে) */
    int repeat;                    /* ধাপ ১৮ক: GFX_EV_KEY_DOWN — auto-repeat */
} gfx_event;
#define GFX_MOD_SHIFT 1
#define GFX_MOD_ALT   2
#define GFX_MOD_CTRL  4

/* timeout_ms: 0 = শুধু দেখা, −1 = চিরকাল; ঘটনা পেলে 1, না পেলে 0 */
int  gfx_poll_event(gfx_event *ev, int timeout_ms);
int  gfx_wait_event(gfx_event *ev, int timeout_ms);   /* gfx_poll_event-এর আরেক নাম */
void gfx_mouse_pos(int *x, int *y);
/* ধাপ ১৮ক: কাঁচা কী-ঘটনা (চাপা + ছাড়া) চালু — গেমের জন্য। টেক্সট মোডে /dev/keyev থেকে,
 * ডেস্কটপে rozwm-এর WM_RAWKEY থেকে। সাধারণ GFX_EV_KEY আগের মতোই আসতে থাকে।
 * ফেরত: 1 = কাঁচা ঘটনা পাওয়া যাবে, 0 = যাবে না (শুধু serial ইত্যাদি) */
int  gfx_raw_keys(int on);
/* মাউস-তীর আঁকা (s-এ); নিচের পিক্সেল gfx_cursor_restore-এর জন্য রেখে দেয় */
void gfx_cursor_draw(gfx_surface *s, int x, int y);
void gfx_cursor_restore(gfx_surface *s);

/* ---------- সময় ---------- */
long gfx_ticks_ms(void);           /* বুট থেকে মিলিসেকেন্ড */
void gfx_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif

#endif
