/* rozOS — librozgame (ধাপ ১৮ক): গেম লুপ, ইনপুট, দ্রুত ছবি আঁকা, sprite, সংঘর্ষ, লেখা
 * (rozgame.h দেখুন)
 *
 * গতির কৌশল:
 * - লজিক্যাল পর্দা উইন্ডোর সমান হলে আলাদা বাফার নেই — সরাসরি rozgfx-এর back buffer-এ আঁকা।
 * - বড় করার সময় প্রতিটা কলামের উৎস আগে থেকে হিসাব করা (xmap); পরপর একই উৎস-সারি হলে
 *   আগের সারি memcpy (2x-এ অর্ধেক সারি শুধু কপি)।
 * - ছবির প্রতিটা সারি আগেই চেনা: পুরো স্বচ্ছ (বাদ), পুরো অস্বচ্ছ (memcpy), মেশানো (মেশাও)।
 * - মেশানো: ভাগ নয়, গুণ আর শিফট (R আর B একসাথে)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rozgame.h"

/* ================= অবস্থা ================= */

static game_config C;
static int opened, running, focused = 1;
static gfx_surface *logical;          /* লজিক্যাল পর্দা */
static int logical_own;               /* 1 = নিজের বাফার (বড়/ছোট করে দেখাতে হয়) */
static int lw, lh;                    /* লজিক্যাল মাপ */
static int ox, oy, ow, oh;            /* পর্দায় কোথায় দেখানো হয় */
static int layout_w = -1, layout_h = -1, need_full_present = 1;
static int *xmap;                     /* উৎস কলাম (nearest) বা 16.16 (smooth) */
static int xmap_n;

static double t_start, t_last, t_frame_start;
static long auto_frames;              /* ROZGAME_FRAMES: এতগুলো ফ্রেমের পরে নিজে বন্ধ (পরীক্ষা/মাপা) */
static const char *auto_shot;         /* ROZGAME_SHOT: বন্ধের সময় লজিক্যাল পর্দার ছবি (.bmp) */
static long auto_shot_at;             /* ROZGAME_SHOT_AT: বন্ধের বদলে এই ফ্রেমে ছবি */
static float dt_last = 1.0f / 60.0f, fps_now;
static long frames, fps_frames;
static double fps_t0;

/* ইনপুট */
static unsigned char kdown[GFX_KEY_COUNT], kpressed[GFX_KEY_COUNT], kreleased[GFX_KEY_COUNT], ktyped[GFX_KEY_COUNT];
static double kfake_until[GFX_KEY_COUNT];   /* কাঁচা ঘটনা না থাকলে: চাপার পরে কতক্ষণ "চাপা" */
static int raw_ok;                    /* gfx_raw_keys সফল */
static double raw_last;               /* শেষ কাঁচা ঘটনার সময় */
static char tbuf[64];
static int tlen;
static int msx, msy, mbuttons, mpressed, mreleased, mwheel;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ================= রং মেশানো ================= */

static uint32_t mix(uint32_t d, uint32_t s)
{
    uint32_t a = s >> 24, ia, rb, g;
    if (a == 255) return s;
    if (a == 0) return d;
    a += a >> 7;                      /* 0..256 */
    ia = 256 - a;
    rb = ((((s & 0xFF00FF) * a) + ((d & 0xFF00FF) * ia)) >> 8) & 0xFF00FF;
    g  = ((((s & 0x00FF00) * a) + ((d & 0x00FF00) * ia)) >> 8) & 0x00FF00;
    return 0xFF000000u | rb | g;
}

static uint32_t mix_alpha(uint32_t d, uint32_t s, uint32_t alpha)
{
    uint32_t a = ((s >> 24) * alpha + 127) / 255;
    return mix(d, (s & 0xFFFFFF) | (a << 24));
}

/* ================= পর্দা সাজানো ================= */

static void free_logical(void)
{
    if (!logical) return;
    if (logical_own) gfx_surface_free(logical);
    else {
        logical->px = NULL;           /* wrap — পিক্সেল rozgfx-এর */
        gfx_surface_free(logical);
    }
    logical = NULL;
}

static void do_layout(void)
{
    gfx_surface *scr = gfx_screen();
    int sw = gfx_width(), sh = gfx_height();
    if (!scr || sw <= 0 || sh <= 0) return;
    layout_w = sw;
    layout_h = sh;

    if (C.scale == GAME_SCALE_NATIVE) {
        lw = sw; lh = sh;
        ox = oy = 0; ow = sw; oh = sh;
    } else {
        lw = C.width; lh = C.height;
        if (C.scale == GAME_SCALE_STRETCH) {
            ow = sw; oh = sh;
        } else {
            double s = (double)sw / lw, s2 = (double)sh / lh;
            if (s2 < s) s = s2;
            if (C.scale == GAME_SCALE_INTEGER && s >= 1.0) s = (double)(int)s;
            ow = (int)(lw * s + 0.5);
            oh = (int)(lh * s + 0.5);
            if (ow > sw) ow = sw;
            if (oh > sh) oh = sh;
            if (ow < 1) ow = 1;
            if (oh < 1) oh = 1;
        }
        ox = (sw - ow) / 2;
        oy = (sh - oh) / 2;
    }

    free_logical();
    if (ow == lw && oh == lh) {
        /* বাড়তি কপি নেই: rozgfx-এর back buffer-এর ভেতরের অংশই লজিক্যাল পর্দা */
        logical = gfx_surface_wrap(scr->px + (size_t)oy * scr->stride + ox, lw, lh, scr->stride);
        logical_own = 0;
    } else {
        logical = gfx_surface_new(lw, lh);
        logical_own = 1;
        if (logical) gfx_clear(logical, GFX_BLACK);
    }
    /* ফিতা (border) — পুরো পর্দা একবার */
    if (ow != sw || oh != sh) {
        gfx_reset_clip(scr);
        gfx_fill_rect(scr, 0, 0, sw, sh, C.border_color | 0xFF000000u);
    }
    free(xmap);
    xmap = NULL;
    xmap_n = 0;
    if (logical_own) {
        int i;
        xmap = (int *)malloc(sizeof(int) * (size_t)ow);
        if (xmap) {
            xmap_n = ow;
            for (i = 0; i < ow; i++) {
                if (C.smooth) {
                    /* 16.16: পিক্সেলের কেন্দ্র মিলিয়ে */
                    long v = (long)(((i + 0.5) * lw / ow - 0.5) * 65536.0);
                    if (v < 0) v = 0;
                    if (v > (long)(lw - 1) << 16) v = (long)(lw - 1) << 16;
                    xmap[i] = (int)v;
                } else {
                    int v = (int)((long)i * lw / ow);
                    xmap[i] = v < lw ? v : lw - 1;
                }
            }
        }
    }
    need_full_present = 1;
}

static uint32_t bilerp(uint32_t a, uint32_t b, uint32_t f)   /* f 0..256 */
{
    uint32_t nf = 256 - f;
    uint32_t rb = (((a & 0xFF00FF) * nf + (b & 0xFF00FF) * f) >> 8) & 0xFF00FF;
    uint32_t g  = (((a & 0x00FF00) * nf + (b & 0x00FF00) * f) >> 8) & 0x00FF00;
    return 0xFF000000u | rb | g;
}

static void scale_to_screen(void)
{
    gfx_surface *scr = gfx_screen();
    int y, i, prev_sy = -1;
    if (!scr || !logical || !xmap) return;
    for (y = 0; y < oh; y++) {
        uint32_t *d = scr->px + (size_t)(oy + y) * scr->stride + ox;
        if (C.smooth) {
            double fy = (y + 0.5) * lh / oh - 0.5;
            int sy0, sy1;
            uint32_t fyw;
            const uint32_t *r0, *r1;
            if (fy < 0) fy = 0;
            sy0 = (int)fy;
            if (sy0 > lh - 1) sy0 = lh - 1;
            sy1 = sy0 + 1 < lh ? sy0 + 1 : sy0;
            fyw = (uint32_t)((fy - sy0) * 256.0);
            r0 = logical->px + (size_t)sy0 * logical->stride;
            r1 = logical->px + (size_t)sy1 * logical->stride;
            for (i = 0; i < ow; i++) {
                int fx = xmap[i], x0 = fx >> 16, x1 = x0 + 1 < lw ? x0 + 1 : x0;
                uint32_t fw = (uint32_t)((fx & 0xFFFF) >> 8);
                uint32_t top = bilerp(r0[x0], r0[x1], fw), bot = bilerp(r1[x0], r1[x1], fw);
                d[i] = bilerp(top, bot, fyw);
            }
        } else {
            int sy = (int)((long)y * lh / oh);
            const uint32_t *s;
            if (sy >= lh) sy = lh - 1;
            if (sy == prev_sy) {
                memcpy(d, d - scr->stride, (size_t)ow * 4);
                continue;
            }
            prev_sy = sy;
            s = logical->px + (size_t)sy * logical->stride;
            for (i = 0; i < ow; i++) d[i] = s[xmap[i]];
        }
    }
}

