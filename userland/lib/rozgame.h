/* rozOS — librozgame: গেম বানানোর লাইব্রেরি (ধাপ ১৮ক), librozgfx-এর ওপরে
 *
 * কম্পাইল:  tcc mygame.c -lrozgame -lrozgfx -lm -o mygame
 *
 * দুইভাবে লেখা যায়:
 *
 * ১. callback (নির্দিষ্ট হারে update — মেশিন ধীর হলেও গেমের গতি একই):
 *      static void update(void *ud, float dt) { ... game_key_down(GFX_KEY_LEFT) ... }
 *      static void draw(void *ud, gfx_surface *s) { game_clear(GFX_BLACK); ... }
 *      int main(void) {
 *          game_config c = game_default_config("My game");
 *          game_app a = { NULL, update, draw, NULL, NULL };
 *          return game_run(&c, &a);
 *      }
 *
 * ২. নিজের লুপ:
 *      game_open(&c);
 *      while (game_begin()) { float dt = game_dt(); ...আঁকা...; game_end(); }
 *      game_close();
 *
 * পর্দা: গেম আঁকে একটা "লজিক্যাল" পর্দায় (ডিফল্ট 640x480), লাইব্রেরি সেটা
 * উইন্ডো বা পুরো পর্দার মাপে বড় করে (অনুপাত ঠিক রেখে, দুই পাশে কালো ফিতা)।
 * উইন্ডো লজিক্যাল মাপের সমান হলে কোনো বাড়তি কপি হয় না — সবচেয়ে দ্রুত।
 * GAME_SCALE_NATIVE দিলে লজিক্যাল পর্দা = উইন্ডোর আসল মাপ (বড় ছবি/ফটোর জন্য পূর্ণ রেজোলিউশন)।
 *
 * ছবি: game_image — PNG, JPG, BMP, GIF, TGA, PPM। লোড করার সময় প্রতিটা সারি দেখে রাখা
 * হয় (পুরো স্বচ্ছ / পুরো অস্বচ্ছ / মেশানো), তাই অস্বচ্ছ অংশ সরাসরি memcpy-তে আঁকা হয়।
 * বড় ফটো আঁকার মাপে একবার game_image_resize(..., 1) দিয়ে সুন্দরভাবে ছোট/বড় করে নাও,
 * তারপর প্রতি ফ্রেমে শুধু কপি — দ্রুত আর ঝকঝকে। */
#ifndef ROZGAME_H
#define ROZGAME_H

#include <stdint.h>
#include <math.h>              /* গেমে sinf/cosf/sqrtf প্রায় সবসময় লাগে */
#include "rozgfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- কনফিগ ও লুপ ---------- */

enum {
    GAME_SCALE_FIT = 0,      /* অনুপাত ঠিক রেখে বড়/ছোট (ডিফল্ট) */
    GAME_SCALE_INTEGER,      /* শুধু পূর্ণসংখ্যা গুণ (1x, 2x, 3x) — pixel-art একদম সমান */
    GAME_SCALE_STRETCH,      /* পুরো জায়গা ভরে (অনুপাত বদলাতে পারে) */
    GAME_SCALE_NATIVE        /* লজিক্যাল = উইন্ডোর মাপ (মাপ বদলালে পর্দাও বদলায়) */
};

typedef struct game_config {
    const char *title;
    int width, height;       /* লজিক্যাল মাপ; 0 = 640x480 */
    int scale;               /* GAME_SCALE_* */
    int smooth;              /* 1 = বড় করার সময় মসৃণ (bilinear) — ধীর; ফটো-গেমে কাজে লাগে */
    int update_hz;           /* game_run-এর update কতবার/সেকেন্ড; 0 = 60 */
    int max_fps;             /* 0 = 60; −1 = সীমা নেই */
    int resizable;           /* ভবিষ্যতের জন্য — এখন rozwm সবসময় মাপ বদলাতে দেয় (গেম নিজে মানিয়ে নেয়) */
    int window_w, window_h;  /* উইন্ডোর শুরুর মাপ; 0 = লজিক্যাল মাপ */
    uint32_t border_color;   /* দুই পাশের ফিতার রং (ডিফল্ট কালো) */
    int show_fps;            /* 1 = কোণে fps দেখাও */
} game_config;

