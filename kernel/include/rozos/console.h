/* rozOS — কনসোল সাবসিস্টেম
 *
 *  font.c : ফন্ট (এখন Limine-এর দেওয়া VGA-ধাঁচের 8xN বিটম্যাপ ফন্ট)
 *  fb.c   : ফ্রেমবাফার ব্যাকএন্ড — পিক্সেলে অক্ষর আঁকে (BIOS ও UEFI দুটোতেই)
 *  vga.c  : VGA টেক্সট-মোড ব্যাকএন্ড (80x25, ফিজিক্যাল 0xB8000)
 *  tty.c  : অক্ষরের গ্রিড, কার্সর, স্ক্রল, রঙ; ব্যাকএন্ড বেছে নেয় এবং সব আউটপুট
 *           serial-এও পাঠায়।
 *
 * VGA সম্পর্কে নোট: base revision 3+ এ HHDM শুধু usable/bootloader/kernel/framebuffer
 * অঞ্চল ম্যাপ করে, তাই 0xB8000 (Reserved) Limine-এর পেজ টেবিলে নেই। VGA ব্যাকএন্ড
 * চালু হবে যখন ধাপ ৩-এর VMM 0xB8000 ম্যাপ করে tty_attach_vga() ডাকবে। ততক্ষণ
 * ফ্রেমবাফার না পেলে আউটপুট শুধু serial-এ যায়। */
#ifndef ROZOS_CONSOLE_H
#define ROZOS_CONSOLE_H

#include <stdint.h>
#include <stddef.h>

/* ---------- রঙ (স্ট্যান্ডার্ড VGA 16-রঙের ক্রম) ---------- */
enum tty_color {
    TTY_COLOR_BLACK = 0,
    TTY_COLOR_BLUE,
    TTY_COLOR_GREEN,
    TTY_COLOR_CYAN,
    TTY_COLOR_RED,
    TTY_COLOR_MAGENTA,
    TTY_COLOR_BROWN,
    TTY_COLOR_LIGHT_GREY,
    TTY_COLOR_DARK_GREY,
    TTY_COLOR_LIGHT_BLUE,
    TTY_COLOR_LIGHT_GREEN,
    TTY_COLOR_LIGHT_CYAN,
    TTY_COLOR_LIGHT_RED,
    TTY_COLOR_LIGHT_MAGENTA,
    TTY_COLOR_YELLOW,
    TTY_COLOR_WHITE
};

/* ---------- ফন্ট ---------- */
struct rozos_font {
    const uint8_t *glyphs;   /* 256টি গ্লিফ, প্রতিটি height বাইট (প্রতি লাইনে 1 বাইট) */
    uint32_t width;          /* সবসময় 8 */
    uint32_t height;
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t spacing;        /* অক্ষরের মাঝে বাড়তি অনুভূমিক পিক্সেল */
};

int  font_init(void);
const struct rozos_font *font_get(void);

/* ---------- ফ্রেমবাফার ব্যাকএন্ড ---------- */
int      fb_init(void);
int      fb_is_ready(void);
uint32_t fb_cols(void);
uint32_t fb_rows(void);
uint64_t fb_width(void);
uint64_t fb_height(void);
uint16_t fb_bpp(void);
void     fb_put_cell(uint32_t col, uint32_t row, char ch, uint8_t fg, uint8_t bg);
void     fb_draw_cursor(uint32_t col, uint32_t row, uint8_t color);
void     fb_clear(uint8_t bg);

/* ---------- VGA টেক্সট-মোড ব্যাকএন্ড ---------- */
#define VGA_TEXT_PHYS 0xB8000
#define VGA_COLS      80
#define VGA_ROWS      25

int  vga_init(volatile void *virt_base);
int  vga_is_ready(void);
void vga_put_cell(uint32_t col, uint32_t row, char ch, uint8_t fg, uint8_t bg);
void vga_set_cursor(uint32_t col, uint32_t row);
void vga_clear(uint8_t bg);

/* ---------- TTY ---------- */
enum tty_backend {
    TTY_BACKEND_NONE = 0,    /* শুধু serial */
    TTY_BACKEND_FB,
    TTY_BACKEND_VGA
};

void tty_init(void);
void tty_attach_vga(volatile void *virt_base);   /* ধাপ ৩-এ VMM ডাকবে */
enum tty_backend tty_backend(void);
const char *tty_backend_name(void);
uint32_t tty_cols(void);
uint32_t tty_rows(void);
void tty_putc(char c);
void tty_write(const char *s, size_t n);
void tty_set_color(uint8_t fg, uint8_t bg);
void tty_clear(void);

#endif
