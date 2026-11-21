/* rozOS — paint: মাউস দিয়ে আঁকা (ধাপ ১৩)
 *   বাঁ বোতাম টেনে আঁকা, ডান বোতাম মোছা (সাদা)। ওপরের সারি: রঙ, তুলির মাপ, বালতি (ভরাট),
 *   মুছে ফেলা, সংরক্ষণ।  কী: 1-9 রঙ, [ ] তুলি, f ভরাট, c মোছা, s সংরক্ষণ, q বের হওয়া
 *   paint FILE.bmp    ওই ছবি খুলে আঁকা (সংরক্ষণ সেখানেই); নইলে ~/drawing.bmp
 *   ধাপ ১৪খ: প্রথম সংরক্ষণে (বা S = Save as) ফাইল-ডায়ালগে নাম/ফোল্ডার বাছাই, তারপর
 *   ঠিক হল কিনা বার্তা-বাক্সে; o = অন্য ছবি খোলা (.bmp .png .jpg ...)। সংরক্ষণ BMP-তে। */

#include <stdio.h>
#include <string.h>
#include <rozgfx.h>
#include <rozui.h>
#include <errno.h>

#define BAR 44

static const uint32_t palette[9] = {
    GFX_RGB(0, 0, 0), GFX_RGB(255, 255, 255), GFX_RGB(220, 40, 40), GFX_RGB(250, 150, 30),
    GFX_RGB(250, 220, 40), GFX_RGB(40, 180, 70), GFX_RGB(40, 120, 230), GFX_RGB(140, 70, 200),
    GFX_RGB(120, 80, 40)
};

static gfx_surface *canvas;
static uint32_t color = GFX_RGB(0, 0, 0);
static int brush = 4, fill_mode;
static char status[160] = "left: draw  right: erase  s: save  S: save as  o: open  q: quit";
static char path[512] = "/home/drawing.bmp";
static int named;                       /* ধাপ ১৪খ: নাম বাছাই হয়েছে (বা কমান্ডে দেওয়া) */

static void draw_bar(gfx_surface *s)
{
    int i, W = gfx_width();
    char b[32];
    gfx_fill_rect(s, 0, 0, W, BAR, GFX_RGB(225, 225, 230));
    for (i = 0; i < 9; i++) {
        gfx_fill_rect(s, 8 + i * 34, 6, 28, 28, palette[i]);
        gfx_rect(s, 8 + i * 34, 6, 28, 28, palette[i] == color ? GFX_RGB(255, 0, 0) : GFX_GRAY);
        if (palette[i] == color) gfx_rect(s, 7 + i * 34, 5, 30, 30, GFX_RGB(255, 0, 0));
    }
    snprintf(b, sizeof(b), "[ %2d ]", brush);
    gfx_text(s, NULL, 330, 14, b, GFX_BLACK);
    gfx_fill_circle(s, 410, 20, brush / 2 + 1, color);
    gfx_text_bg(s, NULL, 440, 14, fill_mode ? " fill: ON " : " fill: off ", GFX_BLACK,
                fill_mode ? GFX_YELLOW : GFX_RGB(200, 200, 205));
    gfx_text_bg(s, NULL, 540, 14, " clear ", GFX_BLACK, GFX_RGB(200, 200, 205));
    gfx_text_bg(s, NULL, 610, 14, " save ", GFX_BLACK, GFX_RGB(200, 200, 205));
    gfx_text(s, NULL, 680, 14, status, GFX_RGB(40, 40, 60));
}

static int bar_click(int x)
{
    if (x >= 8 && x < 8 + 9 * 34) { color = palette[(x - 8) / 34]; return 1; }
    if (x >= 330 && x < 380) { brush = brush > 1 ? brush - 1 : 1; return 1; }
    if (x >= 380 && x < 430) { brush = brush < 40 ? brush + 1 : 40; return 1; }
    if (x >= 440 && x < 530) { fill_mode = !fill_mode; return 1; }
    if (x >= 540 && x < 600) { gfx_clear(canvas, GFX_WHITE); strcpy(status, "cleared"); return 1; }
    if (x >= 610 && x < 670) return 2;
    return 0;
}

/* ধাপ ১৪খ: .bmp না থাকলে যোগ করা (সংরক্ষণ সবসময় BMP) */
static void force_bmp(char *p, size_t size)
{
    size_t n = strlen(p);
    if (n < 4 || strcasecmp(p + n - 4, ".bmp")) {
        char *dot = strrchr(p, '.'), *sl = strrchr(p, '/');
        if (dot && (!sl || dot > sl)) *dot = '\0';
        strncat(p, ".bmp", size - strlen(p) - 1);
    }
}

static void save(int ask)
{
    char m[700];
    if (ask || !named) {
        char p[512];
        snprintf(p, sizeof(p), "%s", path);
        if (!ui_filedlg("Save picture (BMP)", p, sizeof(p), 1)) { snprintf(status, sizeof(status), "not saved"); return; }
        force_bmp(p, sizeof(p));
        snprintf(path, sizeof(path), "%s", p);
        named = 1;
    }
    if (gfx_save_bmp(canvas, path) == 0) {
        snprintf(status, sizeof(status), "saved %s", path);
        snprintf(m, sizeof(m), "Saved:\n%s (%dx%d)", path, canvas->w, canvas->h);
        ui_msgbox("Paint", m, UI_MB_OK | UI_MB_INFO);
    } else {
        snprintf(status, sizeof(status), "cannot save %s", path);
        snprintf(m, sizeof(m), "Cannot save %s:\n%s", path, strerror(errno));
        ui_msgbox("Paint", m, UI_MB_OK | UI_MB_ERROR);
    }
}