typedef struct game_app {
    int  (*init)(void *ud);                      /* 0 = ঠিক আছে; NULL চলে */
    void (*update)(void *ud, float dt);          /* নির্দিষ্ট dt (1/update_hz সেকেন্ড) */
    void (*draw)(void *ud, gfx_surface *screen); /* লজিক্যাল পর্দায় আঁকো */
    void (*quit)(void *ud);
    void *ud;                                    /* তোমার নিজের ডেটা */
} game_app;

game_config game_default_config(const char *title);
int  game_run(const game_config *cfg, const game_app *app);   /* শেষে 0 */

int  game_open(const game_config *cfg);   /* 0 = ঠিক আছে, −1 = গ্রাফিক্স নেই */
int  game_begin(void);                    /* নতুন ফ্রেম: ঘটনা পড়া; 0 = বন্ধ করতে হবে */
void game_end(void);                      /* পর্দায় দেখানো + fps সীমা */
void game_close(void);
void game_quit(void);                     /* পরের game_begin() 0 দেবে */
int  game_running(void);

gfx_surface *game_screen(void);           /* লজিক্যাল পর্দা */
int   game_width(void);
int   game_height(void);
float game_dt(void);                      /* আগের ফ্রেম থেকে সেকেন্ড (সর্বোচ্চ 0.1) */
double game_time(void);                   /* শুরু থেকে সেকেন্ড */
float game_fps(void);
long  game_frame_count(void);
int   game_focused(void);                 /* উইন্ডো সামনে আছে কিনা */
void  game_set_title(const char *title);
int   game_screenshot(const char *path);  /* লজিক্যাল পর্দা .bmp; 0 = ঠিক আছে */

/* ---------- ইনপুট ---------- */
/* কী-কোড: ছোট হাতের অক্ষর ('a'), অঙ্ক ('1'), ' ', GFX_KEY_ENTER, GFX_KEY_ESC,
 * GFX_KEY_UP/DOWN/LEFT/RIGHT, GFX_KEY_F1…, GFX_KEY_LSHIFT/LCTRL/LALT … */
int  game_key_down(int key);              /* এখন চাপা আছে */
int  game_key_pressed(int key);           /* এই ফ্রেমে চাপা হলো (repeat নয়) */
int  game_key_released(int key);          /* এই ফ্রেমে ছাড়া হলো */
int  game_key_typed(int key);             /* চাপা বা চেপে রাখায় repeat — মেনুর জন্য */
int  game_any_key_pressed(void);          /* কোনো কী চাপা হলো (0 = না, নইলে কোড) */
int  game_text_input(char *buf, int size);/* এই ফ্রেমে টাইপ করা অক্ষর (UTF-8 নয়, ASCII) */
int  game_raw_keys(void);                 /* 1 = আসল চাপা/ছাড়া পাওয়া যাচ্ছে */

int  game_mouse_x(void);                  /* লজিক্যাল পর্দায় */
int  game_mouse_y(void);
int  game_mouse_down(int button);         /* GFX_BUTTON_LEFT/RIGHT/MIDDLE */
int  game_mouse_pressed(int button);
int  game_mouse_released(int button);
int  game_mouse_wheel(void);              /* এই ফ্রেমে: −1 ওপরে, +1 নিচে, 0 */
int  game_mouse_inside(void);             /* মাউস লজিক্যাল পর্দার ভেতরে */

/* ---------- ছবি ---------- */

typedef struct game_image {
    int w, h;
    gfx_surface *surf;
    unsigned char *rows;      /* প্রতি সারি: 0 পুরো স্বচ্ছ, 1 পুরো অস্বচ্ছ, 2 মেশানো */
    int opaque;               /* সব সারি অস্বচ্ছ */
} game_image;

enum { GAME_FLIP_X = 1, GAME_FLIP_Y = 2 };