/* ================= ইনপুট ================= */

static void clear_frame_input(void)
{
    memset(kpressed, 0, sizeof(kpressed));
    memset(kreleased, 0, sizeof(kreleased));
    memset(ktyped, 0, sizeof(ktyped));
    tlen = 0;
    tbuf[0] = 0;
    mpressed = mreleased = 0;
    mwheel = 0;
}

static void release_all(void)
{
    int k;
    for (k = 0; k < GFX_KEY_COUNT; k++) {
        if (kdown[k]) kreleased[k] = 1;
        kdown[k] = 0;
        kfake_until[k] = 0;
    }
}

/* অনুবাদ করা কী (GFX_EV_KEY) → মূল কোড (Shift ছাড়া), কাঁচা না থাকলে কাজে লাগে */
static int base_key(int k)
{
    static const char shifted[] = "!@#$%^&*()_+{}|:\"~<>?";
    static const char plain[]   = "1234567890-=[]\\;'`,./";
    const char *p;
    if (k >= 'A' && k <= 'Z') return k + 32;
    if (k >= 1 && k <= 26 && k != 8 && k != 9 && k != 10) return 'a' + k - 1;   /* Ctrl+অক্ষর */
    if (k > 32 && k < 127 && (p = strchr(shifted, k)) != NULL) return plain[p - shifted];
    return k;
}

static void handle_event(const gfx_event *e)
{
    double t = now_s();
    int k;
    switch (e->type) {
    case GFX_EV_KEY_DOWN:
        raw_last = t;
        k = e->key;
        if (k <= 0 || k >= GFX_KEY_COUNT) break;
        if (!e->repeat && !kdown[k]) kpressed[k] = 1;
        kdown[k] = 1;
        kfake_until[k] = 0;
        ktyped[k] = 1;
        break;
    case GFX_EV_KEY_UP:
        raw_last = t;
        k = e->key;
        if (k <= 0 || k >= GFX_KEY_COUNT) break;
        if (kdown[k]) kreleased[k] = 1;
        kdown[k] = 0;
        kfake_until[k] = 0;
        break;
    case GFX_EV_KEY:
        if (e->key >= 32 && e->key < 127 && tlen < (int)sizeof(tbuf) - 1) {
            tbuf[tlen++] = (char)e->key;
            tbuf[tlen] = 0;
        }
        /* কাঁচা ঘটনা আসছে না (serial, বা পুরোনো পথ) — চাপা ধরে নিয়ে একটু পরে ছেড়ে দেওয়া */
        if (raw_ok && t - raw_last < 1.0) break;
        k = base_key(e->key);
        if (k <= 0 || k >= GFX_KEY_COUNT) break;
        if (!kdown[k]) kpressed[k] = 1;
        kdown[k] = 1;
        ktyped[k] = 1;
        kfake_until[k] = t + 0.15;
        break;
    case GFX_EV_MOUSE_MOVE:
        msx = e->x; msy = e->y;
        break;
    case GFX_EV_MOUSE_DOWN:
        msx = e->x; msy = e->y;
        mpressed |= e->button & ~mbuttons;
        mbuttons |= e->button;
        break;
    case GFX_EV_MOUSE_UP:
        msx = e->x; msy = e->y;
        if (mbuttons & e->button) mreleased |= e->button;
        mbuttons &= ~e->button;
        break;
    case GFX_EV_MOUSE_WHEEL:
        mwheel += e->wheel;
        break;
    case GFX_EV_CLOSE:
        running = 0;
        break;
    case GFX_EV_RESIZE:
        layout_w = -1;                 /* পরের ফ্রেমে নতুন করে সাজানো */
        break;
    case GFX_EV_FOCUS:
        focused = e->key ? 1 : 0;
        if (!focused) {
            release_all();
            mbuttons = 0;
        }
        break;
    default:
        break;
    }
}

static void expire_fake_keys(void)
{
    double t = now_s();
    int k;
    for (k = 0; k < GFX_KEY_COUNT; k++) {
        if (kfake_until[k] > 0 && t >= kfake_until[k]) {
            kfake_until[k] = 0;
            if (kdown[k]) kreleased[k] = 1;
            kdown[k] = 0;
        }
    }
}

static void pump_events(int timeout_ms)
{
    gfx_event e;
    if (gfx_poll_event(&e, timeout_ms)) {
        handle_event(&e);
        while (gfx_poll_event(&e, 0)) handle_event(&e);
    }
}

void game_inject_event(const gfx_event *ev) { handle_event(ev); }
void game_input_frame(void) { clear_frame_input(); expire_fake_keys(); }

int game_key_down(int k)     { return k > 0 && k < GFX_KEY_COUNT && kdown[k]; }
int game_key_pressed(int k)  { return k > 0 && k < GFX_KEY_COUNT && kpressed[k]; }
int game_key_released(int k) { return k > 0 && k < GFX_KEY_COUNT && kreleased[k]; }
int game_key_typed(int k)    { return k > 0 && k < GFX_KEY_COUNT && ktyped[k]; }
int game_raw_keys(void)      { return raw_ok; }

int game_any_key_pressed(void)
{
    int k;
    for (k = 1; k < GFX_KEY_COUNT; k++)
        if (kpressed[k]) return k;
    return 0;
}

