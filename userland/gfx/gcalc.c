/* rozOS — gcalc: গ্রাফিক্যাল ক্যালকুলেটর (ধাপ ১৩)
 *   মাউসে বোতাম চাপা, বা কীবোর্ডে: 0-9 . + - * / ( ) %   Enter / = হিসাব
 *   Backspace একটা মোছা, Esc/c সব মোছা, q বের হওয়া
 *   হিসাব double-এ, বন্ধনী আর অগ্রাধিকার মেনে (recursive descent)। */

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
#define BW 90
#define BH 64
#define GAP 10

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
    int running = 1, ox, oy, pw, ph, hover_r = -1, hover_c = -1;

    if (gfx_init_window("Calculator", 440, 570) < 0) {
        perror("gcalc: /dev/fb0");
        return 1;
    }
    s = gfx_screen();
    ttf = gfx_ttf_load("/usr/share/fonts/DejaVuSans.ttf");
    pw = 4 * BW + 5 * GAP;
    ph = 130 + 5 * (BH + GAP) + GAP;
    ox = (gfx_width() - pw) / 2;
    oy = (gfx_height() - ph) / 2;

    while (running) {
        int r, c, mx, my;
        gfx_gradient_v(s, 0, 0, gfx_width(), gfx_height(), GFX_RGB(40, 44, 60), GFX_RGB(16, 18, 26));
        gfx_fill_round_rect(s, ox, oy, pw, ph, 16, GFX_RGB(32, 34, 44));
        gfx_round_rect(s, ox, oy, pw, ph, 16, GFX_RGB(90, 95, 120));
        gfx_fill_round_rect(s, ox + GAP, oy + GAP, pw - 2 * GAP, 110, 10, GFX_RGB(210, 225, 205));
        if (ttf) {
            int w = gfx_ttf_width(ttf, 22, expr);
            gfx_ttf_text(s, ttf, 22, ox + pw - 2 * GAP - 6 - w, oy + GAP + 10, expr, GFX_RGB(60, 70, 60));
            w = gfx_ttf_width(ttf, 44, result);
            gfx_ttf_text(s, ttf, 44, ox + pw - 2 * GAP - 6 - w, oy + GAP + 50, result, GFX_RGB(20, 30, 20));
        } else {
            gfx_text(s, NULL, ox + 20, oy + 30, expr, GFX_BLACK);
            gfx_text(s, NULL, ox + 20, oy + 70, result, GFX_BLACK);
        }
        for (r = 0; r < 5; r++) {
            for (c = 0; c < 4; c++) {
                int x = ox + GAP + c * (BW + GAP), y = oy + 130 + r * (BH + GAP);
                uint32_t col = c == 3 ? GFX_RGB(230, 140, 40) : r == 0 ? GFX_RGB(90, 95, 110) : GFX_RGB(60, 64, 78);
                if (r == hover_r && c == hover_c) col = GFX_RGB(GFX_R(col) + 25 > 255 ? 255 : GFX_R(col) + 25,
                                                                GFX_G(col) + 25 > 255 ? 255 : GFX_G(col) + 25,
                                                                GFX_B(col) + 25 > 255 ? 255 : GFX_B(col) + 25);
                gfx_fill_round_rect(s, x, y, BW, BH, 12, col);
                if (ttf) {
                    int w = gfx_ttf_width(ttf, 30, labels[r][c]);
                    gfx_ttf_text(s, ttf, 30, x + (BW - w) / 2, y + 14, labels[r][c], GFX_WHITE);
                } else {
                    gfx_text(s, NULL, x + BW / 2 - 4, y + BH / 2 - 8, labels[r][c], GFX_WHITE);
                }
            }
        }
        gfx_text(s, NULL, ox, oy + ph + 10, "keys: 0-9 . + - * / ( ) %  Enter  Backspace  c  q", GFX_RGB(160, 165, 180));
        gfx_mouse_pos(&mx, &my);
        gfx_cursor_draw(s, mx, my);
        gfx_present();

        if (!gfx_wait_event(&ev, -1)) continue;
        do {
            if (ev.type == GFX_EV_CLOSE) running = 0;           /* ধাপ ১৪ */
            if (ev.type == GFX_EV_RESIZE) {
                ox = (gfx_width() - pw) / 2;
                oy = (gfx_height() - ph) / 2;
            }
            if (ev.type == GFX_EV_KEY) {
                int k = ev.key;
                char t[2] = { (char)k, 0 };
                if (k == 'q') running = 0;
                else if (k == GFX_KEY_ESC || k == 'c') press("C");
                else if (k == '\n' || k == '=') press("=");
                else if (k == '\b' || k == GFX_KEY_DELETE) press("<");
                else if (strchr("0123456789.+-*/()%", k) && k) add(t);
            } else if (ev.type == GFX_EV_MOUSE_MOVE || ev.type == GFX_EV_MOUSE_DOWN) {
                int cx = (ev.x - ox - GAP), cy = (ev.y - oy - 130);
                hover_r = hover_c = -1;
                if (cx >= 0 && cy >= 0 && cx % (BW + GAP) < BW && cy % (BH + GAP) < BH &&
                    cx / (BW + GAP) < 4 && cy / (BH + GAP) < 5) {
                    hover_c = cx / (BW + GAP);
                    hover_r = cy / (BH + GAP);
                    if (ev.type == GFX_EV_MOUSE_DOWN && ev.button == GFX_BUTTON_LEFT) press(labels[hover_r][hover_c]);
                }
            }
        } while (gfx_poll_event(&ev, 0));
    }
    gfx_ttf_free(ttf);
    gfx_quit();
    return 0;
}
