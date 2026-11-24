/* rozOS — rozterm: টার্মিনাল উইন্ডো (ধাপ ১৪)
 *
 *   rozterm                 উইন্ডোতে shell (rozsh -l)
 *   rozterm -e 'COMMAND'    COMMAND চালায় (sh -c); শেষ হলে উইন্ডো বন্ধ
 *   rozterm -w -e 'COMMAND' শেষ হলে "press any key" — তারপর বন্ধ (মেনুর wait: yes)
 *
 * ভেতরে একটা PTY (openpty): shell আর তার প্রোগ্রামগুলো ভাবে তারা কনসোলে — rozterm তাদের
 * লেখা (ANSI/VT100: কার্সর, রং, পর্দা মোছা, alternate screen, মাউস-রিপোর্ট) নিজের অক্ষর-
 * গ্রিডে আঁকে, আর উইন্ডোর কী/মাউস PTY-তে পাঠায়। কার্নেলের কনসোলের (tty.c) মতোই আচরণ,
 * তাই edit, help, menu, kmon হুবহু চলে।
 *
 * স্ক্রল: মাউসের চাকা বা Shift+PgUp/PgDn দিয়ে আগের ১০০০ লাইন। কপি: মাউসে টেনে নির্বাচন
 * (ছাড়লেই কপি), ডান-ক্লিকে পেস্ট — ক্লিপবোর্ড /tmp/.clipboard (সব টার্মিনাল ভাগ করে)।
 * Ctrl+C: চলমান প্রোগ্রাম থামে (PTY → SIGINT)। উইন্ডোর মাপ বদলালে কলাম/সারিও বদলায়
 * (প্রোগ্রাম নতুন মাপ জানে TIOCGWINSZ-এ; এডিটরে Ctrl+L)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <pty.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <rozgfx.h>
#include <rozui.h>

#define PAD       4
#define MAXCOLS   240
#define MAXROWS   100
#define HISTORY   1000
#define CLIPFILE  "/tmp/.clipboard"

struct cell {
    unsigned char ch, fg, bg, pad;
};

static uint32_t palette[16] = {
    GFX_RGB(11, 18, 21),   GFX_RGB(60, 110, 200),   /* ধাপ ১৪খ: পটভূমি Obsidian #0B1215 */ GFX_RGB(110, 170, 90),  GFX_RGB(70, 170, 180),
    GFX_RGB(200, 80, 80),  GFX_RGB(170, 100, 190), GFX_RGB(190, 140, 70), GFX_RGB(200, 204, 212),
    GFX_RGB(95, 100, 112), GFX_RGB(97, 175, 239), GFX_RGB(152, 195, 121), GFX_RGB(86, 182, 194),
    GFX_RGB(224, 108, 117), GFX_RGB(198, 120, 221), GFX_RGB(229, 192, 123), GFX_RGB(240, 242, 246)
};
static const unsigned char ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static gfx_surface *S;
static gfx_font *font;
static int cw, ch;                      /* ঘরের মাপ */
static int cols, rows;
static struct cell grid[MAXROWS][MAXCOLS], alt_grid[MAXROWS][MAXCOLS];
static int alt_active;
static struct cell hist[HISTORY][MAXCOLS];
static int hist_cols[HISTORY];
static int hist_n, hist_head;           /* ঘুরন্ত: hist_head = সবচেয়ে পুরোনো */
static int view;                        /* কত লাইন ওপরে স্ক্রল করা (0 = নিচে) */
static int cx, cy, saved_cx, saved_cy, alt_cx, alt_cy;
static unsigned char cur_fg = 7, cur_bg = 0;
static int reverse, cursor_hidden;
static int mouse_mode;                  /* 1 ক্লিক, 2 টানা, 4 সব নড়া, 8 SGR */
static unsigned char dirty[MAXROWS];
static int master = -1;
static pid_t child;
static int child_done, wait_key, exit_status;
static int focused = 1;
static int blink_on = 1;                /* ধাপ ১৪খ: কার্সর ঝলকানো (৫০০ ms) */
static long blink_at;