game_image *game_image_load(const char *path);                 /* ব্যর্থ হলে NULL */
game_image *game_image_new(int w, int h);                      /* স্বচ্ছ; আঁকার পরে game_image_update */
game_image *game_image_from_surface(gfx_surface *s);           /* s-এর মালিকানা নিয়ে নেয় */
game_image *game_image_resize(const game_image *img, int w, int h, int smooth);   /* নতুন ছবি */
game_image *game_image_sub(const game_image *img, int x, int y, int w, int h);    /* নতুন ছবি (কপি) */
void game_image_update(game_image *img);                       /* পিক্সেল বদলালে আবার দেখে নেওয়া */
void game_image_colorkey(game_image *img, uint32_t rgb);       /* এই রং → স্বচ্ছ */
void game_image_free(game_image *img);

void game_draw_image(const game_image *img, int x, int y);
void game_draw_image_ex(const game_image *img, int sx, int sy, int sw, int sh,
                        int x, int y, int flags);              /* অংশ + উল্টানো */
void game_draw_image_scaled(const game_image *img, int sx, int sy, int sw, int sh,
                            int x, int y, int w, int h, int flags);
void game_draw_image_alpha(const game_image *img, int x, int y, int alpha);   /* 0-255 আবছা */
void game_draw_image_rotated(const game_image *img, int cx, int cy, float angle, float scale);
                                                               /* কেন্দ্র (cx,cy), রেডিয়ানে */
void game_draw_background(const game_image *img);              /* পুরো পর্দা ভরে (মাপ না মিললে মাপমতো) */

/* sprite sheet: এক ছবিতে সমান মাপের অনেক ফ্রেম, বাঁ থেকে ডানে, ওপর থেকে নিচে */
typedef struct game_sheet {
    game_image *img;
    int fw, fh, cols, rows, count;
} game_sheet;

game_sheet *game_sheet_load(const char *path, int frame_w, int frame_h);
game_sheet *game_sheet_new(game_image *img, int frame_w, int frame_h);   /* img-এর মালিকানা নেয় */
void game_sheet_free(game_sheet *sh);
void game_draw_frame(const game_sheet *sh, int frame, int x, int y, int flags);
void game_draw_frame_scaled(const game_sheet *sh, int frame, int x, int y, int w, int h, int flags);

typedef struct game_anim {
    int first, count;         /* sheet-এর কোন ফ্রেম থেকে কয়টা */
    float fps;
    int loop;                 /* 0 = একবার চলে শেষ ফ্রেমে থামে */
    float t;
    int done;
} game_anim;

void game_anim_set(game_anim *a, int first, int count, float fps, int loop);   /* একই হলে আবার শুরু নয় */
void game_anim_restart(game_anim *a);
void game_anim_update(game_anim *a, float dt);
int  game_anim_frame(const game_anim *a);

/* ---------- আঁকার সহজ রূপ (লজিক্যাল পর্দায়) ---------- */

void game_clear(uint32_t color);
void game_fill_rect(int x, int y, int w, int h, uint32_t color);    /* alpha < 255 হলে মেশায় */
void game_draw_rect(int x, int y, int w, int h, uint32_t color);   /* শুধু রেখা */
void game_fill_circle(int cx, int cy, int r, uint32_t color);
void game_line(int x0, int y0, int x1, int y1, uint32_t color);
void game_gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

/* ---------- লেখা ---------- */

int  game_text(int x, int y, const char *str, uint32_t color);           /* PSF ফন্ট; চওড়া ফেরত */
int  game_text_width(const char *str);
int  game_text_height(void);
void game_text_center(int y, const char *str, uint32_t color);
void game_text_shadow(int x, int y, const char *str, uint32_t color);   /* ছায়াসহ */
/* TrueType (DejaVuSans): যেকোনো মাপ; font = NULL মানে ডিফল্ট */
gfx_ttf *game_font_load(const char *path);
int  game_ttf(gfx_ttf *font, float size, int x, int y, const char *utf8, uint32_t color);
int  game_ttf_width(gfx_ttf *font, float size, const char *utf8);
void game_ttf_center(gfx_ttf *font, float size, int y, const char *utf8, uint32_t color);
int  game_printf(int x, int y, uint32_t color, const char *fmt, ...);

