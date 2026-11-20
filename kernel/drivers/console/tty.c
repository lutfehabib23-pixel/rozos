/* rozOS — TTY: অক্ষর-গ্রিড, কার্সর, স্ক্রল ও রঙ
 *
 * সব লেখা প্রথমে একটা স্ট্যাটিক অক্ষর-গ্রিডে যায়, তারপর বেছে নেওয়া ব্যাকএন্ডে
 * (ফ্রেমবাফার বা VGA) আঁকা হয়। প্রতিটি অক্ষর serial-এও পাঠানো হয়, যাতে স্ক্রিন না
 * থাকলেও লগ হারায় না। heap ছাড়াই কাজ করে (ধাপ ৩-এর আগেও)।
 *
 * ধাপ ৭: স্ক্রিনের জন্য ছোট ANSI/VT100 escape-পার্সার (shell ও এডিটরের জন্য) —
 * কার্সর সরানো (H f A B C D G), মোছা (J K), রঙ (m), কার্সর লুকানো (?25l/h)।
 * serial-এ বাইটগুলো যেমন আছে তেমনই যায়, তাই হোস্টের টার্মিনাল নিজেই বোঝে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/console.h>
#include <rozos/serial.h>
#include <rozos/string.h>

/* সর্বোচ্চ গ্রিড: 3840x2160 স্ক্রিনে 8x16 ফন্টে 480x135 */
#define TTY_MAX_COLS 480
#define TTY_MAX_ROWS 135
#define TTY_TAB_WIDTH 8

struct tty_cell {
    char    ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t reserved;
};

static struct tty_cell grid[TTY_MAX_COLS * TTY_MAX_ROWS];
static enum tty_backend backend = TTY_BACKEND_NONE;
static uint32_t n_cols, n_rows;
static uint32_t cur_col, cur_row;
static uint8_t  cur_fg = TTY_COLOR_LIGHT_GREY;
static uint8_t  cur_bg = TTY_COLOR_BLACK;
static int      cursor_hidden;            /* ধাপ ৭: ESC[?25l */
static int      reverse_video;            /* ধাপ ৭: ESC[7m */

/* ---------- ব্যাকএন্ড-নিরপেক্ষ আঁকা ---------- */

static void draw_cell(uint32_t col, uint32_t row)
{
    const struct tty_cell *c = &grid[row * n_cols + col];
    if (backend == TTY_BACKEND_FB) {
        fb_put_cell(col, row, c->ch, c->fg, c->bg);
    } else if (backend == TTY_BACKEND_VGA) {
        vga_put_cell(col, row, c->ch, c->fg, c->bg);
    }
}

static void draw_cursor(void)
{
    if (cursor_hidden) {
        return;
    }
    if (backend == TTY_BACKEND_FB) {
        fb_draw_cursor(cur_col, cur_row, cur_fg);
    } else if (backend == TTY_BACKEND_VGA) {
        vga_set_cursor(cur_col, cur_row);
    }
}

static void redraw_all(void)
{
    uint32_t r, c;
    for (r = 0; r < n_rows; r++) {
        for (c = 0; c < n_cols; c++) {
            draw_cell(c, r);
        }
    }
    draw_cursor();
}

static void clear_grid(void)
{
    uint32_t i;
    for (i = 0; i < n_cols * n_rows; i++) {
        grid[i].ch = ' ';
        grid[i].fg = cur_fg;
        grid[i].bg = cur_bg;
        grid[i].reserved = 0;
    }
    cur_col = 0;
    cur_row = 0;
}

static void use_backend(enum tty_backend b, uint32_t cols, uint32_t rows)
{
    backend = b;
    n_cols = (cols > TTY_MAX_COLS) ? TTY_MAX_COLS : cols;
    n_rows = (rows > TTY_MAX_ROWS) ? TTY_MAX_ROWS : rows;
    clear_grid();
    redraw_all();
}

/* ---------- পাবলিক ইন্টারফেস ---------- */

void tty_init(void)
{
    if (fb_is_ready()) {
        use_backend(TTY_BACKEND_FB, fb_cols(), fb_rows());
    } else if (vga_is_ready()) {
        use_backend(TTY_BACKEND_VGA, VGA_COLS, VGA_ROWS);
    } else {
        backend = TTY_BACKEND_NONE;
        n_cols = 0;
        n_rows = 0;
    }
}

void tty_attach_vga(volatile void *virt_base)
{
    /* ফ্রেমবাফার থাকলে সেটাই প্রাধান্য পায়; VGA শুধু বিকল্প */
    if (vga_init(virt_base) && backend == TTY_BACKEND_NONE) {
        use_backend(TTY_BACKEND_VGA, VGA_COLS, VGA_ROWS);
    }
}