int game_text_input(char *buf, int size)
{
    int n = tlen < size - 1 ? tlen : size - 1;
    if (size <= 0) return 0;
    memcpy(buf, tbuf, (size_t)(n > 0 ? n : 0));
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static int to_logical_x(int x) { return ow > 0 ? (int)((long)(x - ox) * lw / ow) : x; }
static int to_logical_y(int y) { return oh > 0 ? (int)((long)(y - oy) * lh / oh) : y; }

int game_mouse_x(void) { return to_logical_x(msx); }
int game_mouse_y(void) { return to_logical_y(msy); }
int game_mouse_down(int b)     { return (mbuttons & b) != 0; }
int game_mouse_pressed(int b)  { return (mpressed & b) != 0; }
int game_mouse_released(int b) { return (mreleased & b) != 0; }
int game_mouse_wheel(void)     { return mwheel; }
int game_mouse_inside(void)
{
    int x = game_mouse_x(), y = game_mouse_y();
    return msx >= ox && msy >= oy && x >= 0 && y >= 0 && x < lw && y < lh;
}

/* ================= লুপ ================= */

game_config game_default_config(const char *title)
{
    game_config c;
    memset(&c, 0, sizeof(c));
    c.title = title ? title : "rozOS game";
    c.width = 640;
    c.height = 480;
    c.scale = GAME_SCALE_FIT;
    c.update_hz = 60;
    c.max_fps = 60;
    c.resizable = 1;
    c.border_color = GFX_BLACK;
    return c;
}

int game_open(const game_config *cfg)
{
    int ww, wh, r;
    if (opened) return 0;
    C = cfg ? *cfg : game_default_config(NULL);
    if (!C.title) C.title = "rozOS game";
    if (C.width <= 0) C.width = 640;
    if (C.height <= 0) C.height = 480;
    if (C.update_hz <= 0) C.update_hz = 60;
    if (C.max_fps == 0) C.max_fps = 60;
    ww = C.window_w > 0 ? C.window_w : C.width;
    wh = C.window_h > 0 ? C.window_h : C.height;
    r = gfx_init_window(C.title, ww, wh);   /* rozwm থাকলে উইন্ডো, নইলে পুরো পর্দা */
    if (r < 0) return -1;
    raw_ok = gfx_raw_keys(1);
    opened = 1;
    running = 1;
    focused = 1;
    memset(kdown, 0, sizeof(kdown));
    clear_frame_input();
    do_layout();
    t_start = t_last = t_frame_start = fps_t0 = now_s();
    frames = fps_frames = 0;
    auto_frames = getenv("ROZGAME_FRAMES") ? atol(getenv("ROZGAME_FRAMES")) : 0;
    auto_shot = getenv("ROZGAME_SHOT");
    auto_shot_at = getenv("ROZGAME_SHOT_AT") ? atol(getenv("ROZGAME_SHOT_AT")) : 0;
    fps_now = 0;
    return logical ? 0 : -1;
}

void game_close(void)
{
    if (!opened) return;
    if (auto_shot && *auto_shot && auto_shot_at <= 0) game_screenshot(auto_shot);
    if (auto_frames > 0 || getenv("ROZGAME_STATS")) {
        double t = now_s() - t_start;
        fprintf(stderr, "rozgame: %ld frames in %.2f s = %.1f fps (logical %dx%d, shown %dx%d at %d,%d, %s, raw keys %s)\n",
                frames, t, t > 0 ? frames / t : 0.0, lw, lh, ow, oh, ox, oy,
                logical_own ? (C.smooth ? "scaled smooth" : "scaled") : "direct", raw_ok ? "yes" : "no");
    }
    free_logical();
    free(xmap);
    xmap = NULL;
    gfx_quit();
    opened = 0;
}

void game_quit(void) { running = 0; }
int  game_running(void) { return running; }

int game_begin(void)
{
    double t;
    if (!opened) return 0;
    pump_events(0);
    expire_fake_keys();
    if (gfx_width() != layout_w || gfx_height() != layout_h) do_layout();
    t = now_s();
    dt_last = (float)(t - t_last);
    if (dt_last > 0.1f) dt_last = 0.1f;
    if (dt_last < 0) dt_last = 0;
    t_last = t;
    t_frame_start = t;
    return running;
}

static void draw_fps(void)
{
    char b[32];
    snprintf(b, sizeof(b), "%d fps", (int)(fps_now + 0.5f));
    int y = lh - game_text_height() - 8;          /* নিচের বাঁ কোণ (ওপরে সাধারণত স্কোর থাকে) */
    game_fill_rect(4, y - 2, game_text_width(b) + 8, game_text_height() + 4, GFX_RGBA(0, 0, 0, 160));
    game_text(8, y, b, GFX_RGB(120, 255, 120));
}

static void present_and_wait(void)
{
    double t, frame_time;
    if (!opened) return;
    if (C.show_fps) draw_fps();
    if (auto_shot_at > 0 && frames + 1 == auto_shot_at && auto_shot && *auto_shot) game_screenshot(auto_shot);
    if (logical_own) scale_to_screen();
    if (need_full_present) {
        gfx_present();
        need_full_present = 0;
    } else {
        gfx_present_rect(ox, oy, ow, oh);
    }
    frames++;
    fps_frames++;
    if (auto_frames > 0 && frames >= auto_frames) running = 0;
    t = now_s();
    if (t - fps_t0 >= 0.5) {
        fps_now = (float)(fps_frames / (t - fps_t0));
        fps_frames = 0;
        fps_t0 = t;
    }
    /* fps সীমা: বাকি সময়টা ঘটনার অপেক্ষায় (সাড়া দ্রুত থাকে) */
    if (C.max_fps > 0) {
        frame_time = 1.0 / C.max_fps;
        for (;;) {
            double left = t_frame_start + frame_time - now_s();
            if (left <= 0.0005) break;
            pump_events((int)(left * 1000.0) > 0 ? (int)(left * 1000.0) : 1);
        }
    }
}

void game_end(void)
{
    /* এই ফ্রেমের ইনপুট দেখা শেষ — মুছে তারপর অপেক্ষা (অপেক্ষার সময় আসা ঘটনা পরের ফ্রেমে) */
    clear_frame_input();
    present_and_wait();
}

int game_run(const game_config *cfg, const game_app *app)
{
    float step;
    double acc = 0;
    if (game_open(cfg) < 0) {
        fprintf(stderr, "game: no graphics (/dev/fb0 or rozwm)\n");
        return 1;
    }
    if (app && app->init && app->init(app->ud) != 0) {
        game_close();
        return 1;
    }
    step = 1.0f / (float)C.update_hz;
    while (game_begin()) {
        int n = 0;
        acc += dt_last;
        while (acc >= step && n < 5) {
            if (app && app->update) app->update(app->ud, step);
            acc -= step;
            if (++n == 1) clear_frame_input();     /* একটা চাপা একবারই দেখা যায় */
            if (!running) break;
        }
        if (acc > step * 5) acc = 0;               /* অনেক পিছিয়ে পড়লে ধরার চেষ্টা নয় */
        if (app && app->draw) app->draw(app->ud, logical);
        present_and_wait();
    }
    if (app && app->quit) app->quit(app->ud);
    game_close();
    return 0;
}

gfx_surface *game_screen(void) { return logical; }
int   game_width(void)  { return lw; }
int   game_height(void) { return lh; }
float game_dt(void)     { return dt_last; }
double game_time(void)  { return now_s() - t_start; }
float game_fps(void)    { return fps_now; }
long  game_frame_count(void) { return frames; }
int   game_focused(void) { return focused; }
void  game_set_title(const char *t) { gfx_set_title(t); }

int game_screenshot(const char *path)
{
    return logical ? gfx_save_bmp(logical, path) : -1;
}

/* ================= ছবি ================= */

void game_image_update(game_image *img)
{
    int y, x, all = 1;
    if (!img || !img->surf) return;
    for (y = 0; y < img->h; y++) {
        const uint32_t *p = img->surf->px + (size_t)y * img->surf->stride;
        int any_t = 0, any_o = 0, any_m = 0;
        for (x = 0; x < img->w; x++) {
            uint32_t a = p[x] >> 24;
            if (a == 255) any_o = 1;
            else if (a == 0) any_t = 1;
            else any_m = 1;
            if (any_m || (any_o && any_t)) break;
        }
        img->rows[y] = (any_m || (any_o && any_t)) ? 2 : any_o ? 1 : 0;
        if (img->rows[y] != 1) all = 0;
    }
    img->opaque = all;
}

game_image *game_image_from_surface(gfx_surface *s)
{
    game_image *img;
    if (!s) return NULL;
    img = (game_image *)calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->rows = (unsigned char *)malloc((size_t)s->h);
    if (!img->rows) {
        free(img);
        return NULL;
    }
    img->w = s->w;
    img->h = s->h;
    img->surf = s;
    game_image_update(img);
    return img;
}

game_image *game_image_load(const char *path)
{
    gfx_surface *s = gfx_load_image(path);
    game_image *img = game_image_from_surface(s);
    if (!img && s) gfx_surface_free(s);
    return img;
}

game_image *game_image_new(int w, int h)
{
    return game_image_from_surface(gfx_surface_new(w, h));
}

void game_image_free(game_image *img)
{
    if (!img) return;
    gfx_surface_free(img->surf);
    free(img->rows);
    free(img);
}

void game_image_colorkey(game_image *img, uint32_t rgb)
{
    int x, y;
    if (!img) return;
    for (y = 0; y < img->h; y++) {
        uint32_t *p = img->surf->px + (size_t)y * img->surf->stride;
        for (x = 0; x < img->w; x++)
            if ((p[x] & 0xFFFFFF) == (rgb & 0xFFFFFF)) p[x] = 0;
    }
    game_image_update(img);
}

game_image *game_image_sub(const game_image *img, int x, int y, int w, int h)
{
    game_image *n;
    int r;
    if (!img || w <= 0 || h <= 0) return NULL;
    n = game_image_new(w, h);
    if (!n) return NULL;
    for (r = 0; r < h; r++) {
        int sy = y + r, c;
        for (c = 0; c < w; c++) {
            int sx = x + c;
            n->surf->px[(size_t)r * n->surf->stride + c] =
                (sx >= 0 && sy >= 0 && sx < img->w && sy < img->h) ? img->surf->px[(size_t)sy * img->surf->stride + sx] : 0;
        }
    }
    game_image_update(n);
    return n;
}

/* মানসম্মত মাপ বদল: ছোট করতে এলাকার গড় (box), বড় করতে bilinear; alpha-ওজনসহ */
game_image *game_image_resize(const game_image *img, int w, int h, int smooth)
{
    game_image *n;
    int x, y;
    if (!img || w <= 0 || h <= 0) return NULL;
    n = game_image_new(w, h);
    if (!n) return NULL;
    for (y = 0; y < h; y++) {
        uint32_t *d = n->surf->px + (size_t)y * n->surf->stride;
        for (x = 0; x < w; x++) {
            if (!smooth) {
                int sx = (int)((long)x * img->w / w), sy = (int)((long)y * img->h / h);
                d[x] = img->surf->px[(size_t)sy * img->surf->stride + sx];
            } else if (w < img->w || h < img->h) {
                int x0 = (int)((long)x * img->w / w), x1 = (int)((long)(x + 1) * img->w / w);
                int y0 = (int)((long)y * img->h / h), y1 = (int)((long)(y + 1) * img->h / h);
                unsigned long sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
                int i, j;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                if (x1 > img->w) x1 = img->w;
                if (y1 > img->h) y1 = img->h;
                for (j = y0; j < y1; j++) {
                    const uint32_t *p = img->surf->px + (size_t)j * img->surf->stride;
                    for (i = x0; i < x1; i++) {
                        uint32_t c = p[i], a = c >> 24;
                        sr += GFX_R(c) * a; sg += GFX_G(c) * a; sb += GFX_B(c) * a; sa += a;
                        cnt++;
                    }
                }
                if (sa == 0 || cnt == 0) d[x] = 0;
                else d[x] = GFX_RGBA(sr / sa, sg / sa, sb / sa, sa / cnt);
            } else {
                double fx = (x + 0.5) * img->w / w - 0.5, fy = (y + 0.5) * img->h / h - 0.5;
                int x0, y0, x1, y1, k;
                double ax, ay, ch[4];
                uint32_t c00, c10, c01, c11;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                x0 = (int)fx; y0 = (int)fy;
                if (x0 > img->w - 1) x0 = img->w - 1;
                if (y0 > img->h - 1) y0 = img->h - 1;
                x1 = x0 + 1 < img->w ? x0 + 1 : x0;
                y1 = y0 + 1 < img->h ? y0 + 1 : y0;
                ax = fx - x0; ay = fy - y0;
                c00 = img->surf->px[(size_t)y0 * img->surf->stride + x0];
                c10 = img->surf->px[(size_t)y0 * img->surf->stride + x1];
                c01 = img->surf->px[(size_t)y1 * img->surf->stride + x0];
                c11 = img->surf->px[(size_t)y1 * img->surf->stride + x1];
                for (k = 0; k < 4; k++) {
                    int sh = k == 3 ? 24 : 16 - k * 8;
                    double v00 = (c00 >> sh) & 255, v10 = (c10 >> sh) & 255;
                    double v01 = (c01 >> sh) & 255, v11 = (c11 >> sh) & 255;
                    ch[k] = (v00 * (1 - ax) + v10 * ax) * (1 - ay) + (v01 * (1 - ax) + v11 * ax) * ay;
                }
                d[x] = GFX_RGBA((int)(ch[0] + 0.5), (int)(ch[1] + 0.5), (int)(ch[2] + 0.5), (int)(ch[3] + 0.5));
            }
        }
    }
    game_image_update(n);
    return n;
}

/* মূল আঁকা: img-এর (sx,sy,w,h) অংশ → লজিক্যাল পর্দার (x,y), উল্টানো আর আবছাসহ */
static void draw_part(const game_image *img, int sx, int sy, int w, int h, int x, int y, int flags, int alpha)
{
    gfx_surface *d = logical;
    int i0 = 0, i1, j0 = 0, j1, j, fx = flags & GAME_FLIP_X, fy = flags & GAME_FLIP_Y;
    if (!d || !img || alpha <= 0) return;
    /* উৎস ছবির সীমা */
    if (sx < 0) { w += sx; if (!fx) x -= sx; sx = 0; }
    if (sy < 0) { h += sy; if (!fy) y -= sy; sy = 0; }
    if (sx + w > img->w) { int c = sx + w - img->w; w -= c; if (fx) x += c; }
    if (sy + h > img->h) { int c = sy + h - img->h; h -= c; if (fy) y += c; }
    if (w <= 0 || h <= 0) return;
    /* পর্দার clip */
    i1 = w; j1 = h;
    if (x < d->clip_x) i0 = d->clip_x - x;
    if (y < d->clip_y) j0 = d->clip_y - y;
    if (x + i1 > d->clip_x + d->clip_w) i1 = d->clip_x + d->clip_w - x;
    if (y + j1 > d->clip_y + d->clip_h) j1 = d->clip_y + d->clip_h - y;
    if (i0 >= i1 || j0 >= j1) return;
    for (j = j0; j < j1; j++) {
        int srow = fy ? sy + h - 1 - j : sy + j, kind = img->rows[srow], i;
        const uint32_t *s = img->surf->px + (size_t)srow * img->surf->stride;
        uint32_t *o = d->px + (size_t)(y + j) * d->stride + x;
        if (kind == 0) continue;
        if (alpha < 255) {
            for (i = i0; i < i1; i++) o[i] = mix_alpha(o[i], s[fx ? sx + w - 1 - i : sx + i], (uint32_t)alpha);
        } else if (kind == 1 && !fx) {
            memcpy(o + i0, s + sx + i0, (size_t)(i1 - i0) * 4);
        } else if (kind == 1) {
            for (i = i0; i < i1; i++) o[i] = s[sx + w - 1 - i];
        } else if (!fx) {
            const uint32_t *sp = s + sx;
            for (i = i0; i < i1; i++) {
                uint32_t c = sp[i], a = c >> 24;
                if (a == 255) o[i] = c;
                else if (a) o[i] = mix(o[i], c);
            }
        } else {
            for (i = i0; i < i1; i++) {
                uint32_t c = s[sx + w - 1 - i], a = c >> 24;
                if (a == 255) o[i] = c;
                else if (a) o[i] = mix(o[i], c);
            }
        }
    }
}

void game_draw_image(const game_image *img, int x, int y)
{
    if (img) draw_part(img, 0, 0, img->w, img->h, x, y, 0, 255);
}

void game_draw_image_ex(const game_image *img, int sx, int sy, int sw, int sh, int x, int y, int flags)
{
    draw_part(img, sx, sy, sw, sh, x, y, flags, 255);
}

void game_draw_image_alpha(const game_image *img, int x, int y, int alpha)
{
    if (img) draw_part(img, 0, 0, img->w, img->h, x, y, 0, alpha > 255 ? 255 : alpha);
}

void game_draw_image_scaled(const game_image *img, int sx, int sy, int sw, int sh,
                            int x, int y, int w, int h, int flags)
{
    gfx_surface *d = logical;
    int i0 = 0, i1 = w, j0 = 0, j1 = h, i, j;
    long stepx, stepy;
    if (!d || !img || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    if (w == sw && h == sh) {
        draw_part(img, sx, sy, sw, sh, x, y, flags, 255);
        return;
    }
    if (x < d->clip_x) i0 = d->clip_x - x;
    if (y < d->clip_y) j0 = d->clip_y - y;
    if (x + i1 > d->clip_x + d->clip_w) i1 = d->clip_x + d->clip_w - x;
    if (y + j1 > d->clip_y + d->clip_h) j1 = d->clip_y + d->clip_h - y;
    if (i0 >= i1 || j0 >= j1) return;
    stepx = ((long)sw << 16) / w;
    stepy = ((long)sh << 16) / h;
    for (j = j0; j < j1; j++) {
        int ry = (int)(((long)j * stepy) >> 16), srow;
        const uint32_t *s;
        uint32_t *o = d->px + (size_t)(y + j) * d->stride + x;
        if (ry >= sh) ry = sh - 1;
        srow = sy + ((flags & GAME_FLIP_Y) ? sh - 1 - ry : ry);
        if (srow < 0 || srow >= img->h || img->rows[srow] == 0) continue;
        s = img->surf->px + (size_t)srow * img->surf->stride;
        for (i = i0; i < i1; i++) {
            int rx = (int)(((long)i * stepx) >> 16), scol;
            uint32_t c, a;
            if (rx >= sw) rx = sw - 1;
            scol = sx + ((flags & GAME_FLIP_X) ? sw - 1 - rx : rx);
            if (scol < 0 || scol >= img->w) continue;
            c = s[scol];
            a = c >> 24;
            if (a == 255) o[i] = c;
            else if (a) o[i] = mix(o[i], c);
        }
    }
}

void game_draw_image_rotated(const game_image *img, int cx, int cy, float angle, float scale)
{
    gfx_surface *d = logical;
    float ca, sa, r;
    int x0, y0, x1, y1, x, y;
    if (!d || !img || scale <= 0) return;
    ca = cosf(angle);
    sa = sinf(angle);
    r = sqrtf((float)(img->w * img->w + img->h * img->h)) * 0.5f * scale + 1;
    x0 = (int)(cx - r); x1 = (int)(cx + r);
    y0 = (int)(cy - r); y1 = (int)(cy + r);
    if (x0 < d->clip_x) x0 = d->clip_x;
    if (y0 < d->clip_y) y0 = d->clip_y;
    if (x1 > d->clip_x + d->clip_w - 1) x1 = d->clip_x + d->clip_w - 1;
    if (y1 > d->clip_y + d->clip_h - 1) y1 = d->clip_y + d->clip_h - 1;
    {
        /* পর্দা → ছবি (উল্টো ঘোরানো), 16.16 */
        long ux = (long)(ca / scale * 65536), uy = (long)(-sa / scale * 65536);
        long vx = (long)(sa / scale * 65536), vy = (long)(ca / scale * 65536);
        long hw = (long)img->w << 15, hh = (long)img->h << 15;
        for (y = y0; y <= y1; y++) {
            uint32_t *o = d->px + (size_t)y * d->stride;
            long dy = y - cy, dx0 = x0 - cx;
            long u = dx0 * ux + dy * vx + hw, v = dx0 * uy + dy * vy + hh;
            for (x = x0; x <= x1; x++, u += ux, v += uy) {
                int iu = (int)(u >> 16), iv = (int)(v >> 16);
                uint32_t c, a;
                if (u < 0 || v < 0 || iu >= img->w || iv >= img->h) continue;
                c = img->surf->px[(size_t)iv * img->surf->stride + iu];
                a = c >> 24;
                if (a == 255) o[x] = c;
                else if (a) o[x] = mix(o[x], c);
            }
        }
    }
}

void game_draw_background(const game_image *img)
{
    if (!img || !logical) return;
    if (img->w == lw && img->h == lh) game_draw_image(img, 0, 0);
    else game_draw_image_scaled(img, 0, 0, img->w, img->h, 0, 0, lw, lh, 0);
}

/* ---------- sprite sheet ও অ্যানিমেশন ---------- */

game_sheet *game_sheet_new(game_image *img, int fw, int fh)
{
    game_sheet *sh;
    if (!img || fw <= 0 || fh <= 0) return NULL;
    sh = (game_sheet *)calloc(1, sizeof(*sh));
    if (!sh) return NULL;
    sh->img = img;
    sh->fw = fw;
    sh->fh = fh;
    sh->cols = img->w / fw > 0 ? img->w / fw : 1;
    sh->rows = img->h / fh > 0 ? img->h / fh : 1;
    sh->count = sh->cols * sh->rows;
    return sh;
}

game_sheet *game_sheet_load(const char *path, int fw, int fh)
{
    game_image *img = game_image_load(path);
    game_sheet *sh = game_sheet_new(img, fw, fh);
    if (!sh) game_image_free(img);
    return sh;
}

void game_sheet_free(game_sheet *sh)
{
    if (!sh) return;
    game_image_free(sh->img);
    free(sh);
}

void game_draw_frame(const game_sheet *sh, int frame, int x, int y, int flags)
{
    if (!sh || sh->count <= 0) return;
    frame %= sh->count;
    if (frame < 0) frame += sh->count;
    draw_part(sh->img, (frame % sh->cols) * sh->fw, (frame / sh->cols) * sh->fh, sh->fw, sh->fh, x, y, flags, 255);
}

void game_draw_frame_scaled(const game_sheet *sh, int frame, int x, int y, int w, int h, int flags)
{
    if (!sh || sh->count <= 0) return;
    frame %= sh->count;
    if (frame < 0) frame += sh->count;
    game_draw_image_scaled(sh->img, (frame % sh->cols) * sh->fw, (frame / sh->cols) * sh->fh,
                           sh->fw, sh->fh, x, y, w, h, flags);
}

void game_anim_set(game_anim *a, int first, int count, float fps, int loop)
{
    if (!a) return;
    if (a->first == first && a->count == count && a->fps == fps && a->loop == loop) return;
    a->first = first;
    a->count = count > 0 ? count : 1;
    a->fps = fps;
    a->loop = loop;
    a->t = 0;
    a->done = 0;
}

void game_anim_restart(game_anim *a)
{
    if (a) { a->t = 0; a->done = 0; }
}

void game_anim_update(game_anim *a, float dt)
{
    float len;
    if (!a || a->fps <= 0 || a->done) return;
    a->t += dt;
    len = a->count / a->fps;
    if (a->t >= len) {
        if (a->loop) a->t = fmodf(a->t, len);
        else { a->t = len; a->done = 1; }
    }
}

int game_anim_frame(const game_anim *a)
{
    int f;
    if (!a) return 0;
    f = (int)(a->t * a->fps);
    if (f >= a->count) f = a->count - 1;
    if (f < 0) f = 0;
    return a->first + f;
}

/* ================= সহজ আঁকা ================= */

void game_clear(uint32_t c)
{
    if (logical) gfx_clear(logical, c | 0xFF000000u);
}

void game_fill_rect(int x, int y, int w, int h, uint32_t c)
{
    gfx_surface *d = logical;
    int x1 = x + w, y1 = y + h, row, i;
    if (!d) return;
    if ((c >> 24) == 255) {
        gfx_fill_rect(d, x, y, w, h, c);
        return;
    }
    if ((c >> 24) == 0) return;
    if (x < d->clip_x) x = d->clip_x;
    if (y < d->clip_y) y = d->clip_y;
    if (x1 > d->clip_x + d->clip_w) x1 = d->clip_x + d->clip_w;
    if (y1 > d->clip_y + d->clip_h) y1 = d->clip_y + d->clip_h;
    for (row = y; row < y1; row++) {
        uint32_t *p = d->px + (size_t)row * d->stride;
        for (i = x; i < x1; i++) p[i] = mix(p[i], c);
    }
}

void game_draw_rect(int x, int y, int w, int h, uint32_t c)   { if (logical) gfx_rect(logical, x, y, w, h, c); }
void game_fill_circle(int cx, int cy, int r, uint32_t c)      { if (logical) gfx_fill_circle(logical, cx, cy, r, c); }
void game_line(int x0, int y0, int x1, int y1, uint32_t c)    { if (logical) gfx_line(logical, x0, y0, x1, y1, c); }
void game_gradient(int x, int y, int w, int h, uint32_t t, uint32_t b) { if (logical) gfx_gradient_v(logical, x, y, w, h, t, b); }

/* ================= লেখা ================= */

static gfx_ttf *default_ttf(void)
{
    static gfx_ttf *t;
    static int tried;
    if (!t && !tried) {
        tried = 1;
        t = gfx_ttf_load("/usr/share/fonts/DejaVuSans.ttf");
    }
    return t;
}

int game_text(int x, int y, const char *s, uint32_t c)
{
    return logical ? gfx_text(logical, gfx_font_default(), x, y, s, c) - x : 0;
}

int game_text_width(const char *s) { return gfx_text_width(gfx_font_default(), s); }
int game_text_height(void)         { return gfx_font_height(gfx_font_default()); }

void game_text_center(int y, const char *s, uint32_t c)
{
    game_text((lw - game_text_width(s)) / 2, y, s, c);
}

void game_text_shadow(int x, int y, const char *s, uint32_t c)
{
    game_text(x + 2, y + 2, s, GFX_RGB(0, 0, 0));
    game_text(x, y, s, c);
}

gfx_ttf *game_font_load(const char *path) { return gfx_ttf_load(path); }

int game_ttf(gfx_ttf *f, float size, int x, int y, const char *s, uint32_t c)
{
    if (!f) f = default_ttf();
    if (!f || !logical) return game_text(x, y, s, c);
    return gfx_ttf_text(logical, f, size, x, y, s, c) - x;
}

int game_ttf_width(gfx_ttf *f, float size, const char *s)
{
    if (!f) f = default_ttf();
    if (!f) return game_text_width(s);
    return gfx_ttf_width(f, size, s);
}

void game_ttf_center(gfx_ttf *f, float size, int y, const char *s, uint32_t c)
{
    game_ttf(f, size, (lw - game_ttf_width(f, size, s)) / 2, y, s, c);
}

int game_printf(int x, int y, uint32_t c, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return game_text(x, y, b, c);
}

static float menu_size(const game_menu *m) { return m->size > 0 ? m->size : 28; }

/* i নম্বর লাইনের ঘর (মাউসের জন্য) */
static int menu_item_at(const game_menu *m, int mx, int my)
{
    float size = menu_size(m);
    int line = (int)(size * 1.4f), i;
    for (i = 0; i < m->count; i++) {
        int w = game_ttf_width(NULL, size, m->items[i]), x = (lw - w) / 2, yy = m->y + i * line;
        if (my >= yy - 4 && my < yy - 4 + line && mx >= x - 24 && mx < x + w + 24) return i;
    }
    return -1;
}

int game_menu_update(game_menu *m)
{
    int chosen = -1, mx = game_mouse_x(), my = game_mouse_y(), hit;
    if (!m || m->count <= 0) return -1;
    if (m->sel < 0) m->sel = 0;
    if (m->sel >= m->count) m->sel = m->count - 1;
    if (game_key_typed(GFX_KEY_UP) || game_key_typed('w')) m->sel = (m->sel + m->count - 1) % m->count;
    if (game_key_typed(GFX_KEY_DOWN) || game_key_typed('s')) m->sel = (m->sel + 1) % m->count;
    if (game_key_pressed(GFX_KEY_ENTER) || game_key_pressed(' ')) chosen = m->sel;
    hit = menu_item_at(m, mx, my);
    /* মাউস নড়লে তবেই বাছাই বদলায় — কীবোর্ডের বাছাই টিকে থাকে */
    if (hit >= 0 && (mx != m->last_mx || my != m->last_my) && (m->last_mx || m->last_my)) m->sel = hit;
    if (hit >= 0 && game_mouse_pressed(GFX_BUTTON_LEFT)) chosen = hit;
    m->last_mx = mx ? mx : 1;
    m->last_my = my;
    return chosen;
}

void game_menu_draw(game_menu *m, int y)
{
    float size;
    int i, line;
    if (!m || m->count <= 0) return;
    size = menu_size(m);
    line = (int)(size * 1.4f);
    m->y = y;
    for (i = 0; i < m->count; i++) {
        int w = game_ttf_width(NULL, size, m->items[i]), x = (lw - w) / 2, yy = y + i * line;
        if (i == m->sel) {
            game_fill_rect(x - 24, yy - 4, w + 48, line, GFX_RGBA(255, 255, 255, 40));
            game_ttf(NULL, size, x, yy, m->items[i], m->sel_color ? m->sel_color : GFX_YELLOW);
        } else {
            game_ttf(NULL, size, x, yy, m->items[i], m->color ? m->color : GFX_WHITE);
        }
    }
}

int game_menu_do(game_menu *m, int y)
{
    int c;
    if (!m) return -1;
    m->y = y;
    c = game_menu_update(m);
    game_menu_draw(m, y);
    return c;
}

/* ================= সংঘর্ষ ================= */

game_rect game_make_rect(float x, float y, float w, float h)
{
    game_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

int game_rect_overlap(game_rect a, game_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int game_rect_contains(game_rect r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

game_rect game_rect_intersect(game_rect a, game_rect b)
{
    game_rect r;
    float x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    float x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    float y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    r.x = x0; r.y = y0;
    r.w = x1 > x0 ? x1 - x0 : 0;
    r.h = y1 > y0 ? y1 - y0 : 0;
    if (r.w == 0 || r.h == 0) r.w = r.h = 0;
    return r;
}

int game_circle_overlap(float x1, float y1, float r1, float x2, float y2, float r2)
{
    float dx = x2 - x1, dy = y2 - y1, r = r1 + r2;
    return dx * dx + dy * dy < r * r;
}

int game_circle_rect(float cx, float cy, float r, game_rect b)
{
    float nx = game_clamp(cx, b.x, b.x + b.w), ny = game_clamp(cy, b.y, b.y + b.h);
    float dx = cx - nx, dy = cy - ny;
    return dx * dx + dy * dy < r * r;
}

int game_rect_hit_side(game_rect a, game_rect b)
{
    float left, right, top, bottom, mx, my;
    if (!game_rect_overlap(a, b)) return GAME_SIDE_NONE;
    left = a.x + a.w - b.x;          /* a ডান দিক থেকে ঢুকেছে → a-র ডান পাশ লাগল */
    right = b.x + b.w - a.x;
    top = a.y + a.h - b.y;
    bottom = b.y + b.h - a.y;
    mx = left < right ? left : right;
    my = top < bottom ? top : bottom;
    if (mx < my) return left < right ? GAME_SIDE_RIGHT : GAME_SIDE_LEFT;
    return top < bottom ? GAME_SIDE_BOTTOM : GAME_SIDE_TOP;
}

int game_rect_push_out(game_rect *a, game_rect b)
{
    int side;
    if (!a) return GAME_SIDE_NONE;
    side = game_rect_hit_side(*a, b);
    switch (side) {
    case GAME_SIDE_RIGHT:  a->x = b.x - a->w; break;
    case GAME_SIDE_LEFT:   a->x = b.x + b.w; break;
    case GAME_SIDE_BOTTOM: a->y = b.y - a->h; break;
    case GAME_SIDE_TOP:    a->y = b.y + b.h; break;
    default: break;
    }
    return side;
}

/* ================= সহায়ক ================= */

static uint32_t rng_state;

void game_rand_seed(uint32_t s) { rng_state = s ? s : 0x9E3779B9u; }

uint32_t game_rand(void)
{
    uint32_t x;
    if (!rng_state) {
        uint32_t s = 0;
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            if (read(fd, &s, sizeof(s)) != (ssize_t)sizeof(s)) s = 0;
            close(fd);
        }
        if (!s) s = (uint32_t)(now_s() * 1000003.0);
        game_rand_seed(s);
    }
    x = rng_state;                          /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

int game_rand_int(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(game_rand() % (uint32_t)(hi - lo + 1));
}

float game_rand_float(float lo, float hi)
{
    return lo + (hi - lo) * (float)(game_rand() >> 8) / 16777216.0f;
}

float game_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float game_lerp(float a, float b, float t)    { return a + (b - a) * t; }

float game_approach(float v, float target, float step)
{
    if (v < target) return v + step > target ? target : v + step;
    if (v > target) return v - step < target ? target : v - step;
    return v;
}

float game_dist(float x1, float y1, float x2, float y2)
{
    float dx = x2 - x1, dy = y2 - y1;
    return sqrtf(dx * dx + dy * dy);
}

int game_every(float *acc, float interval, float dt)
{
    if (!acc || interval <= 0) return 0;
    *acc += dt;
    if (*acc >= interval) {
        *acc -= interval;
        if (*acc >= interval) *acc = 0;
        return 1;
    }
    return 0;
}

uint32_t game_color_lerp(uint32_t a, uint32_t b, float t)
{
    int k;
    uint32_t r = 0;
    t = game_clamp(t, 0, 1);
    for (k = 0; k < 32; k += 8) {
        float va = (float)((a >> k) & 255), vb = (float)((b >> k) & 255);
        r |= (uint32_t)(va + (vb - va) * t + 0.5f) << k;
    }
    return r;
}

uint32_t game_hsv(float h, float s, float v)
{
    float c, x, m, r = 0, g = 0, b = 0;
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    c = v * s;
    x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    m = v - c;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    return GFX_RGB((int)((r + m) * 255 + 0.5f), (int)((g + m) * 255 + 0.5f), (int)((b + m) * 255 + 0.5f));
}

static void score_path(char *buf, size_t size, const char *name)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/home";
    snprintf(buf, size, "%s/.rozgame", home);
    mkdir(buf, 0755);
    snprintf(buf, size, "%s/.rozgame/%s.score", home, name);
}

long game_score_load(const char *name)
{
    char p[256];
    long v = 0;
    FILE *f;
    if (!name) return 0;
    score_path(p, sizeof(p), name);
    f = fopen(p, "r");
    if (!f) return 0;
    if (fscanf(f, "%ld", &v) != 1) v = 0;
    fclose(f);
    return v;
}

int game_score_save(const char *name, long score)
{
    char p[256];
    FILE *f;
    if (!name || score <= game_score_load(name)) return 0;
    score_path(p, sizeof(p), name);
    f = fopen(p, "w");
    if (!f) return 0;
    fprintf(f, "%ld\n", score);
    fclose(f);
    return 1;
}

const char *game_data_path(const char *file)
{
    static char p[512];
    struct stat st;
    if (!file) return NULL;
    if (stat(file, &st) == 0) return file;
    snprintf(p, sizeof(p), "/usr/share/games/%s", file);
    return p;
}

/* ================= ক্যামেরা (ধাপ ১৮খ) ================= */

void game_camera_init(game_camera *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
}

static int cam_w(const game_camera *c) { return c->w > 0 ? c->w : lw; }
static int cam_h(const game_camera *c) { return c->h > 0 ? c->h : lh; }

static void cam_clamp(game_camera *c)
{
    float w = (float)cam_w(c), h = (float)cam_h(c);
    if (!c->bounded) return;
    if (c->bx1 - c->bx0 <= w) c->x = c->bx0 - (w - (c->bx1 - c->bx0)) / 2;   /* জগত ছোট — মাঝখানে */
    else c->x = game_clamp(c->x, c->bx0, c->bx1 - w);
    if (c->by1 - c->by0 <= h) c->y = c->by0 - (h - (c->by1 - c->by0)) / 2;
    else c->y = game_clamp(c->y, c->by0, c->by1 - h);
}

void game_camera_bounds(game_camera *c, float x0, float y0, float x1, float y1)
{
    if (!c) return;
    c->bounded = 1;
    c->bx0 = x0; c->by0 = y0; c->bx1 = x1; c->by1 = y1;
    cam_clamp(c);
}

void game_camera_follow(game_camera *c, float tx, float ty, float speed, float dt)
{
    float gx, gy, k;
    if (!c) return;
    gx = tx - cam_w(c) / 2.0f;
    gy = ty - cam_h(c) / 2.0f;
    if (speed <= 0) {
        c->x = gx;
        c->y = gy;
    } else {
        k = 1.0f - expf(-speed * dt);              /* fps যাই হোক একই রকম মসৃণ */
        c->x += (gx - c->x) * k;
        c->y += (gy - c->y) * k;
    }
    cam_clamp(c);
}

void game_camera_shake(game_camera *c, float amp, float seconds)
{
    if (!c) return;
    if (amp > c->shake_amp || c->shake_left <= 0) c->shake_amp = amp;
    c->shake_time = seconds;
    c->shake_left = seconds;
}

void game_camera_update(game_camera *c, float dt)
{
    if (!c) return;
    if (c->shake_left > 0) {
        float a = c->shake_amp * (c->shake_time > 0 ? c->shake_left / c->shake_time : 0);
        c->shake_left -= dt;
        c->sx = (int)game_rand_float(-a, a);
        c->sy = (int)game_rand_float(-a, a);
    } else {
        c->sx = c->sy = 0;
        c->shake_amp = 0;
    }
}

int game_camera_view_x(const game_camera *c) { return c ? (int)floorf(c->x) + c->sx : 0; }
int game_camera_view_y(const game_camera *c) { return c ? (int)floorf(c->y) + c->sy : 0; }

/* ================= টাইল ম্যাপ (ধাপ ১৮খ) ================= */

game_tilemap *game_tilemap_new(int cols, int rows, int tw, int th)
{
    game_tilemap *m;
    int i;
    if (cols <= 0 || rows <= 0 || tw <= 0 || th <= 0) return NULL;
    m = (game_tilemap *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->cells = (unsigned char *)malloc((size_t)cols * (size_t)rows);
    if (!m->cells) {
        free(m);
        return NULL;
    }
    memset(m->cells, '.', (size_t)cols * (size_t)rows);
    m->cols = cols; m->rows = rows; m->tw = tw; m->th = th;
    for (i = 0; i < 256; i++) m->frame[i] = -1;
    m->edges = GAME_EDGE_LEFT | GAME_EDGE_RIGHT | GAME_EDGE_TOP;
    return m;
}

void game_tilemap_free(game_tilemap *m)
{
    if (!m) return;
    game_sheet_free(m->set);
    free(m->cells);
    free(m);
}

void game_tilemap_tile(game_tilemap *m, int ch, int frame, int solid)
{
    if (!m || ch <= 0 || ch > 255) return;
    m->frame[ch] = (short)frame;
    m->solid[ch] = solid ? 1 : 0;
}

/* tileset/ছবির পথ: ম্যাপের ফোল্ডারে আগে, তারপর game_data_path */
static const char *sibling_path(const char *map_path, const char *file, char *buf, size_t size)
{
    const char *slash = strrchr(map_path, '/');
    struct stat st;
    if (file[0] == '/') return file;
    if (slash) {
        snprintf(buf, size, "%.*s/%s", (int)(slash - map_path), map_path, file);
        if (stat(buf, &st) == 0) return buf;
    }
    return game_data_path(file);
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

game_tilemap *game_tilemap_load(const char *path)
{
    FILE *f;
    char line[1024], tileset[256] = "", buf[512];
    int tw = 32, th = 32, in_map = 0, cols = 0, rows = 0, cap = 0, r, i;
    char **lines = NULL;
    short frame[256];
    unsigned char solid[256];
    uint32_t color[256];
    game_tilemap *m = NULL;

    if (!path || !(f = fopen(path, "r"))) {
        fprintf(stderr, "game_tilemap_load: %s: cannot open\n", path ? path : "(null)");
        return NULL;
    }
    for (i = 0; i < 256; i++) { frame[i] = -1; solid[i] = 0; color[i] = 0; }
    while (fgets(line, sizeof(line), f)) {
        if (in_map) {
            char *e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            if (rows == cap) {
                char **n = (char **)realloc(lines, sizeof(char *) * (size_t)(cap ? cap * 2 : 32));
                if (!n) break;
                lines = n;
                cap = cap ? cap * 2 : 32;
            }
            lines[rows] = strdup(line);
            if (!lines[rows]) break;
            if ((int)strlen(line) > cols) cols = (int)strlen(line);
            rows++;
            continue;
        }
        {
            char *s = trim(line), *kw, *a, *b, *c;
            if (!*s || *s == '#') continue;
            kw = strtok(s, " \t");
            a = strtok(NULL, " \t");
            b = strtok(NULL, " \t");
            c = strtok(NULL, " \t");
            if (!strcmp(kw, "map")) in_map = 1;
            else if (!strcmp(kw, "tileset") && a) {
                snprintf(tileset, sizeof(tileset), "%s", a);
                if (b) tw = atoi(b);
                if (c) th = atoi(c);
            } else if (!strcmp(kw, "size") && a && b) {
                tw = atoi(a);
                th = atoi(b);
            } else if (!strcmp(kw, "tile") && a && b) {
                frame[(unsigned char)a[0]] = (short)atoi(b);
                solid[(unsigned char)a[0]] = c && !strcmp(c, "solid");
            } else if (!strcmp(kw, "color") && a && b) {
                color[(unsigned char)a[0]] = 0xFF000000u | (uint32_t)strtoul(b, NULL, 16);
                solid[(unsigned char)a[0]] = c && !strcmp(c, "solid");
            }
        }
    }
    fclose(f);
    if (rows > 0 && cols > 0) m = game_tilemap_new(cols, rows, tw > 0 ? tw : 32, th > 0 ? th : 32);
    if (m) {
        for (r = 0; r < rows; r++) {
            int n = (int)strlen(lines[r]);
            for (i = 0; i < n; i++) m->cells[(size_t)r * cols + i] = (unsigned char)lines[r][i];
        }
        memcpy(m->frame, frame, sizeof(frame));
        memcpy(m->solid, solid, sizeof(solid));
        memcpy(m->color, color, sizeof(color));
        if (tileset[0]) {
            m->set = game_sheet_load(sibling_path(path, tileset, buf, sizeof(buf)), m->tw, m->th);
            if (!m->set) fprintf(stderr, "game_tilemap_load: tileset %s not found\n", tileset);
        }
    } else {
        fprintf(stderr, "game_tilemap_load: %s: no map lines\n", path);
    }
    for (r = 0; r < rows; r++) free(lines[r]);
    free(lines);
    return m;
}

int game_tilemap_get(const game_tilemap *m, int c, int r)
{
    if (!m || c < 0 || r < 0 || c >= m->cols || r >= m->rows) return 0;
    return m->cells[(size_t)r * m->cols + c];
}

void game_tilemap_set(game_tilemap *m, int c, int r, int ch)
{
    if (!m || c < 0 || r < 0 || c >= m->cols || r >= m->rows) return;
    m->cells[(size_t)r * m->cols + c] = (unsigned char)ch;
}

int game_tilemap_solid(const game_tilemap *m, int c, int r)
{
    if (!m) return 0;
    if (c < 0) return (m->edges & GAME_EDGE_LEFT) != 0;
    if (c >= m->cols) return (m->edges & GAME_EDGE_RIGHT) != 0;
    if (r < 0) return (m->edges & GAME_EDGE_TOP) != 0;
    if (r >= m->rows) return (m->edges & GAME_EDGE_BOTTOM) != 0;
    return m->solid[m->cells[(size_t)r * m->cols + c]];
}

int game_tilemap_solid_at(const game_tilemap *m, float x, float y)
{
    if (!m) return 0;
    return game_tilemap_solid(m, (int)floorf(x / m->tw), (int)floorf(y / m->th));
}

int game_tilemap_rect_solid(const game_tilemap *m, game_rect r)
{
    int c0, c1, r0, r1, c, rr;
    if (!m || r.w <= 0 || r.h <= 0) return 0;
    c0 = (int)floorf(r.x / m->tw);
    c1 = (int)floorf((r.x + r.w - 0.001f) / m->tw);
    r0 = (int)floorf(r.y / m->th);
    r1 = (int)floorf((r.y + r.h - 0.001f) / m->th);
    for (rr = r0; rr <= r1; rr++)
        for (c = c0; c <= c1; c++)
            if (game_tilemap_solid(m, c, rr)) return 1;
    return 0;
}

int game_tilemap_find(const game_tilemap *m, int ch, int start, int *col, int *row)
{
    int i, n;
    if (!m) return -1;
    n = m->cols * m->rows;
    for (i = start < 0 ? 0 : start; i < n; i++)
        if (m->cells[i] == (unsigned char)ch) {
            if (col) *col = i % m->cols;
            if (row) *row = i / m->cols;
            return i;
        }
    return -1;
}

/* এক অক্ষে সরানো; ধাপ টাইলের অর্ধেকের বেশি নয় (পাতলা দেয়াল ভেদ করে না) */
static int move_axis(const game_tilemap *m, game_rect *r, float d, int xaxis)
{
    int hit = 0;
    float limit = (xaxis ? m->tw : m->th) * 0.5f;
    while (d != 0 && !hit) {
        float step = d > limit ? limit : d < -limit ? -limit : d;
        game_rect t = *r;
        d -= step;
        if (xaxis) t.x += step; else t.y += step;
        if (!game_tilemap_rect_solid(m, t)) {
            *r = t;
            continue;
        }
        /* ধাক্কা: টাইলের কিনারায় বসানো */
        if (xaxis) {
            if (step > 0) {
                int c = (int)floorf((t.x + t.w - 0.001f) / m->tw);
                r->x = c * m->tw - r->w;
                hit = GAME_HIT_RIGHT;
            } else {
                int c = (int)floorf(t.x / m->tw);
                r->x = (float)(c + 1) * m->tw;
                hit = GAME_HIT_LEFT;
            }
        } else {
            if (step > 0) {
                int rr = (int)floorf((t.y + t.h - 0.001f) / m->th);
                r->y = rr * m->th - r->h;
                hit = GAME_HIT_BOTTOM;
            } else {
                int rr = (int)floorf(t.y / m->th);
                r->y = (float)(rr + 1) * m->th;
                hit = GAME_HIT_TOP;
            }
        }
        /* কিনারায় বসানোর পরেও ঢুকে থাকলে (অস্বাভাবিক) আগের জায়গায় */
        if (game_tilemap_rect_solid(m, *r)) {
            if (xaxis) r->x = t.x - step; else r->y = t.y - step;
        }
    }
    return hit;
}

int game_tilemap_move(const game_tilemap *m, game_rect *r, float dx, float dy)
{
    int hit = 0;
    if (!m || !r) return 0;
    if (dx != 0) hit |= move_axis(m, r, dx, 1);
    if (dy != 0) hit |= move_axis(m, r, dy, 0);
    return hit;
}

void game_tilemap_draw(const game_tilemap *m, int vx, int vy)
{
    int c0, c1, r0, r1, c, r;
    if (!m || !logical) return;
    c0 = vx < 0 ? -((-vx + m->tw - 1) / m->tw) : vx / m->tw;
    r0 = vy < 0 ? -((-vy + m->th - 1) / m->th) : vy / m->th;
    c1 = (vx + lw) / m->tw;
    r1 = (vy + lh) / m->th;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 >= m->cols) c1 = m->cols - 1;
    if (r1 >= m->rows) r1 = m->rows - 1;
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) {
            int ch = m->cells[(size_t)r * m->cols + c], x = c * m->tw - vx, y = r * m->th - vy;
            if (m->frame[ch] >= 0 && m->set) game_draw_frame(m->set, m->frame[ch], x, y, 0);
            else if (m->color[ch] >> 24) game_fill_rect(x, y, m->tw, m->th, m->color[ch]);
        }
}

/* ================= কণা / particle (ধাপ ১৮খ) ================= */

game_emitter game_emitter_default(uint32_t color)
{
    game_emitter e;
    memset(&e, 0, sizeof(e));
    e.speed_min = 40;
    e.speed_max = 180;
    e.spread = 6.2831853f;
    e.life_min = 0.4f;
    e.life_max = 0.9f;
    e.size_start = 4;
    e.size_end = 1;
    e.color_start = color | 0xFF000000u;
    e.color_end = color & 0x00FFFFFFu;            /* মিলিয়ে যায় */
    e.gravity = 200;
    e.drag = 0.5f;
    return e;
}

game_particles *game_particles_new(int max)
{
    game_particles *ps;
    if (max <= 0) return NULL;
    ps = (game_particles *)calloc(1, sizeof(*ps));
    if (!ps) return NULL;
    ps->p = (game_particle *)calloc((size_t)max, sizeof(game_particle));
    if (!ps->p) {
        free(ps);
        return NULL;
    }
    ps->max = max;
    return ps;
}

void game_particles_free(game_particles *ps)
{
    if (!ps) return;
    free(ps->p);
    free(ps);
}

void game_particles_clear(game_particles *ps)
{
    if (ps) memset(ps->p, 0, sizeof(game_particle) * (size_t)ps->max);
}

void game_particles_burst(game_particles *ps, float x, float y, int n, const game_emitter *e)
{
    game_emitter d;
    if (!ps || n <= 0) return;
    if (!e) {
        d = game_emitter_default(GFX_WHITE);
        e = &d;
    }
    while (n-- > 0) {
        game_particle *p;
        int i, slot = -1;
        float a, sp;
        /* খালি ঘর খোঁজা; না পেলে সবচেয়ে পুরোনোটার জায়গায় (ঘুরে ঘুরে) */
        for (i = 0; i < ps->max; i++) {
            int k = (ps->next + i) % ps->max;
            if (ps->p[k].life <= 0) { slot = k; break; }
        }
        if (slot < 0) slot = ps->next;
        ps->next = (slot + 1) % ps->max;
        p = &ps->p[slot];
        a = e->angle + game_rand_float(-e->spread / 2, e->spread / 2);
        sp = game_rand_float(e->speed_min, e->speed_max > e->speed_min ? e->speed_max : e->speed_min + 0.001f);
        p->x = x; p->y = y;
        p->vx = cosf(a) * sp;
        p->vy = sinf(a) * sp;
        p->max = p->life = game_rand_float(e->life_min > 0 ? e->life_min : 0.01f,
                                          e->life_max > e->life_min ? e->life_max : e->life_min + 0.01f);
        p->size0 = e->size_start;
        p->size1 = e->size_end;
        p->gravity = e->gravity;
        p->drag = e->drag;
        p->c0 = e->color_start;
        p->c1 = e->color_end;
    }
}

void game_particles_update(game_particles *ps, float dt)
{
    int i;
    if (!ps) return;
    for (i = 0; i < ps->max; i++) {
        game_particle *p = &ps->p[i];
        float k;
        if (p->life <= 0) continue;
        p->life -= dt;
        p->vy += p->gravity * dt;
        k = 1.0f - p->drag * dt;
        if (k < 0) k = 0;
        p->vx *= k;
        p->vy *= k;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
    }
}

void game_particles_draw(const game_particles *ps, int vx, int vy)
{
    int i;
    if (!ps) return;
    for (i = 0; i < ps->max; i++) {
        const game_particle *p = &ps->p[i];
        float t, sz;
        int s;
        if (p->life <= 0) continue;
        t = 1.0f - p->life / p->max;
        sz = p->size0 + (p->size1 - p->size0) * t;
        s = sz < 1 ? 1 : (int)(sz + 0.5f);
        game_fill_rect((int)p->x - s / 2 - vx, (int)p->y - s / 2 - vy, s, s, game_color_lerp(p->c0, p->c1, t));
    }
}

int game_particles_alive(const game_particles *ps)
{
    int i, n = 0;
    if (!ps) return 0;
    for (i = 0; i < ps->max; i++)
        if (ps->p[i].life > 0) n++;
    return n;
}
