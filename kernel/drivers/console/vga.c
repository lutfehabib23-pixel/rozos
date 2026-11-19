/* rozOS — VGA টেক্সট-মোড ব্যাকএন্ড (80x25)
 *
 * প্রতিটি ঘর 2 বাইট: নিচের বাইটে অক্ষর, উপরের বাইটে রঙ (bg << 4 | fg)।
 * হার্ডওয়্যার কার্সর CRT controller পোর্ট 0x3D4/0x3D5 দিয়ে সরানো হয়।
 *
 * কেবল BIOS বুট + টেক্সট মোডে অর্থবহ; UEFI-তে VGA টেক্সট মোড থাকে না।
 * ফিজিক্যাল 0xB8000 Limine-এর HHDM-এ ম্যাপ করা থাকে না (base revision 3+),
 * তাই virt_base দেবে ধাপ ৩-এর VMM। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/console.h>
#include <arch/x86_64/io.h>

#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

static volatile uint16_t *vga_buf;
static int vga_ready;

int vga_init(volatile void *virt_base)
{
    vga_buf = (volatile uint16_t *)virt_base;
    vga_ready = (vga_buf != NULL);
    if (vga_ready) {
        /* কার্সর দৃশ্যমান: scanline 14-15 */
        outb(VGA_CRTC_INDEX, 0x0A);
        outb(VGA_CRTC_DATA, (uint8_t)((inb(VGA_CRTC_DATA) & 0xC0) | 14));
        outb(VGA_CRTC_INDEX, 0x0B);
        outb(VGA_CRTC_DATA, (uint8_t)((inb(VGA_CRTC_DATA) & 0xE0) | 15));
    }
    return vga_ready;
}

int vga_is_ready(void)
{
    return vga_ready;
}

void vga_put_cell(uint32_t col, uint32_t row, char ch, uint8_t fg, uint8_t bg)
{
    if (!vga_ready || col >= VGA_COLS || row >= VGA_ROWS) {
        return;
    }
    vga_buf[row * VGA_COLS + col] =
        (uint16_t)(uint8_t)ch | (uint16_t)(((bg & 0x0F) << 4 | (fg & 0x0F)) << 8);
}

void vga_set_cursor(uint32_t col, uint32_t row)
{
    uint16_t pos;
    if (!vga_ready) {
        return;
    }
    pos = (uint16_t)(row * VGA_COLS + col);
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)(pos >> 8));
}

void vga_clear(uint8_t bg)
{
    uint32_t i;
    if (!vga_ready) {
        return;
    }
    for (i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_buf[i] = (uint16_t)' ' | (uint16_t)(((bg & 0x0F) << 4 | TTY_COLOR_LIGHT_GREY) << 8);
    }
}