/* নির্বাচন (স্ক্রিনের সারি/কলাম, view সহ যা দেখা যাচ্ছে) */
static int sel_on, sel_active, sel_x0, sel_y0, sel_x1, sel_y1;

/* ---------- PTY-তে লেখা ---------- */
static void pty_write(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(master, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void pty_puts(const char *s) { pty_write(s, strlen(s)); }

/* ---------- গ্রিড ---------- */
static void mark_all(void) { memset(dirty, 1, sizeof(dirty)); }

static void clear_cells(int row, int x0, int x1)
{
    int x;
    if (row < 0 || row >= rows) return;
    for (x = x0; x < x1 && x < cols; x++) {
        grid[row][x].ch = ' ';
        grid[row][x].fg = cur_fg;
        grid[row][x].bg = cur_bg;
    }
    dirty[row] = 1;
}

static void push_history(int row)
{
    int slot;
    if (alt_active) return;
    if (hist_n < HISTORY) slot = (hist_head + hist_n++) % HISTORY;
    else { slot = hist_head; hist_head = (hist_head + 1) % HISTORY; }
    memcpy(hist[slot], grid[row], sizeof(grid[row]));
    hist_cols[slot] = cols;
}

static void scroll_up(void)
{
    int r;
    push_history(0);
    for (r = 0; r < rows - 1; r++) memcpy(grid[r], grid[r + 1], sizeof(grid[r]));
    clear_cells(rows - 1, 0, cols);
    mark_all();
    if (view > 0 && view < hist_n) view++;        /* দেখা অংশ স্থির থাকুক */
}

static void newline(void)
{
    cx = 0;
    if (++cy >= rows) {
        cy = rows - 1;
        scroll_up();
    }
}

static void put_char(unsigned char c)
{
    if (cx >= cols) newline();
    grid[cy][cx].ch = c;
    grid[cy][cx].fg = cur_fg;
    grid[cy][cx].bg = cur_bg;
    dirty[cy] = 1;
    if (++cx >= cols) newline();                  /* কার্নেলের কনসোলের মতো সাথে সাথে মোড়া */
}

static void set_alt(int on)
{
    if (on == alt_active) return;
    if (on) {
        memcpy(alt_grid, grid, sizeof(grid));
        alt_cx = cx; alt_cy = cy;
        alt_active = 1;
        {
            int r;
            for (r = 0; r < rows; r++) clear_cells(r, 0, cols);
        }
        cx = cy = 0;
    } else {
        memcpy(grid, alt_grid, sizeof(grid));
        cx = alt_cx; cy = alt_cy;
        alt_active = 0;
    }
    view = 0;
    mark_all();
}

/* ---------- ANSI পার্সার ---------- */
static int esc_state;                   /* 0 সাধারণ, 1 ESC পাওয়া, 2 CSI */
static int params[16], nparams, priv;

static int param(int i, int def) { return (i < nparams && params[i] > 0) ? params[i] : def; }

static void sgr(void)
{
    int i;
    if (nparams == 0) { nparams = 1; params[0] = 0; }
    for (i = 0; i < nparams; i++) {
        int p = params[i];
        unsigned char t;
        if (p == 0) {
            if (reverse) { t = cur_fg; cur_fg = cur_bg; cur_bg = t; reverse = 0; }
            cur_fg = 7; cur_bg = 0;
        } else if (p == 1) cur_fg |= 8;
        else if (p == 22) cur_fg &= 7;
        else if (p == 7 || p == 27) {
            if ((p == 7) != reverse) { t = cur_fg; cur_fg = cur_bg; cur_bg = t; reverse = p == 7; }
        } else if (p >= 30 && p <= 37) cur_fg = (unsigned char)((cur_fg & 8) | ansi_to_vga[p - 30]);
        else if (p >= 90 && p <= 97) cur_fg = (unsigned char)(8 | ansi_to_vga[p - 90]);
        else if (p == 39) cur_fg = 7;
        else if (p >= 40 && p <= 47) cur_bg = ansi_to_vga[p - 40];
        else if (p >= 100 && p <= 107) cur_bg = (unsigned char)(8 | ansi_to_vga[p - 100]);
        else if (p == 49) cur_bg = 0;
    }
}

static void csi(char c)
{
    int i;
    dirty[cy] = 1;
    if (priv) {
        for (i = 0; i < (nparams ? nparams : 1); i++) {
            int p = nparams ? params[i] : 0, on = c == 'h';
            if (c != 'h' && c != 'l') break;
            if (p == 25) cursor_hidden = !on;
            else if (p == 1049 || p == 1047 || p == 47) {
                if (p == 1049 && on) { saved_cx = cx; saved_cy = cy; }
                set_alt(on);
                if (p == 1049 && !on) { cx = saved_cx; cy = saved_cy; }
            }
            else if (p == 1000) mouse_mode = on ? mouse_mode | 1 : mouse_mode & ~1;
            else if (p == 1002) mouse_mode = on ? mouse_mode | 2 : mouse_mode & ~2;
            else if (p == 1003) mouse_mode = on ? mouse_mode | 4 : mouse_mode & ~4;
            else if (p == 1006) mouse_mode = on ? mouse_mode | 8 : mouse_mode & ~8;
        }
        return;
    }
    switch (c) {
    case 'H': case 'f':
        cy = param(0, 1) - 1; cx = param(1, 1) - 1; break;
    case 'A': cy -= param(0, 1); break;
    case 'B': cy += param(0, 1); break;
    case 'C': cx += param(0, 1); break;
    case 'D': cx -= param(0, 1); break;
    case 'G': cx = param(0, 1) - 1; break;
    case 'd': cy = param(0, 1) - 1; break;
    case 'J': {
        int mode = nparams ? params[0] : 0, r;
        if (mode == 2 || mode == 3) for (r = 0; r < rows; r++) clear_cells(r, 0, cols);
        else if (mode == 1) { for (r = 0; r < cy; r++) clear_cells(r, 0, cols); clear_cells(cy, 0, cx + 1); }
        else { clear_cells(cy, cx, cols); for (r = cy + 1; r < rows; r++) clear_cells(r, 0, cols); }
        break;
    }
    case 'K': {
        int mode = nparams ? params[0] : 0;
        if (mode == 2) clear_cells(cy, 0, cols);
        else if (mode == 1) clear_cells(cy, 0, cx + 1);
        else clear_cells(cy, cx, cols);
        break;
    }
    case 'm': sgr(); break;
    case 's': saved_cx = cx; saved_cy = cy; break;
    case 'u': cx = saved_cx; cy = saved_cy; break;
    case 'n':
        if (param(0, 0) == 6) {                   /* কার্সর কোথায়? */
            char b[32];
            snprintf(b, sizeof(b), "\x1b[%d;%dR", cy + 1, cx + 1);
            pty_puts(b);
        }
        break;
    }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= cols) cx = cols - 1;
    if (cy >= rows) cy = rows - 1;
    dirty[cy] = 1;
}

static void feed(unsigned char c)
{
    if (esc_state == 1) {
        if (c == '[') { esc_state = 2; nparams = 0; priv = 0; memset(params, 0, sizeof(params)); return; }
        if (c == '7') { saved_cx = cx; saved_cy = cy; }
        else if (c == '8') { cx = saved_cx; cy = saved_cy; dirty[cy] = 1; }
        esc_state = 0;
        return;
    }
    if (esc_state == 2) {
        if (c == '?') { priv = 1; return; }
        if (c >= '0' && c <= '9') {
            if (nparams == 0) nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (nparams == 0) nparams = 1;
            if (nparams < 16) nparams++;
            return;
        }
        esc_state = 0;
        csi((char)c);
        return;
    }
    switch (c) {
    case 0x1b: esc_state = 1; break;
    case '\n': dirty[cy] = 1; newline(); break;
    case '\r': dirty[cy] = 1; cx = 0; break;
    case '\b': if (cx > 0) cx--; dirty[cy] = 1; break;
    case '\t': do put_char(' '); while (cx % 8 && cx < cols); break;
    case '\a': case 0: break;
    default:
        if (c >= 32) put_char(c);
        break;
    }
}

/* ---------- আঁকা ---------- */

/* পর্দার সারি r-এ কোন লাইন দেখাবো (view অনুযায়ী): ইতিহাস বা গ্রিড */
static const struct cell *line_at(int r)
{
    int idx = r - view;
    if (idx >= 0) return grid[idx];
    idx = hist_n + idx;                           /* ঋণাত্মক → ইতিহাসের শেষ থেকে */
    if (idx < 0) return NULL;
    return hist[(hist_head + idx) % HISTORY];
}

static int in_sel(int x, int y)
{
    int a = sel_y0 * MAXCOLS + sel_x0, b = sel_y1 * MAXCOLS + sel_x1, p = y * MAXCOLS + x, t;
    if (!sel_on) return 0;
    if (a > b) { t = a; a = b; b = t; }
    return p >= a && p <= b;
}

static void draw_row(int r)
{
    const struct cell *ln = line_at(r);
    int x, y = PAD + r * ch;
    for (x = 0; x < cols; x++) {
        unsigned char c = ln ? ln[x].ch : ' ', fg = ln ? ln[x].fg : 7, bg = ln ? ln[x].bg : 0;
        uint32_t f = palette[fg & 15], b = palette[bg & 15];
        if (c == 0) c = ' ';
        if (in_sel(x, r)) { f = GFX_WHITE; b = GFX_RGB(85, 93, 80); }      /* Ebony */
        gfx_glyph(S, font, PAD + x * cw, y, c, f, b);
    }
    /* কার্সর: সামনে থাকলে ঝলকানো সাদা ভরা ঘর, পেছনে থাকলে ফাঁপা বাক্স */
    if (r == cy + view && view >= 0 && cy + view < rows && !cursor_hidden && !child_done) {
        if (focused) {
            if (blink_on) {
                unsigned char c = grid[cy][cx < cols ? cx : cols - 1].ch;
                gfx_glyph(S, font, PAD + cx * cw, y, c ? c : ' ', palette[0], GFX_WHITE);
            }
        } else {
            gfx_rect(S, PAD + cx * cw, y, cw, ch, palette[7]);
        }
    }
}

static void render(void)
{
    int r, y0 = -1, y1 = -1;
    for (r = 0; r < rows; r++) {
        if (!dirty[r]) continue;
        draw_row(r);
        dirty[r] = 0;
        if (y0 < 0) y0 = r;
        y1 = r;
    }
    if (y0 >= 0) gfx_present_rect(0, PAD + y0 * ch, gfx_width(), (y1 - y0 + 1) * ch);
}

static void full_redraw(void)
{
    gfx_fill_rect(S, 0, 0, gfx_width(), gfx_height(), palette[0]);
    mark_all();
    render();
    gfx_present();
}

/* ---------- মাপ ---------- */
static void resize_to_window(void)
{
    struct winsize ws;
    int nc = (gfx_width() - 2 * PAD) / cw, nr = (gfx_height() - 2 * PAD) / ch, r, x;
    if (nc < 10) nc = 10;
    if (nr < 3) nr = 3;
    if (nc > MAXCOLS) nc = MAXCOLS;
    if (nr > MAXROWS) nr = MAXROWS;
    /* নতুন কলাম ফাঁকা দিয়ে ভরা; সারি কমলে ওপরের লাইন ইতিহাসে */
    for (r = 0; r < MAXROWS; r++)
        for (x = (r < rows ? cols : 0); x < MAXCOLS; x++) {
            grid[r][x].ch = ' '; grid[r][x].fg = 7; grid[r][x].bg = 0;
        }
    while (cy >= nr) {
        int k;
        push_history(0);
        for (k = 0; k < rows - 1; k++) memcpy(grid[k], grid[k + 1], sizeof(grid[k]));
        cy--;
    }
    cols = nc;
    rows = nr;
    if (cx >= cols) cx = cols - 1;
    view = 0;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = ws.ws_ypixel = 0;
    if (master >= 0) ioctl(master, TIOCSWINSZ, &ws);
    full_redraw();
}

/* ---------- কী → PTY ---------- */
static void send_key(int key, int mods)
{
    char b[16];
    const char *seq = NULL;
    int m = mods ? mods + 1 : 0;
    if (view && !(mods & GFX_MOD_SHIFT)) { view = 0; mark_all(); }
    if ((mods & GFX_MOD_SHIFT) && (key == GFX_KEY_PAGE_UP || key == GFX_KEY_PAGE_DOWN)) {
        view += key == GFX_KEY_PAGE_UP ? rows - 1 : -(rows - 1);
        if (view > hist_n) view = hist_n;
        if (view < 0) view = 0;
        mark_all();
        return;
    }
    if (key < 0x100) {
        char c = (char)key;
        pty_write(&c, 1);
        return;
    }
    switch (key) {
    case GFX_KEY_UP:    seq = "A"; break;
    case GFX_KEY_DOWN:  seq = "B"; break;
    case GFX_KEY_RIGHT: seq = "C"; break;
    case GFX_KEY_LEFT:  seq = "D"; break;
    case GFX_KEY_HOME:  seq = "H"; break;
    case GFX_KEY_END:   seq = "F"; break;
    }
    if (seq) {
        if (m) snprintf(b, sizeof(b), "\x1b[1;%d%s", m, seq);
        else snprintf(b, sizeof(b), "\x1b[%s", seq);
        pty_puts(b);
        return;
    }
    {
        int n = 0;
        switch (key) {
        case GFX_KEY_INSERT: n = 2; break;
        case GFX_KEY_DELETE: n = 3; break;
        case GFX_KEY_PAGE_UP: n = 5; break;
        case GFX_KEY_PAGE_DOWN: n = 6; break;
        case GFX_KEY_F5: n = 15; break;
        case GFX_KEY_F6: n = 17; break;
        case GFX_KEY_F7: n = 18; break;
        case GFX_KEY_F8: n = 19; break;
        case GFX_KEY_F9: n = 20; break;
        case GFX_KEY_F10: n = 21; break;
        case GFX_KEY_F11: n = 23; break;
        case GFX_KEY_F12: n = 24; break;
        }
        if (n) {
            if (m) snprintf(b, sizeof(b), "\x1b[%d;%d~", n, m);
            else snprintf(b, sizeof(b), "\x1b[%d~", n);
            pty_puts(b);
            return;
        }
        if (key >= GFX_KEY_F1 && key <= GFX_KEY_F4) {
            /* ধাপ ১৯ক: Shift/Ctrl/Alt থাকলে xterm-এর মতো ESC[1;<m>P (edit-এর Ctrl+F1) */
            if (m) snprintf(b, sizeof(b), "\x1b[1;%d%c", m, 'P' + (key - GFX_KEY_F1));
            else snprintf(b, sizeof(b), "\x1bO%c", 'P' + (key - GFX_KEY_F1));
            pty_puts(b);
        }
    }
}

/* ---------- মাউস ---------- */
static void mouse_report(int b, int x, int y, int press)
{
    char s[40];
    if (!(mouse_mode & 8)) return;                 /* শুধু SGR ভাষা (rozOS-এর প্রোগ্রাম এটাই চায়) */
    snprintf(s, sizeof(s), "\x1b[<%d;%d;%d%c", b, x + 1, y + 1, press ? 'M' : 'm');
    pty_puts(s);
}

static void copy_selection(void)
{
    FILE *f;
    int a, b, t, p;
    if (!sel_on) return;
    a = sel_y0 * MAXCOLS + sel_x0;
    b = sel_y1 * MAXCOLS + sel_x1;
    if (a > b) { t = a; a = b; b = t; }
    f = fopen(CLIPFILE, "w");
    if (!f) return;
    for (p = a; p <= b; p++) {
        int x = p % MAXCOLS, y = p / MAXCOLS;
        const struct cell *ln;
        if (x >= cols) continue;
        ln = line_at(y);
        fputc(ln && ln[x].ch ? ln[x].ch : ' ', f);
        if (x == cols - 1 && p != b) {
            fputc('\n', f);
            p = (y + 1) * MAXCOLS - 1;
        }
    }
    fclose(f);
}

static void paste(void)
{
    FILE *f = fopen(CLIPFILE, "r");
    char b[256];
    size_t n;
    if (!f) return;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) pty_write(b, n);
    fclose(f);
}

