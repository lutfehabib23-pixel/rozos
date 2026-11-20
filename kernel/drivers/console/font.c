/* rozOS — কনসোল ফন্ট
 *
 * এখন ফন্ট আসে Limine-এর "Flanterm FB Init Params" feature থেকে (PROTOCOL.md):
 * 256 গ্লিফের VGA-ধাঁচের বিটম্যাপ, প্রস্থ সবসময় 8, আকার
 * font_width * font_height * 256 / 8 বাইট।
 *
 * সতর্কতা: ফন্টটি bootloader-reclaimable মেমোরিতে থাকে। ধাপ ৩-এ PMM ওই মেমোরি
 * ফেরত নেওয়ার আগে font_init() ফন্টটা কার্নেলের নিজস্ব বাফারে কপি করে রাখে
 * (নিচের font_copy), তাই পরে reclaim করলেও কনসোল ভাঙবে না। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/console.h>
#include <rozos/boot.h>
#include <rozos/string.h>

/* সর্বোচ্চ 8x32 ফন্ট কপি রাখার জায়গা (256 * 32 = 8 KiB) */
#define FONT_MAX_HEIGHT 32
static uint8_t font_copy[256 * FONT_MAX_HEIGHT];

static struct rozos_font font;
static int font_ready;
/* ধাপ ১৩: setfont-এর ফন্ট — 256 গ্লিফ × 32 লাইন × 2 বাইট */
#define FONT_USER_MAX (256 * 32 * 2)
static uint8_t font_user[FONT_USER_MAX];
static struct rozos_font boot_font;     /* Limine-এর (font_reset-এর জন্য) */

int font_init(void)
{
    struct limine_flanterm_fb_init_params_response *r = limine_flanterm_req.response;
    struct limine_flanterm_fb_init_params *p;

    font_ready = 0;
    if (!r || r->entry_count == 0 || !r->entries) {
        return 0;
    }
    p = r->entries[0];               /* প্রথম ফ্রেমবাফারের প্যারামিটার */
    if (!p || !p->font || p->font_width != 8 ||
        p->font_height == 0 || p->font_height > FONT_MAX_HEIGHT) {
        return 0;                    /* non-32bpp হলে entry শূন্য থাকে */
    }

    memcpy(font_copy, (const void *)p->font, (size_t)256 * p->font_height);

    font.glyphs  = font_copy;
    font.width   = 8;
    font.height  = (uint32_t)p->font_height;
    font.scale_x = p->font_scale_x ? (uint32_t)p->font_scale_x : 1;
    font.scale_y = p->font_scale_y ? (uint32_t)p->font_scale_y : 1;
    font.spacing = (uint32_t)p->font_spacing;
    font.bytes_per_row = 1;
    boot_font = font;
    font_ready = 1;
    return 1;
}

int font_set(const uint8_t *glyphs, uint32_t width, uint32_t height)
{
    uint32_t bpr = (width + 7) / 8;
    if (width == 0 || width > 16 || height == 0 || height > 32) {
        return -1;
    }
    memcpy(font_user, glyphs, (size_t)256 * height * bpr);
    font.glyphs = font_user;
    font.width = width;
    font.height = height;
    font.scale_x = 1;
    font.scale_y = 1;
    font.spacing = 0;
    font.bytes_per_row = bpr;
    font_ready = 1;
    return 0;
}

void font_reset(void)
{
    if (boot_font.glyphs) {
        font = boot_font;
        font_ready = 1;
    }
}

const struct rozos_font *font_get(void)
{
    return font_ready ? &font : NULL;
}
