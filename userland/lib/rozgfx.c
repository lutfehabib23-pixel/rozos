/* rozOS — librozgfx (ধাপ ১৩): পর্দা, আঁকা, ছবি, PSF ফন্ট, ইনপুট। rozgfx.h দেখুন।
 * TrueType অংশ আলাদা ফাইলে (rozgfx_ttf.c, stb_truetype)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/fb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <rozos.h>
#include "rozgfx.h"
#include "rozwm_proto.h"

/* ================= পর্দা ================= */

static int fb_fd = -1;
static struct rozos_fb_info fbi;
static uint8_t *fb_mem;
static gfx_surface *back;
static int same_format;            /* পর্দা XRGB8888 (red 16, green 8, blue 0) — সরাসরি কপি */

/* ---------- ধাপ ১৪: rozwm-এর উইন্ডো ---------- */
static int raw_on, keyev_fd = -1;   /* ধাপ ১৮ক: কাঁচা কী-ঘটনা (/dev/keyev) */
static int wm_fd = -1;             /* >= 0 মানে উইন্ডো-মোড */
static uint32_t wm_win;
static uint32_t *wm_px;
static uint32_t *wm_back;            /* ধাপ ১৪খ: অ্যাপের নিজের back buffer (shm-এ শুধু present-এ কপি) */
static size_t wm_px_size;
static unsigned char wm_rbuf[sizeof(struct wm_msg) * 8];
static size_t wm_rlen;
static struct wm_msg wm_queue[32];   /* WM_CREATED-এর অপেক্ষায় আসা অন্য বার্তা */
static int wm_nqueue;

