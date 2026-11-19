/* rozOS — TTY: অক্ষর-গ্রিড, কার্সর, স্ক্রল ও রঙ
 *
 * সব লেখা প্রথমে একটা স্ট্যাটিক অক্ষর-গ্রিডে যায়, তারপর বেছে নেওয়া ব্যাকএন্ডে
 * (ফ্রেমবাফার বা VGA) আঁকা হয়। প্রতিটি অক্ষর serial-এও পাঠানো হয়, যাতে স্ক্রিন না
 * থাকলেও লগ হারায় না। heap ছাড়াই কাজ করে (ধাপ ৩-এর আগেও)। */

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

void tty_putc(char c)
{
    serial_putc(c);
    screen_putc(c);
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
