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
    uint32_t bytes_per_row;  /* ধাপ ১৩: প্রতি গ্লিফ-লাইনে কত বাইট ((width+7)/8) */
};

int  font_init(void);
const struct rozos_font *font_get(void);
/* ধাপ ১৩: নতুন ফন্ট (setfont, PSF থেকে): 256 গ্লিফ, width ≤ 16, height ≤ 32,
 * প্রতি লাইনে (width+7)/8 বাইট; 0 = ঠিক আছে। font_reset = বুটের (Limine) ফন্ট। */
int  font_set(const uint8_t *glyphs, uint32_t width, uint32_t height);
void font_reset(void);
/* ধাপ ১৩: ফন্ট বদলের পর ঘরের মাপ নতুন করে হিসাব, কনসোল নতুন মাপে (পর্দা মোছে) */
int  fb_relayout(void);
void tty_font_changed(void);

/* ---------- ফ্রেমবাফার ব্যাকএন্ড ---------- */
int      fb_init(void);
int      fb_is_ready(void);
uint32_t fb_cols(void);
uint32_t fb_rows(void);
uint64_t fb_width(void);
uint64_t fb_height(void);
/* ধাপ ১৯ঝ: framebuffer-এর ভার্চুয়াল ঠিকানা (Limine-এর HHDM-এ) আর মাপ (pitch × height); না থাকলে 0 */
int fb_region(uint64_t *virt, uint64_t *bytes);
uint16_t fb_bpp(void);
void     fb_put_cell(uint32_t col, uint32_t row, char ch, uint8_t fg, uint8_t bg);
void     fb_draw_cursor(uint32_t col, uint32_t row, uint8_t color);
void     fb_clear(uint8_t bg);
/* ধাপ ১৩: /dev/fb0 (drivers/console/fbdev.c) */
struct fb_info_k {
    uint64_t phys, virt, size;
    uint32_t width, height, pitch;
    uint16_t bpp;
    uint8_t  red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};
const struct fb_info_k *fb_info(void);   /* ফ্রেমবাফার না থাকলে NULL */
void     fb_set_graphics(int on);
int      fb_graphics_mode(void);
void     tty_refresh(void);              /* পুরো পর্দা গ্রিড থেকে আবার আঁকা */

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

/* ধাপ ১২: প্রোগ্রাম কোন মাউস-রিপোর্টিং চেয়েছে (ESC[?1000h ইত্যাদি) */
#define TTY_MOUSE_CLICKS 0x01      /* ?1000: চাপা/ছাড়া, চাকা */
#define TTY_MOUSE_DRAG   0x02      /* ?1002: বোতাম চেপে নড়াচড়া */
#define TTY_MOUSE_MOTION 0x04      /* ?1003: সব নড়াচড়া */
#define TTY_MOUSE_SGR    0x08      /* ?1006: ESC[<b;x;yM ভাষা */
int  tty_mouse_mode(void);
/* ফ্রেমবাফারে মাউস-কার্সর (উল্টো রঙের ঘর) দেখানো/সরানো */
void tty_mouse_pointer(int visible, uint32_t col, uint32_t row);
void tty_clear(void);

#endif