static int wm_send(uint32_t type, int a, int b, int c, int d, uint32_t flags, const char *text)
{
    struct wm_msg m;
    size_t done = 0;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.win = wm_win;
    m.a = a; m.b = b; m.c = c; m.d = d;
    m.flags = flags;
    if (text) strncpy(m.text, text, sizeof(m.text) - 1);
    while (done < sizeof(m)) {
        ssize_t n = write(wm_fd, (char *)&m + done, sizeof(m) - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* একটা পুরো বার্তা (block = 1: আসা পর্যন্ত অপেক্ষা); 1 = পাওয়া, 0 = এখন নেই, -1 = বন্ধ */
static int wm_recv(struct wm_msg *m, int block)
{
    for (;;) {
        ssize_t n;
        if (wm_rlen >= sizeof(*m)) {
            memcpy(m, wm_rbuf, sizeof(*m));
            memmove(wm_rbuf, wm_rbuf + sizeof(*m), wm_rlen - sizeof(*m));
            wm_rlen -= sizeof(*m);
            return 1;
        }
        if (!block) {
            struct pollfd p = { wm_fd, POLLIN, 0 };
            if (poll(&p, 1, 0) <= 0) return 0;
        }
        n = read(wm_fd, wm_rbuf + wm_rlen, sizeof(wm_rbuf) - wm_rlen);
        if (n <= 0) return -1;
        wm_rlen += (size_t)n;
    }
}

static int wm_connect(const char *title, int w, int h)
{
    struct sockaddr_un a;
    struct wm_msg m;
    int fd, shm;
    const char *self = getenv("ROZWM_SERVER");
    if (self && self[0] == '1') return -1;          /* rozwm নিজে */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, ROZWM_SOCKET);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    wm_fd = fd;
    wm_send(WM_HELLO, (int)getpid(), 0, 0, 0, 0, NULL);
    wm_send(WM_CREATE, w, h, -1, -1, 0, title ? title : (__progname[0] ? __progname : "rozOS"));
    for (;;) {
        int r = wm_recv(&m, 1);
        if (r < 0) goto fail;
        if (m.type == WM_CREATED) break;
        if (wm_nqueue < 32) wm_queue[wm_nqueue++] = m;
    }
    wm_win = m.win;
    shm = shm_open(m.text, O_RDWR, 0);
    if (shm < 0) goto fail;
    wm_px_size = (size_t)m.c * (size_t)m.d * 4;      /* stride × বাফারের উচ্চতা */
    wm_px = (uint32_t *)mmap(NULL, wm_px_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    close(shm);
    if (wm_px == (uint32_t *)MAP_FAILED) goto fail;
    /* ধাপ ১৪খ: আঁকা হয় আলাদা মেমোরিতে, gfx_present তখন তৈরি ছবি shm-এ কপি করে — rozwm
     * কখনো আধা-আঁকা ফ্রেম দেখায় না (লেখার সময়/ডায়ালগে ঝিকমিক বন্ধ) */
    wm_back = (uint32_t *)calloc(wm_px_size / 4, 4);
    if (!wm_back) goto fail;
    back = gfx_surface_wrap(wm_back, m.a, m.b, m.c);
    if (!back) goto fail;
    return 0;
fail:
    if (wm_px && wm_px != (uint32_t *)MAP_FAILED) munmap(wm_px, wm_px_size);
    wm_px = NULL;
    free(wm_back);
    wm_back = NULL;
    close(fd);
    wm_fd = -1;
    return -1;
}

int gfx_windowed(void) { return wm_fd >= 0; }

/* ধাপ ১৪খ: [X]-এর পর প্রোগ্রাম নিজে প্রশ্ন করবে — rozwm যেন ৩ সেকেন্ডে বন্ধ না করে */
void gfx_keep_open(void)
{
    if (wm_fd >= 0) wm_send(WM_KEEP_OPEN, 0, 0, 0, 0, 0, NULL);
}

void gfx_set_title(const char *title)
{
    if (wm_fd >= 0) wm_send(WM_TITLE, 0, 0, 0, 0, 0, title);
}

int gfx_init_window(const char *title, int w, int h)
{
    if (back) return 0;
    if (wm_connect(title, w, h) == 0) return 0;
    return gfx_init_fullscreen();
}

int gfx_init(void)
{
    return gfx_init_window(NULL, 0, 0);
}

int gfx_init_fullscreen(void)
{
    if (back) return 0;
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return -1;
    if (ioctl(fb_fd, FBIO_ROZ_GETINFO, &fbi) < 0 || fbi.bpp != 32) {
        close(fb_fd);
        fb_fd = -1;
        errno = ENODEV;
        return -1;
    }
    fb_mem = (uint8_t *)mmap(NULL, (size_t)fbi.size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == (uint8_t *)MAP_FAILED) {
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    back = gfx_surface_new((int)fbi.width, (int)fbi.height);
    if (!back) {
        gfx_quit();
        errno = ENOMEM;
        return -1;
    }
    same_format = fbi.red_shift == 16 && fbi.green_shift == 8 && fbi.blue_shift == 0 &&
                  fbi.red_size == 8 && fbi.green_size == 8 && fbi.blue_size == 8;
    return 0;
}

void gfx_quit(void)
{
    if (wm_fd >= 0) {
        wm_send(WM_DESTROY, 0, 0, 0, 0, 0, NULL);
        if (back) back->px = NULL;                    /* wm_back নিচে আলাদা free */
        gfx_surface_free(back);
        back = NULL;
        munmap(wm_px, wm_px_size);
        wm_px = NULL;
        free(wm_back);
        wm_back = NULL;
        close(wm_fd);
        wm_fd = -1;
        return;
    }
    if (keyev_fd >= 0) {                              /* ধাপ ১৮ক */
        close(keyev_fd);
        keyev_fd = -1;
    }
    if (fb_mem) munmap(fb_mem, (size_t)fbi.size);
    fb_mem = NULL;
    if (fb_fd >= 0) {
        ioctl(fb_fd, KDSETMODE, KD_TEXT);
        close(fb_fd);
    }
    fb_fd = -1;
    gfx_surface_free(back);
    back = NULL;
}

gfx_surface *gfx_screen(void) { return back; }
int gfx_width(void)  { return back ? back->w : 0; }
int gfx_height(void) { return back ? back->h : 0; }

static uint32_t to_fb(uint32_t c)
{
    uint32_t r = GFX_R(c), g = GFX_G(c), b = GFX_B(c);
    return ((r >> (8 - fbi.red_size)) << fbi.red_shift) |
           ((g >> (8 - fbi.green_size)) << fbi.green_shift) |
           ((b >> (8 - fbi.blue_size)) << fbi.blue_shift);
}

void gfx_present_rect(int x, int y, int w, int h)
{
    int row;
    if (wm_fd >= 0 && back) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > back->w) w = back->w - x;
        if (y + h > back->h) h = back->h - y;
        if (w > 0 && h > 0) {
            for (row = y; row < y + h; row++)          /* তৈরি ছবি → shm */
                memcpy(wm_px + (size_t)row * back->stride + x, back->px + (size_t)row * back->stride + x, (size_t)w * 4);
            wm_send(WM_DAMAGE, x, y, w, h, 0, NULL);
        }
        return;
    }
    if (!back || !fb_mem) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > back->w) w = back->w - x;
    if (y + h > back->h) h = back->h - y;
    if (w <= 0 || h <= 0) return;
    for (row = y; row < y + h; row++) {
        uint32_t *dst = (uint32_t *)(fb_mem + (size_t)row * fbi.pitch) + x;
        const uint32_t *src = back->px + (size_t)row * back->stride + x;
        if (same_format) {
            memcpy(dst, src, (size_t)w * 4);
        } else {
            int i;
            for (i = 0; i < w; i++) dst[i] = to_fb(src[i]);
        }
    }
}

void gfx_present(void)
{
    if (back) gfx_present_rect(0, 0, back->w, back->h);
}

/* ================= surface ================= */

gfx_surface *gfx_surface_new(int w, int h)
{
    gfx_surface *s;
    if (w <= 0 || h <= 0) return NULL;
    s = (gfx_surface *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->px = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!s->px) {
        free(s);
        return NULL;
    }
    s->w = w;
    s->h = h;
    s->stride = w;
    s->owns_pixels = 1;
    gfx_reset_clip(s);
    return s;
}

gfx_surface *gfx_surface_wrap(uint32_t *px, int w, int h, int stride)
{
    gfx_surface *s = (gfx_surface *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    gfx_reset_clip(s);
    return s;
}

void gfx_surface_free(gfx_surface *s)
{
    if (!s) return;
    if (s->owns_pixels) free(s->px);
    free(s);
}

void gfx_set_clip(gfx_surface *s, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    s->clip_x = x; s->clip_y = y; s->clip_w = w; s->clip_h = h;
}

void gfx_reset_clip(gfx_surface *s)
{
    s->clip_x = 0; s->clip_y = 0; s->clip_w = s->w; s->clip_h = s->h;
}

#define IN_CLIP(s, x, y) ((x) >= (s)->clip_x && (y) >= (s)->clip_y && \
                          (x) < (s)->clip_x + (s)->clip_w && (y) < (s)->clip_y + (s)->clip_h)

/* ================= আঁকা ================= */

static uint32_t blend(uint32_t dst, uint32_t src)
{
    uint32_t a = GFX_A(src), ia = 255 - a;
    uint32_t r, g, b;
    if (a == 255) return src;
    if (a == 0) return dst;
    r = (GFX_R(src) * a + GFX_R(dst) * ia + 127) / 255;
    g = (GFX_G(src) * a + GFX_G(dst) * ia + 127) / 255;
    b = (GFX_B(src) * a + GFX_B(dst) * ia + 127) / 255;
    return GFX_RGBA(r, g, b, 255);
}

void gfx_pixel(gfx_surface *s, int x, int y, uint32_t c)
{
    if (IN_CLIP(s, x, y)) s->px[(size_t)y * s->stride + x] = c;
}

uint32_t gfx_get_pixel(const gfx_surface *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return s->px[(size_t)y * s->stride + x];
}

void gfx_blend_pixel(gfx_surface *s, int x, int y, uint32_t c)
{
    if (IN_CLIP(s, x, y)) {
        uint32_t *p = &s->px[(size_t)y * s->stride + x];
        *p = blend(*p, c);
    }
}

void gfx_fill_rect(gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    int x1 = x + w, y1 = y + h, row, i;
    if (x < s->clip_x) x = s->clip_x;
    if (y < s->clip_y) y = s->clip_y;
    if (x1 > s->clip_x + s->clip_w) x1 = s->clip_x + s->clip_w;
    if (y1 > s->clip_y + s->clip_h) y1 = s->clip_y + s->clip_h;
    for (row = y; row < y1; row++) {
        uint32_t *p = s->px + (size_t)row * s->stride;
        for (i = x; i < x1; i++) p[i] = c;
    }
}

void gfx_fill_rect_alpha(gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    int x1 = x + w, y1 = y + h, row, i;
    if (x < s->clip_x) x = s->clip_x;
    if (y < s->clip_y) y = s->clip_y;
    if (x1 > s->clip_x + s->clip_w) x1 = s->clip_x + s->clip_w;
    if (y1 > s->clip_y + s->clip_h) y1 = s->clip_y + s->clip_h;
    for (row = y; row < y1; row++) {
        uint32_t *p = s->px + (size_t)row * s->stride;
        for (i = x; i < x1; i++) p[i] = blend(p[i], c);
    }
}

void gfx_clear(gfx_surface *s, uint32_t c)
{
    int cx = s->clip_x, cy = s->clip_y, cw = s->clip_w, ch = s->clip_h;
    gfx_reset_clip(s);
    gfx_fill_rect(s, 0, 0, s->w, s->h, c);
    s->clip_x = cx; s->clip_y = cy; s->clip_w = cw; s->clip_h = ch;
}

void gfx_hline(gfx_surface *s, int x, int y, int w, uint32_t c) { gfx_fill_rect(s, x, y, w, 1, c); }
void gfx_vline(gfx_surface *s, int x, int y, int h, uint32_t c) { gfx_fill_rect(s, x, y, 1, h, c); }

void gfx_line(gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        int e2;
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_thick_line(gfx_surface *s, int x0, int y0, int x1, int y1, int width, uint32_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, r = width / 2;
    if (width <= 1) {
        gfx_line(s, x0, y0, x1, y1, c);
        return;
    }
    for (;;) {
        int e2;
        gfx_fill_circle(s, x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(s, x, y, w, c);
    gfx_hline(s, x, y + h - 1, w, c);
    gfx_vline(s, x, y, h, c);
    gfx_vline(s, x + w - 1, y, h, c);
}

void gfx_circle(gfx_surface *s, int cx, int cy, int r, uint32_t c)
{
    int x = r, y = 0, err = 1 - r;
    if (r < 0) return;
    while (x >= y) {
        gfx_pixel(s, cx + x, cy + y, c); gfx_pixel(s, cx + y, cy + x, c);
        gfx_pixel(s, cx - y, cy + x, c); gfx_pixel(s, cx - x, cy + y, c);
        gfx_pixel(s, cx - x, cy - y, c); gfx_pixel(s, cx - y, cy - x, c);
        gfx_pixel(s, cx + y, cy - x, c); gfx_pixel(s, cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_fill_circle(gfx_surface *s, int cx, int cy, int r, uint32_t c)
{
    int y;
    if (r < 0) return;
    for (y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r + r) w++;
        gfx_hline(s, cx - w, cy + y, 2 * w + 1, c);
    }
}

void gfx_ellipse(gfx_surface *s, int cx, int cy, int rx, int ry, uint32_t c)
{
    long a2 = (long)rx * rx, b2 = (long)ry * ry;
    int x, y, px = -1, py = ry;
    if (rx <= 0 || ry <= 0) return;
    for (x = 0; x <= rx; x++) {
        /* y = ry * sqrt(1 - x²/rx²), পূর্ণসংখ্যায় */
        long t = b2 - b2 * x * x / a2;
        y = 0;
        while ((long)(y + 1) * (y + 1) <= t) y++;
        if (px >= 0) {
            int yy;
            for (yy = y; yy <= py; yy++) {     /* খাড়া অংশে ফাঁক না থাকুক */
                gfx_pixel(s, cx + x, cy + yy, c); gfx_pixel(s, cx - x, cy + yy, c);
                gfx_pixel(s, cx + x, cy - yy, c); gfx_pixel(s, cx - x, cy - yy, c);
            }
        } else {
            gfx_pixel(s, cx, cy + y, c); gfx_pixel(s, cx, cy - y, c);
        }
        px = x; py = y;
    }
}

void gfx_round_rect(gfx_surface *s, int x, int y, int w, int h, int r, uint32_t c)
{
    int i;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_rect(s, x, y, w, h, c); return; }
    gfx_hline(s, x + r, y, w - 2 * r, c);
    gfx_hline(s, x + r, y + h - 1, w - 2 * r, c);
    gfx_vline(s, x, y + r, h - 2 * r, c);
    gfx_vline(s, x + w - 1, y + r, h - 2 * r, c);
    for (i = 0; i <= r; i++) {           /* চার কোণে বৃত্তের চতুর্থাংশ */
        int j = 0;
        while ((j + 1) * (j + 1) + i * i <= r * r) j++;
        gfx_pixel(s, x + r - j, y + r - i, c); gfx_pixel(s, x + w - 1 - r + j, y + r - i, c);
        gfx_pixel(s, x + r - j, y + h - 1 - r + i, c); gfx_pixel(s, x + w - 1 - r + j, y + h - 1 - r + i, c);
        gfx_pixel(s, x + r - i, y + r - j, c); gfx_pixel(s, x + w - 1 - r + i, y + r - j, c);
        gfx_pixel(s, x + r - i, y + h - 1 - r + j, c); gfx_pixel(s, x + w - 1 - r + i, y + h - 1 - r + j, c);
    }
}

void gfx_fill_round_rect(gfx_surface *s, int x, int y, int w, int h, int r, uint32_t c)
{
    int i;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_fill_rect(s, x, y, w, h, c); return; }
    gfx_fill_rect(s, x, y + r, w, h - 2 * r, c);
    for (i = 0; i < r; i++) {
        int dy = r - i, j = 0;
        while ((j + 1) * (j + 1) + dy * dy <= r * r) j++;
        gfx_hline(s, x + r - j, y + i, w - 2 * (r - j), c);
        gfx_hline(s, x + r - j, y + h - 1 - i, w - 2 * (r - j), c);
    }
}

void gfx_gradient_v(gfx_surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
    int i;
    if (h <= 0) return;
    for (i = 0; i < h; i++) {
        uint32_t t = (uint32_t)(h > 1 ? i * 255 / (h - 1) : 0), u = 255 - t;
        uint32_t c = GFX_RGB((GFX_R(top) * u + GFX_R(bot) * t) / 255,
                             (GFX_G(top) * u + GFX_G(bot) * t) / 255,
                             (GFX_B(top) * u + GFX_B(bot) * t) / 255);
        gfx_hline(s, x, y + i, w, c);
    }
}

void gfx_fill_triangle(gfx_surface *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c)
{
    int t, y;
    /* y অনুযায়ী সাজানো: y0 ≤ y1 ≤ y2 */
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 == y0) {
        int mn = x0, mx = x0;
        if (x1 < mn) mn = x1;
        if (x2 < mn) mn = x2;
        if (x1 > mx) mx = x1;
        if (x2 > mx) mx = x2;
        gfx_hline(s, mn, y0, mx - mn + 1, c);
        return;
    }
    for (y = y0; y <= y2; y++) {
        long xa = x0 + (long)(x2 - x0) * (y - y0) / (y2 - y0), xb;
        if (y < y1) xb = (y1 == y0) ? x1 : x0 + (long)(x1 - x0) * (y - y0) / (y1 - y0);
        else xb = (y2 == y1) ? x1 : x1 + (long)(x2 - x1) * (y - y1) / (y2 - y1);
        if (xa > xb) { long tt = xa; xa = xb; xb = tt; }
        gfx_hline(s, (int)xa, y, (int)(xb - xa + 1), c);
    }
}

/* scanline flood fill — নিজের স্ট্যাক (malloc), পুনরাবৃত্তি নয় */
void gfx_flood_fill(gfx_surface *s, int x, int y, uint32_t c)
{
    uint32_t target;
    int *stack, cap = 4096, n = 0;
    if (!IN_CLIP(s, x, y)) return;
    target = s->px[(size_t)y * s->stride + x];
    if (target == c) return;
    stack = (int *)malloc((size_t)cap * 2 * sizeof(int));
    if (!stack) return;
    stack[n * 2] = x; stack[n * 2 + 1] = y; n++;
    while (n > 0) {
        int sx, sy, lx, rx, i;
        n--;
        sx = stack[n * 2]; sy = stack[n * 2 + 1];
        if (s->px[(size_t)sy * s->stride + sx] != target) continue;
        lx = rx = sx;
        while (lx - 1 >= s->clip_x && s->px[(size_t)sy * s->stride + lx - 1] == target) lx--;
        while (rx + 1 < s->clip_x + s->clip_w && s->px[(size_t)sy * s->stride + rx + 1] == target) rx++;
        for (i = lx; i <= rx; i++) s->px[(size_t)sy * s->stride + i] = c;
        for (i = lx; i <= rx; i++) {
            int dir;
            for (dir = -1; dir <= 1; dir += 2) {
                int ny = sy + dir;
                if (ny < s->clip_y || ny >= s->clip_y + s->clip_h) continue;
                if (s->px[(size_t)ny * s->stride + i] != target) continue;
                if (i > lx && s->px[(size_t)ny * s->stride + i - 1] == target) continue;  /* একই অংশ */
                if (n == cap) {
                    int *ns = (int *)realloc(stack, (size_t)cap * 4 * sizeof(int));
                    if (!ns) { free(stack); return; }
                    stack = ns;
                    cap *= 2;
                }
                stack[n * 2] = i; stack[n * 2 + 1] = ny; n++;
            }
        }
    }
    free(stack);
}

static void blit_common(gfx_surface *dst, int dx, int dy, const gfx_surface *src,
                        int sx, int sy, int w, int h, int alpha)
{
    int row, i;
    if (w < 0) w = src->w;
    if (h < 0) h = src->h;
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx < dst->clip_x) { int d = dst->clip_x - dx; w -= d; sx += d; dx = dst->clip_x; }
    if (dy < dst->clip_y) { int d = dst->clip_y - dy; h -= d; sy += d; dy = dst->clip_y; }
    if (dx + w > dst->clip_x + dst->clip_w) w = dst->clip_x + dst->clip_w - dx;
    if (dy + h > dst->clip_y + dst->clip_h) h = dst->clip_y + dst->clip_h - dy;
    if (w <= 0 || h <= 0) return;
    for (row = 0; row < h; row++) {
        uint32_t *d = dst->px + (size_t)(dy + row) * dst->stride + dx;
        const uint32_t *sp = src->px + (size_t)(sy + row) * src->stride + sx;
        if (!alpha) memmove(d, sp, (size_t)w * 4);
        else for (i = 0; i < w; i++) d[i] = blend(d[i], sp[i]);
    }
}

void gfx_blit(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h)
{
    blit_common(dst, dx, dy, src, sx, sy, w, h, 0);
}

void gfx_blit_alpha(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h)
{
    blit_common(dst, dx, dy, src, sx, sy, w, h, 1);
}

void gfx_blit_scaled(gfx_surface *dst, int dx, int dy, int dw, int dh, const gfx_surface *src)
{
    int x, y;
    if (dw <= 0 || dh <= 0) return;
    for (y = 0; y < dh; y++) {
        int yy = dy + y, sy = (int)((long)y * src->h / dh);
        if (yy < dst->clip_y || yy >= dst->clip_y + dst->clip_h) continue;
        for (x = 0; x < dw; x++) {
            int xx = dx + x;
            if (xx < dst->clip_x || xx >= dst->clip_x + dst->clip_w) continue;
            dst->px[(size_t)yy * dst->stride + xx] = src->px[(size_t)sy * src->stride + (long)x * src->w / dw];
        }
    }
}

/* ================= ছবি ================= */

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint8_t *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (b && n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
        errno = EIO;
    }
    fclose(f);
    *size = n;
    return b;
}

static gfx_surface *load_bmp(const uint8_t *b, long size)
{
    uint32_t off, hsize, comp;
    int32_t w, h;
    int bpp, y, x, top_down = 0;
    long stride;
    gfx_surface *s;
    if (size < 54) return NULL;
    off = rd32(b + 10);
    hsize = rd32(b + 14);
    w = (int32_t)rd32(b + 18);
    h = (int32_t)rd32(b + 22);
    bpp = (int)rd16(b + 28);
    comp = rd32(b + 30);
    if (hsize < 40 || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) return NULL;
    if (h < 0) { h = -h; top_down = 1; }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    stride = ((long)w * (bpp / 8) + 3) & ~3L;
    if ((long)off + stride * h > size) return NULL;
    s = gfx_surface_new(w, h);
    if (!s) return NULL;
    for (y = 0; y < h; y++) {
        const uint8_t *row = b + off + stride * (top_down ? y : h - 1 - y);
        for (x = 0; x < w; x++) {
            const uint8_t *p = row + x * (bpp / 8);
            uint32_t a = bpp == 32 ? p[3] : 255;
            if (bpp == 32 && comp == 0) a = 255;         /* BI_RGB-এর 4র্থ বাইট সাধারণত ফাঁকা */
            s->px[(size_t)y * s->stride + x] = GFX_RGBA(p[2], p[1], p[0], a);
        }
    }
    return s;
}

static int ppm_num(const uint8_t *b, long size, long *pos)
{
    int v = 0, any = 0;
    while (*pos < size) {
        if (b[*pos] == '#') { while (*pos < size && b[*pos] != '\n') (*pos)++; }
        else if (b[*pos] == ' ' || b[*pos] == '\t' || b[*pos] == '\r' || b[*pos] == '\n') (*pos)++;
        else break;
    }
    while (*pos < size && b[*pos] >= '0' && b[*pos] <= '9') { v = v * 10 + (b[*pos] - '0'); (*pos)++; any = 1; }
    return any ? v : -1;
}

static gfx_surface *load_ppm(const uint8_t *b, long size)
{
    long pos = 2;
    int w, h, maxv, x, y, ascii = b[1] == '3';
    gfx_surface *s;
    w = ppm_num(b, size, &pos);
    h = ppm_num(b, size, &pos);
    maxv = ppm_num(b, size, &pos);
    if (w <= 0 || h <= 0 || maxv <= 0 || maxv > 255 || w > 16384 || h > 16384) return NULL;
    pos++;                                       /* মাথার পরে একটা ফাঁকা */
    if (!ascii && pos + (long)w * h * 3 > size) return NULL;
    s = gfx_surface_new(w, h);
    if (!s) return NULL;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int r, g, bl;
            if (ascii) {
                r = ppm_num(b, size, &pos); g = ppm_num(b, size, &pos); bl = ppm_num(b, size, &pos);
                if (r < 0 || g < 0 || bl < 0) { gfx_surface_free(s); return NULL; }
            } else {
                r = b[pos]; g = b[pos + 1]; bl = b[pos + 2];
                pos += 3;
            }
            s->px[(size_t)y * s->stride + x] = GFX_RGB(r * 255 / maxv, g * 255 / maxv, bl * 255 / maxv);
        }
    }
    return s;
}

