/* rozOS — gcalc: গ্রাফিক্যাল ক্যালকুলেটর (ধাপ ১৩)
 *   মাউসে বোতাম চাপা, বা কীবোর্ডে: 0-9 . + - * / ( ) %   Enter / = হিসাব
 *   Backspace একটা মোছা, Esc/c সব মোছা, q বের হওয়া
 *   হিসাব double-এ, বন্ধনী আর অগ্রাধিকার মেনে (recursive descent)।
 *   ধাপ ১৯ঘ: ছোট উইন্ডো (২৪০x৩৩০) — সব মাপ উইন্ডো থেকে হিসাব, কোণে টেনে বড় করলে বোতাম আর লেখা
 *   সেই অনুপাতে বড় হয়। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rozgfx.h>

static char expr[128];
static char result[64] = "0";
static int error;

/* ---------- হিসাব ---------- */
static const char *ep;
static double parse_sum(void);

static void skip(void) { while (*ep == ' ') ep++; }

static double parse_atom(void)
{
    double v;
    skip();
    if (*ep == '(') {
        ep++;
        v = parse_sum();
        skip();
        if (*ep == ')') ep++; else error = 1;
        return v;
    }
    if (*ep == '-') { ep++; return -parse_atom(); }
    if (*ep == '+') { ep++; return parse_atom(); }
    if ((*ep >= '0' && *ep <= '9') || *ep == '.') {
        char *end;
        v = strtod(ep, &end);
        if (end == ep) error = 1;
        ep = end;
        skip();
        if (*ep == '%') { ep++; v /= 100.0; }
        return v;
    }
    error = 1;
    return 0;
}

static double parse_prod(void)
{
    double v = parse_atom();
    for (;;) {
        skip();
        if (*ep == '*') { ep++; v *= parse_atom(); }
        else if (*ep == '/') {
            double d;
            ep++;
            d = parse_atom();
            if (d == 0) error = 2;
            else v /= d;
        } else return v;
    }
}

static double parse_sum(void)
{
    double v = parse_prod();
    for (;;) {
        skip();
        if (*ep == '+') { ep++; v += parse_prod(); }
        else if (*ep == '-') { ep++; v -= parse_prod(); }
        else return v;
    }
}

static void evaluate(void)
{
    double v;
    if (!expr[0]) return;
    error = 0;
    ep = expr;
    v = parse_sum();
    skip();
    if (*ep) error = 1;
    if (error == 2) strcpy(result, "division by zero");
    else if (error) strcpy(result, "error");
    else {
        snprintf(result, sizeof(result), "%.10g", v);
        snprintf(expr, sizeof(expr), "%s", result);
    }
}

static void add(const char *t)
{
    if (strlen(expr) + strlen(t) < sizeof(expr) - 1) strcat(expr, t);
}