enum tty_backend tty_backend(void) { return backend; }
uint32_t tty_cols(void) { return n_cols; }
uint32_t tty_rows(void) { return n_rows; }

const char *tty_backend_name(void)
{
    switch (backend) {
    case TTY_BACKEND_FB:  return "framebuffer";
    case TTY_BACKEND_VGA: return "VGA text mode";
    default:              return "serial only";
    }
}

static void scroll_up(void)
{
    uint32_t i;
    memmove(&grid[0], &grid[n_cols], (size_t)n_cols * (n_rows - 1) * sizeof(grid[0]));
    for (i = 0; i < n_cols; i++) {
        struct tty_cell *c = &grid[(n_rows - 1) * n_cols + i];
        c->ch = ' ';
        c->fg = cur_fg;
        c->bg = cur_bg;
    }
    redraw_all();
}

static void newline(void)
{
    cur_col = 0;
    if (++cur_row >= n_rows) {
        cur_row = n_rows - 1;
        scroll_up();
    }
}

static void screen_putc(char c)
{
    uint32_t old_col = cur_col, old_row = cur_row;
    struct tty_cell *cell;

    if (backend == TTY_BACKEND_NONE) {
        return;
    }

    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cur_col = 0;
        break;
    case '\t':
        do {
            screen_putc(' ');
        } while (cur_col % TTY_TAB_WIDTH);
        return;
    case '\b':
        if (cur_col > 0) {
            cur_col--;
        }
        break;
    default:
        cell = &grid[cur_row * n_cols + cur_col];
        cell->ch = c;
        cell->fg = cur_fg;
        cell->bg = cur_bg;
        draw_cell(cur_col, cur_row);
        if (++cur_col >= n_cols) {
            newline();
        }
        break;
    }

    /* পুরনো জায়গার কার্সর মুছে নতুন জায়গায় আঁকা */
    if ((old_col != cur_col || old_row != cur_row) && old_row < n_rows) {
        draw_cell(old_col, old_row);
    }
    draw_cursor();
}

/* ---------- ধাপ ৭: ANSI escape sequence ---------- */

#define ANSI_MAX_PARAMS 8

static enum { ANSI_NORMAL, ANSI_ESC, ANSI_CSI } ansi_state;
static int ansi_params[ANSI_MAX_PARAMS];
static int ansi_nparams;
static int ansi_private;                  /* '?' */

/* ANSI রঙের ক্রম (কালো লাল সবুজ হলুদ নীল বেগুনি সায়ান সাদা) → VGA-র ক্রম */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void move_cursor(uint32_t col, uint32_t row)
{
    uint32_t oc = cur_col, orow = cur_row;
    if (n_cols == 0 || n_rows == 0) {
        return;
    }
    cur_col = col < n_cols ? col : n_cols - 1;
    cur_row = row < n_rows ? row : n_rows - 1;
    if (orow < n_rows && oc < n_cols) {
        draw_cell(oc, orow);
    }
    draw_cursor();
}

static void clear_cells(uint32_t from, uint32_t to)   /* গ্রিডের সূচক, [from, to) */
{
    uint32_t i;
    for (i = from; i < to && i < n_cols * n_rows; i++) {
        grid[i].ch = ' ';
        grid[i].fg = cur_fg;
        grid[i].bg = cur_bg;
        draw_cell(i % n_cols, i / n_cols);
    }
    draw_cursor();
}

static int param(int i, int def)
{
    return (i < ansi_nparams && ansi_params[i] > 0) ? ansi_params[i] : def;
}

static void apply_sgr(void)
{
    int i;
    if (ansi_nparams == 0) {
        ansi_params[0] = 0;
        ansi_nparams = 1;
    }
    for (i = 0; i < ansi_nparams; i++) {
        int p = ansi_params[i];
        uint8_t t;
        if (p == 0) {
            if (reverse_video) {
                reverse_video = 0;
            }
            cur_fg = TTY_COLOR_LIGHT_GREY;
            cur_bg = TTY_COLOR_BLACK;
        } else if (p == 1) {
            cur_fg |= 0x08;                       /* উজ্জ্বল */
        } else if (p == 7 || p == 27) {
            if ((p == 7) != reverse_video) {
                t = cur_fg; cur_fg = cur_bg; cur_bg = t;
                reverse_video = (p == 7);
            }
        } else if (p >= 30 && p <= 37) {
            cur_fg = (uint8_t)((cur_fg & 0x08) | ansi_to_vga[p - 30]);
        } else if (p >= 90 && p <= 97) {
            cur_fg = (uint8_t)(0x08 | ansi_to_vga[p - 90]);
        } else if (p == 39) {
            cur_fg = TTY_COLOR_LIGHT_GREY;
        } else if (p >= 40 && p <= 47) {
            cur_bg = ansi_to_vga[p - 40];
        } else if (p >= 100 && p <= 107) {
            cur_bg = (uint8_t)(0x08 | ansi_to_vga[p - 100]);
        } else if (p == 49) {
            cur_bg = TTY_COLOR_BLACK;
        }
    }
}