gfx_surface *gfx_load_image(const char *path)
{
    long size;
    uint8_t *b = read_file(path, &size);
    gfx_surface *s = NULL;
    if (!b) return NULL;
    if (size >= 2 && b[0] == 'B' && b[1] == 'M') s = load_bmp(b, size);
    else if (size >= 2 && b[0] == 'P' && (b[1] == '6' || b[1] == '3')) s = load_ppm(b, size);
    if (!s) s = gfx_load_image_stb(b, size);          /* ধাপ ১৪: JPEG, PNG, GIF, TGA, অন্য BMP */
    free(b);
    if (!s) errno = EINVAL;
    return s;
}

static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

int gfx_save_bmp(const gfx_surface *s, const char *path)
{
    uint8_t hdr[54];
    long stride = ((long)s->w * 3 + 3) & ~3L;
    uint8_t *row;
    FILE *f;
    int y, x;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    wr32(hdr + 2, (uint32_t)(54 + stride * s->h));
    wr32(hdr + 10, 54);
    wr32(hdr + 14, 40);
    wr32(hdr + 18, (uint32_t)s->w);
    wr32(hdr + 22, (uint32_t)s->h);
    wr16(hdr + 26, 1);
    wr16(hdr + 28, 24);
    wr32(hdr + 34, (uint32_t)(stride * s->h));
    f = fopen(path, "wb");
    if (!f) return -1;
    row = (uint8_t *)calloc(1, (size_t)stride);
    if (!row) { fclose(f); return -1; }
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = s->h - 1; y >= 0; y--) {
        for (x = 0; x < s->w; x++) {
            uint32_t c = s->px[(size_t)y * s->stride + x];
            row[x * 3] = (uint8_t)GFX_B(c); row[x * 3 + 1] = (uint8_t)GFX_G(c); row[x * 3 + 2] = (uint8_t)GFX_R(c);
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    return fclose(f) == 0 ? 0 : -1;
}

/* ================= PSF ফন্ট ================= */

struct gfx_font {
    int w, h, bpr, count;
    uint8_t *glyphs;
};

gfx_font *gfx_font_load(const char *path)
{
    long size;
    uint8_t *b = read_file(path, &size);
    gfx_font *f;
    if (!b) return NULL;
    f = (gfx_font *)calloc(1, sizeof(*f));
    if (!f) { free(b); return NULL; }
    if (size >= 32 && rd32(b) == 0x864AB572) {
        uint32_t hdr = rd32(b + 8), count = rd32(b + 16), cs = rd32(b + 20);
        f->h = (int)rd32(b + 24);
        f->w = (int)rd32(b + 28);
        f->bpr = (f->w + 7) / 8;
        f->count = (int)count;
        if (cs != (uint32_t)(f->h * f->bpr) || (long)(hdr + count * cs) > size || count == 0) goto bad;
        f->glyphs = (uint8_t *)malloc(count * cs);
        if (!f->glyphs) goto bad;
        memcpy(f->glyphs, b + hdr, count * cs);
    } else if (size >= 4 && b[0] == 0x36 && b[1] == 0x04) {
        f->w = 8;
        f->h = b[3];
        f->bpr = 1;
        f->count = (b[2] & 1) ? 512 : 256;
        if ((long)(4 + f->count * f->h) > size) goto bad;
        f->glyphs = (uint8_t *)malloc((size_t)f->count * f->h);
        if (!f->glyphs) goto bad;
        memcpy(f->glyphs, b + 4, (size_t)f->count * f->h);
    } else {
        goto bad;
    }
    free(b);
    return f;
bad:
    free(b);
    free(f);
    errno = EINVAL;
    return NULL;
}

gfx_font *gfx_font_default(void)
{
    static gfx_font *def;
    if (!def) def = gfx_font_load("/usr/share/fonts/rozos-mono-16.psf");
    return def;
}

void gfx_font_free(gfx_font *f)
{
    if (!f || f == gfx_font_default()) return;
    free(f->glyphs);
    free(f);
}

int gfx_font_width(const gfx_font *f)  { return f ? f->w : 8; }
int gfx_font_height(const gfx_font *f) { return f ? f->h : 16; }

/* UTF-8 → অক্ষর; ফন্টে CP437 ক্রম, তাই Latin-1/বক্স ছাড়া বাকি '?' */
static int next_char(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int c = *p++;
    if (c >= 0x80) {
        int cp = 0, extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
        c = cp < 256 ? cp : '?';
        if (c >= 0x80) {
            /* Latin-1 → CP437 (যেগুলো আছে) */
            static const unsigned short map[][2] = {
                {0xE9,0x82},{0xE8,0x8A},{0xEA,0x88},{0xE0,0x85},{0xE2,0x83},{0xE4,0x84},{0xE7,0x87},
                {0xF6,0x94},{0xFC,0x81},{0xD6,0x99},{0xDC,0x9A},{0xC4,0x8E},{0xF1,0xA4},{0xD1,0xA5},
                {0xDF,0xE1},{0xB0,0xF8},{0xB1,0xF1},{0xB5,0xE6},{0xA3,0x9C},{0xA9,'c'},{0xB7,0xFA}
            };
            size_t i;
            int out = '?';
            for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (map[i][0] == c) out = map[i][1];
            c = out;
        }
    }
    *pp = (const char *)p;
    return c;
}

static int draw_text(gfx_surface *s, gfx_font *f, int x, int y, const char *str,
                     uint32_t fg, uint32_t bg, int with_bg)
{
    int x0 = x;
    if (!f) f = gfx_font_default();
    if (!f) return x;
    while (*str) {
        int c = next_char(&str), gy, gx;
        const uint8_t *g;
        if (c == '\n') { x = x0; y += f->h; continue; }
        if (c >= f->count) c = '?';
        g = f->glyphs + (size_t)c * f->h * f->bpr;
        for (gy = 0; gy < f->h; gy++) {
            const uint8_t *row = g + gy * f->bpr;
            for (gx = 0; gx < f->w; gx++) {
                if (row[gx >> 3] & (0x80 >> (gx & 7))) gfx_pixel(s, x + gx, y + gy, fg);
                else if (with_bg) gfx_pixel(s, x + gx, y + gy, bg);
            }
        }
        x += f->w;
    }
    return x;
}

void gfx_glyph(gfx_surface *s, gfx_font *f, int x, int y, int index, uint32_t fg, uint32_t bg)
{
    const uint8_t *g;
    int gy, gx;
    if (!f) f = gfx_font_default();
    if (!f) return;
    if (index < 0 || index >= f->count) index = '?';
    g = f->glyphs + (size_t)index * f->h * f->bpr;
    for (gy = 0; gy < f->h; gy++) {
        const uint8_t *row = g + gy * f->bpr;
        int yy = y + gy;
        if (yy < s->clip_y || yy >= s->clip_y + s->clip_h) continue;
        for (gx = 0; gx < f->w; gx++) {
            int xx = x + gx;
            if (xx < s->clip_x || xx >= s->clip_x + s->clip_w) continue;
            s->px[(size_t)yy * s->stride + xx] = (row[gx >> 3] & (0x80 >> (gx & 7))) ? fg : bg;
        }
    }
}

int gfx_text(gfx_surface *s, gfx_font *f, int x, int y, const char *str, uint32_t c)
{
    return draw_text(s, f, x, y, str, c, 0, 0);
}

int gfx_text_bg(gfx_surface *s, gfx_font *f, int x, int y, const char *str, uint32_t fg, uint32_t bg)
{
    return draw_text(s, f, x, y, str, fg, bg, 1);
}

int gfx_text_width(gfx_font *f, const char *str)
{
    int n = 0, best = 0;
    if (!f) f = gfx_font_default();
    while (*str) {
        int c = next_char(&str);
        if (c == '\n') { if (n > best) best = n; n = 0; }
        else n++;
    }
    if (n > best) best = n;
    return best * (f ? f->w : 8);
}

/* ================= ইনপুট ================= */

static int con_fd = -1, mouse_fd = -1;
static int kbd_only;                /* ধাপ ১৪: con_fd = /dev/kbd (int32 কী-কোড) */

void gfx_input_keyboard_only(int on) { kbd_only = on; }
static int mouse_x, mouse_y, mouse_buttons;
static unsigned char kbuf[64];
static int klen;
static gfx_event pending[64];       /* ধাপ ১৮ক: 16 → 64 (কাঁচা কী-ঘটনাও আসে) */
static int npending;

static void open_inputs(void)
{
    if (con_fd < 0) {
        if (kbd_only) con_fd = open("/dev/kbd", O_RDONLY);
        if (con_fd < 0) con_fd = 0;                   /* stdin = /dev/console */
        else kbd_only = 2;                            /* সত্যিই /dev/kbd খোলা */
    }
    if (mouse_fd < 0) mouse_fd = open("/dev/mouse", O_RDONLY);
}

int gfx_raw_keys(int on)
{
    raw_on = on ? 1 : 0;
    if (!raw_on) return 0;
    if (wm_fd >= 0) return 1;                         /* rozwm WM_RAWKEY পাঠায় */
    if (keyev_fd < 0) keyev_fd = open("/dev/keyev", O_RDONLY);
    return keyev_fd >= 0;
}

static void push(const gfx_event *e);

/* কার্নেলের int32 (বিট 0-15 কোড, 16-18 Shift/Ctrl/Alt, 29 repeat, 30 ছাড়া) → ঘটনা */
static void raw_event(int32_t v)
{
    gfx_event e;
    int m = (v >> 16) & 7;
    memset(&e, 0, sizeof(e));
    e.type = (v & 0x40000000) ? GFX_EV_KEY_UP : GFX_EV_KEY_DOWN;
    e.key = v & 0xFFFF;
    e.repeat = (v & 0x20000000) ? 1 : 0;
    e.mods = ((m & 1) ? GFX_MOD_SHIFT : 0) | ((m & 2) ? GFX_MOD_CTRL : 0) | ((m & 4) ? GFX_MOD_ALT : 0);
    e.x = mouse_x; e.y = mouse_y; e.buttons = mouse_buttons;
    if (e.key) push(&e);
}

static void read_keyev(void)
{
    int32_t evs[32];
    ssize_t r = read(keyev_fd, evs, sizeof(evs)), i;
    for (i = 0; r > 0 && i < r / (ssize_t)sizeof(evs[0]); i++) raw_event(evs[i]);
}

static void push(const gfx_event *e)
{
    if (npending < (int)(sizeof(pending) / sizeof(pending[0]))) pending[npending++] = *e;
}

static int key_mods;               /* ধাপ ১৪: পরের key_event-এর modifier */

static void key_event(int key)
{
    gfx_event e;
    memset(&e, 0, sizeof(e));
    e.type = GFX_EV_KEY;
    e.key = key;
    e.mods = key_mods;
    key_mods = 0;
    e.x = mouse_x; e.y = mouse_y; e.buttons = mouse_buttons;
    push(&e);
}

/* kbuf থেকে কী বের করা: ESC [ ... (তীর ইত্যাদি)। final = 1: অসম্পূর্ণ ESC-কে Esc ধরা */
static void parse_keys(int final)
{
    int i = 0;
    while (i < klen) {
        int c = kbuf[i];
        if (c == 27) {
            int j, num = 0, key = 0;
            if (i + 1 >= klen) {
                if (!final) break;
                key_event(GFX_KEY_ESC);
                i++;
                continue;
            }
            if (kbuf[i + 1] != '[' && kbuf[i + 1] != 'O') {
                key_event(GFX_KEY_ESC);
                i++;
                continue;
            }
            for (j = i + 2; j < klen && ((kbuf[j] >= '0' && kbuf[j] <= '9') || kbuf[j] == ';'); j++) {
                if (kbuf[j] == ';') num = 0;          /* modifier অংশ বাদ */
                else if (!num || j == i + 2 || kbuf[j - 1] != ';') num = num * 10 + (kbuf[j] - '0');
            }
            if (j >= klen) {
                if (!final) break;
                key_event(GFX_KEY_ESC);
                i++;
                continue;
            }
            /* ESC[1;5A-এর মতো হলে প্রথম সংখ্যাই চাই — নতুন করে পড়া; ;-এর পরেরটা modifier */
            num = 0;
            {
                int k = i + 2, mod = 0;
                while (k < j && kbuf[k] >= '0' && kbuf[k] <= '9') num = num * 10 + (kbuf[k++] - '0');
                if (k < j && kbuf[k] == ';') {
                    k++;
                    while (k < j && kbuf[k] >= '0' && kbuf[k] <= '9') mod = mod * 10 + (kbuf[k++] - '0');
                }
                key_mods = mod > 1 ? mod - 1 : 0;           /* xterm: 1 + (shift 1, alt 2, ctrl 4) */
            }
            switch (kbuf[j]) {
            case 'A': key = GFX_KEY_UP; break;
            case 'B': key = GFX_KEY_DOWN; break;
            case 'C': key = GFX_KEY_RIGHT; break;
            case 'D': key = GFX_KEY_LEFT; break;
            case 'H': key = GFX_KEY_HOME; break;
            case 'F': key = GFX_KEY_END; break;
            case 'P': key = GFX_KEY_F1; break;
            case 'Q': key = GFX_KEY_F2; break;
            case 'R': key = GFX_KEY_F3; break;
            case 'S': key = GFX_KEY_F4; break;
            case '~':
                switch (num) {
                case 1: case 7: key = GFX_KEY_HOME; break;
                case 4: case 8: key = GFX_KEY_END; break;
                case 2: key = GFX_KEY_INSERT; break;
                case 3: key = GFX_KEY_DELETE; break;
                case 5: key = GFX_KEY_PAGE_UP; break;
                case 6: key = GFX_KEY_PAGE_DOWN; break;
                case 15: key = GFX_KEY_F5; break;
                case 17: key = GFX_KEY_F6; break;
                case 18: key = GFX_KEY_F7; break;
                case 19: key = GFX_KEY_F8; break;
                case 20: key = GFX_KEY_F9; break;
                case 21: key = GFX_KEY_F10; break;
                case 23: key = GFX_KEY_F11; break;
                case 24: key = GFX_KEY_F12; break;
                }
                break;
            }
            if (key) key_event(key);
            else key_mods = 0;
            i = j + 1;
            continue;
        }
        if (c == '\r') c = '\n';
        if (c == 127) c = '\b';
        key_event(c);
        i++;
    }
    if (i > 0) {
        memmove(kbuf, kbuf + i, (size_t)(klen - i));
        klen -= i;
    }
}

static void read_mouse(void)
{
    struct { int32_t x, y, dx, dy; uint8_t buttons; int8_t wheel; uint8_t pad[2]; } m[16];
    ssize_t n = read(mouse_fd, m, sizeof(m));
    int i, cnt = n > 0 ? (int)(n / (ssize_t)sizeof(m[0])) : 0;
    for (i = 0; i < cnt; i++) {
        gfx_event e;
        int changed = (m[i].buttons ^ mouse_buttons) & 7, b;
        memset(&e, 0, sizeof(e));
        e.x = m[i].x; e.y = m[i].y; e.dx = m[i].dx; e.dy = m[i].dy;
        mouse_x = m[i].x; mouse_y = m[i].y;
        if (m[i].dx || m[i].dy) {
            e.type = GFX_EV_MOUSE_MOVE;
            e.buttons = mouse_buttons;
            push(&e);
        }
        for (b = 1; b <= 4; b <<= 1) {
            if (changed & b) {
                e.type = (m[i].buttons & b) ? GFX_EV_MOUSE_DOWN : GFX_EV_MOUSE_UP;
                e.button = b;
                e.buttons = m[i].buttons;
                push(&e);
            }
        }
        mouse_buttons = m[i].buttons;
        if (m[i].wheel) {
            e.type = GFX_EV_MOUSE_WHEEL;
            e.wheel = m[i].wheel < 0 ? -1 : 1;
            e.buttons = mouse_buttons;
            push(&e);
        }
    }
}

/* ধাপ ১৪: rozwm-এর বার্তা → gfx_event */
static void wm_to_event(const struct wm_msg *m)
{
    gfx_event e;
    memset(&e, 0, sizeof(e));
    switch (m->type) {
    case WM_KEY:
        e.type = GFX_EV_KEY;
        e.key = m->a;
        e.mods = m->b;
        break;
    case WM_MOUSE:
        e.type = (int)m->flags;
        e.x = m->a; e.y = m->b;
        e.dx = e.x - mouse_x; e.dy = e.y - mouse_y;
        e.buttons = m->c;
        if (e.type == GFX_EV_MOUSE_WHEEL) e.wheel = m->d;
        else e.button = m->d;
        mouse_x = m->a; mouse_y = m->b; mouse_buttons = m->c;
        break;
    case WM_CLOSE:
        e.type = GFX_EV_CLOSE;
        break;
    case WM_RESIZE:
        if (back && m->a > 0 && m->b > 0) {
            back->w = m->a;
            back->h = m->b;
            gfx_reset_clip(back);
        }
        e.type = GFX_EV_RESIZE;
        e.x = m->a; e.y = m->b;
        break;
    case WM_FOCUS:
        e.type = GFX_EV_FOCUS;
        e.key = m->a;
        break;
    case WM_RAWKEY:                                   /* ধাপ ১৮ক */
        if (raw_on) raw_event(m->a);
        return;
    default:
        return;
    }
    e.x = e.type == GFX_EV_RESIZE ? e.x : (e.type == GFX_EV_KEY ? mouse_x : e.x);
    if (e.type == GFX_EV_KEY) e.y = mouse_y;
    push(&e);
}

static int wm_poll_event(gfx_event *ev, int timeout_ms)
{
    long end = timeout_ms > 0 ? gfx_ticks_ms() + timeout_ms : 0;
    while (wm_nqueue > 0) {
        wm_to_event(&wm_queue[0]);
        wm_nqueue--;
        memmove(wm_queue, wm_queue + 1, (size_t)wm_nqueue * sizeof(wm_queue[0]));
    }
    for (;;) {
        struct wm_msg m;
        int r, wait;
        if (npending > 0) {
            *ev = pending[0];
            npending--;
            memmove(pending, pending + 1, (size_t)npending * sizeof(pending[0]));
            return 1;
        }
        while ((r = wm_recv(&m, 0)) == 1) wm_to_event(&m);
        if (r < 0) {                                  /* rozwm বন্ধ — প্রোগ্রামও বন্ধ হোক */
            gfx_event e;
            memset(&e, 0, sizeof(e));
            e.type = GFX_EV_CLOSE;
            push(&e);
            continue;
        }
        if (npending > 0) continue;
        if (timeout_ms == 0) return 0;
        if (timeout_ms < 0) wait = -1;
        else {
            long left = end - gfx_ticks_ms();
            if (left <= 0) return 0;
            wait = (int)left;
        }
        {
            struct pollfd p = { wm_fd, POLLIN, 0 };
            poll(&p, 1, wait);
        }
    }
}

int gfx_event_fds(int *fds, int max)
{
    int n = 0;
    if (wm_fd >= 0) {
        if (max > 0) fds[n++] = wm_fd;
        return n;
    }
    open_inputs();
    if (max > n) fds[n++] = con_fd;
    if (mouse_fd >= 0 && max > n) fds[n++] = mouse_fd;
    if (keyev_fd >= 0 && max > n) fds[n++] = keyev_fd;   /* ধাপ ১৮ক */
    return n;
}

int gfx_poll_event(gfx_event *ev, int timeout_ms)
{
    long end = timeout_ms > 0 ? gfx_ticks_ms() + timeout_ms : 0;
    if (wm_fd >= 0) return wm_poll_event(ev, timeout_ms);
    open_inputs();
    for (;;) {
        struct pollfd p[3];
        int n, wait, np, kp = -1;
        if (npending > 0) {
            *ev = pending[0];
            npending--;
            memmove(pending, pending + 1, (size_t)npending * sizeof(pending[0]));
            return 1;
        }
        p[0].fd = con_fd; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = mouse_fd; p[1].events = POLLIN; p[1].revents = 0;
        np = mouse_fd >= 0 ? 2 : 1;
        if (raw_on && keyev_fd >= 0) {                /* ধাপ ১৮ক */
            kp = np++;
            p[kp].fd = keyev_fd; p[kp].events = POLLIN; p[kp].revents = 0;
        }
        if (timeout_ms < 0) wait = -1;
        else if (timeout_ms == 0) wait = 0;
        else {
            long left = end - gfx_ticks_ms();
            wait = left > 0 ? (int)left : 0;
        }
        /* অর্ধেক-আসা ESC sequence: বেশিক্ষণ অপেক্ষা নয় */
        if (klen > 0 && (wait < 0 || wait > 30)) wait = 30;
        n = poll(p, np, wait);
        if (n > 0 && (p[0].revents & POLLIN) && kbd_only == 2) {
            int32_t keys[32];
            ssize_t r = read(con_fd, keys, sizeof(keys)), i;
            for (i = 0; r > 0 && i < r / (ssize_t)sizeof(keys[0]); i++) {
                int k = keys[i] & 0xFFFF, m = (keys[i] >> 16) & 7;
                /* কার্নেল: Shift 1, Ctrl 2, Alt 4  →  GFX_MOD: Shift 1, Alt 2, Ctrl 4 */
                key_mods = ((m & 1) ? GFX_MOD_SHIFT : 0) | ((m & 2) ? GFX_MOD_CTRL : 0) | ((m & 4) ? GFX_MOD_ALT : 0);
                key_event(k);
            }
        } else if (n > 0 && (p[0].revents & POLLIN)) {
            ssize_t r = read(con_fd, kbuf + klen, sizeof(kbuf) - (size_t)klen);
            if (r > 0) klen += (int)r;
            parse_keys(0);
        } else if (klen > 0) {
            parse_keys(1);                 /* সময় শেষ — একা ESC = Esc কী */
        }
        if (n > 0 && mouse_fd >= 0 && (p[1].revents & POLLIN)) read_mouse();
        if (n > 0 && kp >= 0 && (p[kp].revents & POLLIN)) read_keyev();
        if (npending > 0) continue;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && gfx_ticks_ms() >= end) return 0;
    }
}