static void handle_mouse(const gfx_event *ev)
{
    int x = (ev->x - PAD) / cw, y = (ev->y - PAD) / ch;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cols) x = cols - 1;
    if (y >= rows) y = rows - 1;
    if (ev->type == GFX_EV_MOUSE_WHEEL) {
        if (mouse_mode && view == 0) {
            mouse_report(ev->wheel < 0 ? 64 : 65, x, y, 1);
            return;
        }
        if (!alt_active) {
            view += ev->wheel < 0 ? 3 : -3;
            if (view > hist_n) view = hist_n;
            if (view < 0) view = 0;
            mark_all();
        }
        return;
    }
    if (mouse_mode && view == 0) {                 /* প্রোগ্রাম নিজে মাউস চায় (edit, help) */
        int b = ev->button == GFX_BUTTON_RIGHT ? 2 : ev->button == GFX_BUTTON_MIDDLE ? 1 : 0;
        if (ev->type == GFX_EV_MOUSE_DOWN) mouse_report(b, x, y, 1);
        else if (ev->type == GFX_EV_MOUSE_UP) mouse_report(b, x, y, 0);
        else if (ev->type == GFX_EV_MOUSE_MOVE && (ev->buttons & 1) && (mouse_mode & 6))
            mouse_report(32, x, y, 1);
        return;
    }
    /* নিজের নির্বাচন */
    if (ev->type == GFX_EV_MOUSE_DOWN && ev->button == GFX_BUTTON_LEFT) {
        sel_on = 0;
        sel_active = 1;
        sel_x0 = sel_x1 = x;
        sel_y0 = sel_y1 = y;
        mark_all();
    } else if (ev->type == GFX_EV_MOUSE_MOVE && sel_active) {
        if (x != sel_x1 || y != sel_y1) {
            sel_x1 = x; sel_y1 = y;
            sel_on = 1;
            mark_all();
        }
    } else if (ev->type == GFX_EV_MOUSE_UP && ev->button == GFX_BUTTON_LEFT) {
        sel_active = 0;
        if (sel_on) copy_selection();
    } else if (ev->type == GFX_EV_MOUSE_DOWN && ev->button == GFX_BUTTON_RIGHT) {
        if (sel_on) { sel_on = 0; mark_all(); }
        paste();
    }
}