static void csi_command(char c)
{
    uint32_t pos = cur_row * n_cols + cur_col;

    if (ansi_private) {
        if (param(0, 0) == 25 && (c == 'l' || c == 'h')) {
            cursor_hidden = (c == 'l');
            draw_cell(cur_col, cur_row);
            draw_cursor();
        }
        return;
    }
    switch (c) {
    case 'H':
    case 'f':
        move_cursor((uint32_t)param(1, 1) - 1, (uint32_t)param(0, 1) - 1);
        break;
    case 'A':
        move_cursor(cur_col, cur_row >= (uint32_t)param(0, 1) ? cur_row - (uint32_t)param(0, 1) : 0);
        break;
    case 'B':
        move_cursor(cur_col, cur_row + (uint32_t)param(0, 1));
        break;
    case 'C':
        move_cursor(cur_col + (uint32_t)param(0, 1), cur_row);
        break;
    case 'D':
        move_cursor(cur_col >= (uint32_t)param(0, 1) ? cur_col - (uint32_t)param(0, 1) : 0, cur_row);
        break;
    case 'G':
        move_cursor((uint32_t)param(0, 1) - 1, cur_row);
        break;
    case 'J':
        if (ansi_nparams && ansi_params[0] == 2) {
            clear_cells(0, n_cols * n_rows);
        } else if (ansi_nparams && ansi_params[0] == 1) {
            clear_cells(0, pos + 1);
        } else {
            clear_cells(pos, n_cols * n_rows);
        }
        break;
    case 'K':
        if (ansi_nparams && ansi_params[0] == 2) {
            clear_cells(cur_row * n_cols, (cur_row + 1) * n_cols);
        } else if (ansi_nparams && ansi_params[0] == 1) {
            clear_cells(cur_row * n_cols, pos + 1);
        } else {
            clear_cells(pos, (cur_row + 1) * n_cols);
        }
        break;
    case 'm':
        apply_sgr();
        break;
    default:
        break;                                    /* অজানা — উপেক্ষা */
    }
}

static void ansi_feed(char c)
{
    switch (ansi_state) {
    case ANSI_NORMAL:
        if (c == 0x1B) {
            ansi_state = ANSI_ESC;
        } else {
            screen_putc(c);
        }
        break;
    case ANSI_ESC:
        if (c == '[') {
            ansi_state = ANSI_CSI;
            ansi_nparams = 0;
            ansi_private = 0;
            memset(ansi_params, 0, sizeof(ansi_params));
        } else {
            ansi_state = ANSI_NORMAL;             /* ESC + অন্য কিছু: উপেক্ষা */
        }
        break;
    case ANSI_CSI:
        if (c == '?') {
            ansi_private = 1;
        } else if (c >= '0' && c <= '9') {
            if (ansi_nparams == 0) {
                ansi_nparams = 1;
            }
            if (ansi_params[ansi_nparams - 1] < 10000) {
                ansi_params[ansi_nparams - 1] = ansi_params[ansi_nparams - 1] * 10 + (c - '0');
            }
        } else if (c == ';') {
            if (ansi_nparams == 0) {
                ansi_nparams = 1;
            }
            if (ansi_nparams < ANSI_MAX_PARAMS) {
                ansi_nparams++;
            }
        } else {
            ansi_state = ANSI_NORMAL;
            if (backend != TTY_BACKEND_NONE) {
                csi_command(c);
            }
        }
        break;
    }
}

void tty_putc(char c)
{
    serial_putc(c);
    ansi_feed(c);
}

void tty_write(const char *s, size_t n)
{
    while (n--) {
        tty_putc(*s++);
    }
}

void tty_set_color(uint8_t fg, uint8_t bg)
{
    cur_fg = fg & 0x0F;
    cur_bg = bg & 0x0F;
}

void tty_clear(void)
{
    if (backend == TTY_BACKEND_NONE) {
        return;
    }
    clear_grid();
    redraw_all();
}