int gfx_wait_event(gfx_event *ev, int timeout_ms)
{
    return gfx_poll_event(ev, timeout_ms);
}

void gfx_mouse_pos(int *x, int *y)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}

/* 12x19 তীর: '#' কালো ধার, '.' সাদা ভেতর */
static const char *const arrow[] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
    "#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
    "#.........# ", "#..........#", "#......#####", "#...#..#    ", "#..# #..#   ",
    "#.#  #..#   ", "##    #..#  ", "      #..#  ", "       ##   "
};
#define CUR_W 12
#define CUR_H 19
static uint32_t under[CUR_W * CUR_H];
static int under_x, under_y, under_valid;

void gfx_cursor_draw(gfx_surface *s, int x, int y)
{
    int i, j;
    if (wm_fd >= 0) return;                 /* ধাপ ১৪: ডেস্কটপে তীর rozwm নিজে আঁকে */
    for (j = 0; j < CUR_H; j++)
        for (i = 0; i < CUR_W; i++)
            under[j * CUR_W + i] = gfx_get_pixel(s, x + i, y + j);
    under_x = x; under_y = y; under_valid = 1;
    for (j = 0; j < CUR_H; j++)
        for (i = 0; i < CUR_W; i++) {
            char c = arrow[j][i];
            if (c == '#') gfx_pixel(s, x + i, y + j, GFX_BLACK);
            else if (c == '.') gfx_pixel(s, x + i, y + j, GFX_WHITE);
        }
}

void gfx_cursor_restore(gfx_surface *s)
{
    int i, j;
    if (wm_fd >= 0) return;
    if (!under_valid) return;
    for (j = 0; j < CUR_H; j++)
        for (i = 0; i < CUR_W; i++)
            if (under_x + i < s->w && under_y + j < s->h)
                gfx_pixel(s, under_x + i, under_y + j, under[j * CUR_W + i]);
    under_valid = 0;
}

/* ================= সময় ================= */

long gfx_ticks_ms(void) { return rozos_now_ms(); }

void gfx_sleep_ms(int ms)
{
    if (ms > 0) usleep((unsigned long)ms * 1000UL);
}