/* ---------- বোতাম ---------- */
static const char *labels[5][4] = {
    { "C", "(", ")", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", ".", "<", "=" },
};
/* ধাপ ১৯ঘ: বাঁধা মাপের বদলে উইন্ডোর মাপ থেকে বিন্যাস (layout()) */
static int ox, oy, pw, ph, bw, bh, gap, disp_h, hint;

static void layout(void)
{
    int W = gfx_width(), H = gfx_height();
    if (W < 160) W = 160;
    if (H < 220) H = 220;
    gap = W / 40 > 3 ? W / 40 : 3;
    hint = H >= 400 ? 20 : 0;                     /* বড় উইন্ডোতে নিচে কী-এর তালিকা */
    pw = W - 2 * gap;
    ph = H - 2 * gap - hint;
    ox = gap;
    oy = gap;
    disp_h = ph * 22 / 100;
    bw = (pw - 5 * gap) / 4;
    bh = (ph - disp_h - 7 * gap) / 5;
    if (bw < 20) bw = 20;
    if (bh < 16) bh = 16;
}

static uint32_t lighter(uint32_t col)
{
    return GFX_RGB(GFX_R(col) + 25 > 255 ? 255 : GFX_R(col) + 25, GFX_G(col) + 25 > 255 ? 255 : GFX_G(col) + 25,
                   GFX_B(col) + 25 > 255 ? 255 : GFX_B(col) + 25);
}

/* লেখা ঘরে না ধরলে ছোট করে আঁকা (ডান দিকে মিলিয়ে) */
static void right_text(gfx_surface *s, gfx_ttf *ttf, float size, int right, int y, int maxw, const char *t, uint32_t c)
{
    int w = gfx_ttf_width(ttf, size, t);
    while (w > maxw && size > 8) {
        size -= 1;
        w = gfx_ttf_width(ttf, size, t);
    }
    gfx_ttf_text(s, ttf, size, right - w, y, t, c);
}

static void press(const char *l)
{
    if (!strcmp(l, "C")) { expr[0] = '\0'; strcpy(result, "0"); }
    else if (!strcmp(l, "<")) { size_t n = strlen(expr); if (n) expr[n - 1] = '\0'; }
    else if (!strcmp(l, "=")) evaluate();
    else add(l);
}

int main(void)
{
    gfx_surface *s;
    gfx_ttf *ttf;
    gfx_event ev;
    int running = 1, hover_r = -1, hover_c = -1;

    if (gfx_init_window("Calculator", 240, 330) < 0) {      /* ধাপ ১৯ঘ: ছোট শুরু */
        perror("gcalc: /dev/fb0");
        return 1;
    }
    s = gfx_screen();
    ttf = gfx_ttf_load("/usr/share/fonts/DejaVuSans.ttf");
    layout();

    while (running) {
        int r, c, mx, my, top = oy + disp_h + 2 * gap;
        float big = (float)disp_h * 0.42f, small = (float)disp_h * 0.22f, lab = (float)bh * 0.48f;
        gfx_gradient_v(s, 0, 0, gfx_width(), gfx_height(), GFX_RGB(40, 44, 60), GFX_RGB(16, 18, 26));
        gfx_fill_round_rect(s, ox, oy, pw, ph, gap + 4, GFX_RGB(32, 34, 44));
        gfx_round_rect(s, ox, oy, pw, ph, gap + 4, GFX_RGB(90, 95, 120));
        gfx_fill_round_rect(s, ox + gap, oy + gap, pw - 2 * gap, disp_h, gap, GFX_RGB(210, 225, 205));
        if (ttf) {
            int right = ox + pw - 2 * gap - 4, maxw = pw - 4 * gap - 8;
            right_text(s, ttf, small, right, oy + gap + disp_h / 10, maxw, expr, GFX_RGB(60, 70, 60));
            right_text(s, ttf, big, right, oy + gap + disp_h * 45 / 100, maxw, result, GFX_RGB(20, 30, 20));
        } else {
            gfx_text(s, NULL, ox + 2 * gap, oy + gap + 4, expr, GFX_BLACK);
            gfx_text(s, NULL, ox + 2 * gap, oy + gap + disp_h / 2, result, GFX_BLACK);
        }
        for (r = 0; r < 5; r++) {
            for (c = 0; c < 4; c++) {
                int x = ox + gap + c * (bw + gap), y = top + r * (bh + gap);
                uint32_t col = c == 3 ? GFX_RGB(230, 140, 40) : r == 0 ? GFX_RGB(90, 95, 110) : GFX_RGB(60, 64, 78);
                if (r == hover_r && c == hover_c) col = lighter(col);
                gfx_fill_round_rect(s, x, y, bw, bh, bh / 5, col);
                if (ttf) {
                    int w = gfx_ttf_width(ttf, lab, labels[r][c]);
                    gfx_ttf_text(s, ttf, lab, x + (bw - w) / 2, y + (bh - (int)lab) / 2 - 1, labels[r][c], GFX_WHITE);
                } else {
                    gfx_text(s, NULL, x + bw / 2 - 4, y + bh / 2 - 8, labels[r][c], GFX_WHITE);
                }
            }
        }
        if (hint) gfx_text(s, NULL, ox, oy + ph + 4, "keys: 0-9 . + - * / ( ) %  Enter  Backspace  c  q", GFX_RGB(160, 165, 180));
        gfx_mouse_pos(&mx, &my);
        gfx_cursor_draw(s, mx, my);
        gfx_present();

        if (!gfx_wait_event(&ev, -1)) continue;
        do {
            if (ev.type == GFX_EV_CLOSE) running = 0;           /* ধাপ ১৪ */
            if (ev.type == GFX_EV_RESIZE) layout();             /* ধাপ ১৯ঘ: সব নতুন মাপে */
            if (ev.type == GFX_EV_KEY) {
                int k = ev.key;
                char t[2] = { (char)k, 0 };
                if (k == 'q') running = 0;
                else if (k == GFX_KEY_ESC || k == 'c') press("C");
                else if (k == '\n' || k == '=') press("=");
                else if (k == '\b' || k == GFX_KEY_DELETE) press("<");
                else if (strchr("0123456789.+-*/()%", k) && k) add(t);
            } else if (ev.type == GFX_EV_MOUSE_MOVE || ev.type == GFX_EV_MOUSE_DOWN) {
                int cx = (ev.x - ox - gap), cy = (ev.y - (oy + disp_h + 2 * gap));
                hover_r = hover_c = -1;
                if (cx >= 0 && cy >= 0 && cx % (bw + gap) < bw && cy % (bh + gap) < bh &&
                    cx / (bw + gap) < 4 && cy / (bh + gap) < 5) {
                    hover_c = cx / (bw + gap);
                    hover_r = cy / (bh + gap);
                    if (ev.type == GFX_EV_MOUSE_DOWN && ev.button == GFX_BUTTON_LEFT) press(labels[hover_r][hover_c]);
                }
            }
        } while (gfx_poll_event(&ev, 0));
    }
    gfx_ttf_free(ttf);
    gfx_quit();
    return 0;
}