/* ছোট মেনু: ওপর/নিচ (W/S), Enter/Space বা ক্লিকে বাছাই।
 * game_run-এ: update-এ game_menu_update (বাছাই → index, নইলে −1), draw-এ game_menu_draw।
 * নিজের লুপে: game_menu_do = দুটো একসাথে। */
typedef struct game_menu {
    const char *const *items;
    int count, sel;
    float size;                /* TTF মাপ; 0 = 28 */
    uint32_t color, sel_color;
    int y;                     /* শেষবার কোথায় আঁকা হয়েছিল (মাউসের জন্য; নিজে বসে) */
    int last_mx, last_my;
} game_menu;
int  game_menu_update(game_menu *m);
void game_menu_draw(game_menu *m, int y);
int  game_menu_do(game_menu *m, int y);

/* ---------- সংঘর্ষ ---------- */

typedef struct game_rect { float x, y, w, h; } game_rect;
enum { GAME_SIDE_NONE = 0, GAME_SIDE_LEFT, GAME_SIDE_RIGHT, GAME_SIDE_TOP, GAME_SIDE_BOTTOM };

game_rect game_make_rect(float x, float y, float w, float h);
int  game_rect_overlap(game_rect a, game_rect b);
int  game_rect_contains(game_rect r, float x, float y);
game_rect game_rect_intersect(game_rect a, game_rect b);           /* না ছুঁলে w = h = 0 */
int  game_circle_overlap(float x1, float y1, float r1, float x2, float y2, float r2);
int  game_circle_rect(float cx, float cy, float r, game_rect b);
/* a কোন দিক দিয়ে b-তে ঢুকেছে (সবচেয়ে কম ঢোকা অক্ষ ধরে) — বল আর ইটের জন্য।
 * a-র কোন পাশ ধাক্কা খেয়েছে: GAME_SIDE_LEFT মানে a-র বাঁ পাশ (তাই x-গতি উল্টাও) */
int  game_rect_hit_side(game_rect a, game_rect b);
/* a-কে b থেকে বের করে দেওয়া (কম দূরত্বের দিকে); কোন পাশ দিয়ে ফেরত */
int  game_rect_push_out(game_rect *a, game_rect b);

/* ---------- সহায়ক ---------- */

void     game_rand_seed(uint32_t seed);          /* না ডাকলে /dev/urandom থেকে */
uint32_t game_rand(void);
int      game_rand_int(int lo, int hi);          /* lo থেকে hi, দুটোই সহ */
float    game_rand_float(float lo, float hi);
float    game_clamp(float v, float lo, float hi);
float    game_lerp(float a, float b, float t);
float    game_approach(float v, float target, float step);   /* step পরিমাণ করে target-এর দিকে */
float    game_dist(float x1, float y1, float x2, float y2);
int      game_every(float *acc, float interval, float dt);   /* interval পার হলে 1 (জমা রেখে) */
uint32_t game_color_lerp(uint32_t a, uint32_t b, float t);
uint32_t game_hsv(float h, float s, float v);               /* h 0-360 */

/* সর্বোচ্চ স্কোর: $HOME/.rozgame/<name>.score (HOME না থাকলে /home) */
long game_score_load(const char *name);
int  game_score_save(const char *name, long score);         /* বেশি হলে তবেই লেখে; 1 = নতুন রেকর্ড */

/* গেমের ফাইল খোঁজা: আগে বর্তমান ফোল্ডার, তারপর /usr/share/games/ — পথ ফেরত (static বাফার) */
const char *game_data_path(const char *file);

/* ভেতরের পরীক্ষার জন্য (systest): একটা ঘটনা নিজে ঢোকানো, তারপর game_input_frame() */
void game_inject_event(const gfx_event *ev);
void game_input_frame(void);

#ifdef __cplusplus
}
#endif

#endif