/* ---------- শুরু ---------- */
static void start_child(const char *cmd)
{
    int slave;
    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = ws.ws_ypixel = 0;
    if (openpty(&master, &slave, NULL, NULL, &ws) < 0) {
        perror("rozterm: openpty");
        exit(1);
    }
    child = fork();
    if (child == 0) {
        int fd;
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        for (fd = 3; fd < 64; fd++) close(fd);
        setenv("TERM", "rozos", 1);
        signal(SIGINT, SIG_DFL);
        if (cmd) execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        else execl("/bin/rozsh", "rozsh", "-l", (char *)NULL);
        _exit(127);
    }
    close(slave);
}

int main(int argc, char **argv)
{
    const char *cmd = NULL, *title = "Terminal";
    char tbuf[64];
    int i, running = 1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) wait_key = 1;
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) cmd = argv[++i];
    }
    font = gfx_font_mono();                      /* ধাপ ২৩ক: JetBrains Mono (না থাকলে PSF) */
    if (!font) {
        fprintf(stderr, "rozterm: no font (/usr/share/fonts/rozos-mono-16.psf)\n");
        return 1;
    }
    cw = gfx_font_width(font);
    ch = gfx_font_height(font);
    if (cmd) {
        snprintf(tbuf, sizeof(tbuf), "%.56s", cmd);
        title = tbuf;
    }
    if (gfx_init_window(title, 92 * cw + 2 * PAD, 28 * ch + 2 * PAD) < 0) {
        perror("rozterm: no screen");
        return 1;
    }
    if (!gfx_windowed()) {
        gfx_quit();
        fprintf(stderr, "rozterm: needs the desktop (rozwm) - run 'desktop'\n");
        return 1;
    }
    S = gfx_screen();
    cols = (gfx_width() - 2 * PAD) / cw;
    rows = (gfx_height() - 2 * PAD) / ch;
    if (cols > MAXCOLS) cols = MAXCOLS;
    if (rows > MAXROWS) rows = MAXROWS;
    {
        int r, x;
        for (r = 0; r < MAXROWS; r++) for (x = 0; x < MAXCOLS; x++) { grid[r][x].ch = ' '; grid[r][x].fg = 7; grid[r][x].bg = 0; }
    }
    signal(SIGPIPE, SIG_IGN);
    start_child(cmd);
    full_redraw();

    while (running) {
        struct pollfd p[4];
        int np = 0, evfd[2], nev = gfx_event_fds(evfd, 2), k;
        gfx_event ev;
        for (k = 0; k < nev; k++) { p[np].fd = evfd[k]; p[np].events = POLLIN; p[np].revents = 0; np++; }
        if (!child_done) { p[np].fd = master; p[np].events = POLLIN; p[np].revents = 0; np++; }
        /* ধাপ ১৪খ: সামনে থাকলে ৫০০ ms পরপর জেগে কার্সর ঝলকানো */
        {
            int tmo = -1;
            if (focused && !child_done && !cursor_hidden) {
                tmo = 500 - (int)(gfx_ticks_ms() - blink_at);
                if (tmo < 0) tmo = 0;
            }
            if (poll(p, (unsigned long)np, tmo) == 0 && tmo >= 0) {
                blink_on = !blink_on;
                blink_at = gfx_ticks_ms();
                if (cy + view < rows) dirty[cy + view] = 1;
            }
        }

        if (!child_done && (p[np - 1].revents & (POLLIN | POLLHUP))) {
            unsigned char buf[4096];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                ssize_t j;
                if (view) { view = 0; mark_all(); }
                for (j = 0; j < n; j++) feed(buf[j]);
            } else {
                int st = 0;
                child_done = 1;
                waitpid(child, &st, 0);
                exit_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
                if (!wait_key) running = 0;
                else {
                    const char *msg = "\r\n[finished - press any key to close]";
                    while (*msg) feed((unsigned char)*msg++);
                    gfx_set_title("finished - press a key");
                }
            }
        }
        while (gfx_poll_event(&ev, 0)) {
            switch (ev.type) {
            case GFX_EV_CLOSE:
                running = 0;
                break;
            case GFX_EV_KEY:
                if (child_done) { running = 0; break; }
                blink_on = 1;                            /* টাইপের সময় কার্সর সবসময় দেখা যায় */
                blink_at = gfx_ticks_ms();
                if (sel_on) { sel_on = 0; mark_all(); }
                send_key(ev.key, ev.mods);
                break;
            case GFX_EV_RESIZE:
                resize_to_window();
                break;
            case GFX_EV_FOCUS:
                focused = ev.key;
                dirty[cy] = 1;
                break;
            default:
                if (child_done && ev.type == GFX_EV_MOUSE_DOWN) { running = 0; break; }
                handle_mouse(&ev);
                break;
            }
        }
        render();
    }
    if (!child_done && child > 0) {
        kill(-child, SIGHUP);                     /* ভেতরের shell আর তার প্রোগ্রাম */
        kill(child, SIGHUP);
    }
    gfx_quit();
    return exit_status;
}