static void open_picture(void)
{
    char p[512];
    gfx_surface *img;
    snprintf(p, sizeof(p), "%s", path);
    if (!ui_filedlg("Open picture", p, sizeof(p), 0)) return;
    img = gfx_load_image(p);
    if (!img) {
        char m[700];
        snprintf(m, sizeof(m), "Cannot open %s:\n%s", p, gfx_image_error() ? gfx_image_error() : "unknown format");
        ui_msgbox("Paint", m, UI_MB_OK | UI_MB_ERROR);
        return;
    }
    gfx_clear(canvas, GFX_WHITE);
    gfx_blit(canvas, 0, 0, img, 0, 0, -1, -1);
    gfx_surface_free(img);
    snprintf(path, sizeof(path), "%s", p);
    force_bmp(path, sizeof(path));             /* .png খুললে সংরক্ষণ হবে একই নামের .bmp-তে */
    named = 1;
    snprintf(status, sizeof(status), "opened %s", p);
}

int main(int argc, char **argv)
{
    gfx_surface *s;
    gfx_event ev;
    int running = 1, down = 0, lx = 0, ly = 0, W, H;

    if (argc > 1) { snprintf(path, sizeof(path), "%s", argv[1]); named = 1; }
    if (gfx_init_window("Paint", 0, 0) < 0) {
        perror("paint: /dev/fb0");
        return 1;
    }
    s = gfx_screen();
    W = gfx_width();
    H = gfx_height();
    canvas = gfx_surface_new(W, H - BAR);
    if (!canvas) { gfx_quit(); return 1; }
    gfx_clear(canvas, GFX_WHITE);
    if (argc > 1) {
        gfx_surface *img = gfx_load_image(path);
        if (img) {
            gfx_blit(canvas, 0, 0, img, 0, 0, -1, -1);
            gfx_surface_free(img);
        }
    }
    while (running) {
        int mx, my;
        draw_bar(s);
        gfx_blit(s, 0, BAR, canvas, 0, 0, -1, -1);
        gfx_mouse_pos(&mx, &my);
        gfx_cursor_draw(s, mx, my);
        gfx_present();
        if (!gfx_wait_event(&ev, -1)) continue;
        do {
            int cx = ev.x, cy = ev.y - BAR;
            if (ev.type == GFX_EV_CLOSE) running = 0;           /* ধাপ ১৪ */
            if (ev.type == GFX_EV_RESIZE) {                     /* ধাপ ১৪: বড় কাগজ */
                gfx_surface *nc = gfx_surface_new(gfx_width(), gfx_height() - BAR > 1 ? gfx_height() - BAR : 1);
                if (nc) {
                    gfx_clear(nc, GFX_WHITE);
                    gfx_blit(nc, 0, 0, canvas, 0, 0, -1, -1);
                    gfx_surface_free(canvas);
                    canvas = nc;
                }
            }
            if (ev.type == GFX_EV_KEY) {
                int k = ev.key;
                if (k == 'q' || k == GFX_KEY_ESC) running = 0;
                else if (k >= '1' && k <= '9') color = palette[k - '1'];
                else if (k == '[') brush = brush > 1 ? brush - 1 : 1;
                else if (k == ']') brush = brush < 40 ? brush + 1 : 40;
                else if (k == 'f') fill_mode = !fill_mode;
                else if (k == 'c') gfx_clear(canvas, GFX_WHITE);
                else if (k == 's') save(0);
                else if (k == 'S') save(1);            /* ধাপ ১৪খ: অন্য নামে */
                else if (k == 'o') open_picture();
            } else if (ev.type == GFX_EV_MOUSE_DOWN) {
                if (ev.y < BAR) {
                    if (ev.button == GFX_BUTTON_LEFT && bar_click(ev.x) == 2) save(0);
                } else if (fill_mode && ev.button == GFX_BUTTON_LEFT) {
                    gfx_flood_fill(canvas, cx, cy, color);
                } else {
                    down = ev.button;
                    lx = cx; ly = cy;
                    gfx_fill_circle(canvas, cx, cy, brush / 2, down == GFX_BUTTON_RIGHT ? GFX_WHITE : color);
                }
            } else if (ev.type == GFX_EV_MOUSE_UP) {
                down = 0;
            } else if (ev.type == GFX_EV_MOUSE_MOVE && down && ev.y >= BAR) {
                gfx_thick_line(canvas, lx, ly, cx, cy, brush, down == GFX_BUTTON_RIGHT ? GFX_WHITE : color);
                lx = cx; ly = cy;
            } else if (ev.type == GFX_EV_MOUSE_WHEEL) {
                brush += ev.wheel < 0 ? 1 : -1;
                if (brush < 1) brush = 1;
                if (brush > 40) brush = 40;
            }
        } while (gfx_poll_event(&ev, 0));       /* জমে থাকা সব ঘটনা, তারপর একবার আঁকা */
    }
    gfx_surface_free(canvas);
    gfx_quit();
    return 0;
}
