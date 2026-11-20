/* rozOS — ফ্রেমবাফার কনসোল ব্যাকএন্ড
 *
 * Limine-এর প্রথম ফ্রেমবাফারে অক্ষর আঁকে। শুধু 32bpp, RGB মেমোরি মডেল সাপোর্টেড
 * (QEMU ও আধুনিক হার্ডওয়্যারে এটাই স্বাভাবিক)। রঙের বিন্যাস red/green/blue
 * mask-এর size ও shift থেকে হিসাব করা হয়, তাই RGB/BGR দুটোই কাজ করে।
 *
 * ফ্রেমবাফার write-combining মেমোরি — সেখান থেকে পড়া খুব ধীর। তাই স্ক্রল করার
 * সময় পিক্সেল সরানো হয় না; tty.c নিজের অক্ষর-গ্রিড থেকে পুরো স্ক্রিন আবার আঁকে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/console.h>
#include <rozos/boot.h>

/* VGA-র স্ট্যান্ডার্ড 16 রঙ, 0xRRGGBB */
static const uint32_t fb_palette_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static volatile uint8_t *fb_addr;
static uint64_t fb_w, fb_h, fb_pitch;
static uint16_t fb_depth;
static uint32_t fb_palette[16];           /* ফ্রেমবাফারের নিজস্ব ফরম্যাটে রূপান্তরিত */
static uint32_t cell_w, cell_h, cols, rows;
static const struct rozos_font *fnt;
static int fb_ready;
static int fb_graphics;                   /* ধাপ ১৩: /dev/fb0-এর মালিক আঁকছে — কনসোল আঁকে না */
static struct fb_info_k fb_info_v;        /* ধাপ ১৩: /dev/fb0-এর জন্য */

static uint32_t pack_channel(uint32_t value8, uint8_t size, uint8_t shift)
{
    if (size == 0) {
        return 0;
    }
    if (size < 8) {
        value8 >>= (8 - size);
    }
    return value8 << shift;
}

int fb_init(void)
{
    struct limine_framebuffer_response *r = limine_framebuffer_req.response;
    struct limine_framebuffer *f;
    int i;

    fb_ready = 0;
    fnt = font_get();
    if (!r || r->framebuffer_count == 0 || !fnt) {
        return 0;
    }
    f = r->framebuffers[0];
    if (!f || f->bpp != 32 || f->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        return 0;
    }

    fb_addr  = (volatile uint8_t *)f->address;
    fb_w     = f->width;
    fb_h     = f->height;
    fb_pitch = f->pitch;
    fb_depth = f->bpp;
    fb_info_v.virt = (uint64_t)(uintptr_t)f->address;
    fb_info_v.phys = fb_info_v.virt - boot_hhdm_offset();
    fb_info_v.width = (uint32_t)f->width;
    fb_info_v.height = (uint32_t)f->height;
    fb_info_v.pitch = (uint32_t)f->pitch;
    fb_info_v.bpp = f->bpp;
    fb_info_v.red_size = f->red_mask_size;     fb_info_v.red_shift = f->red_mask_shift;
    fb_info_v.green_size = f->green_mask_size; fb_info_v.green_shift = f->green_mask_shift;
    fb_info_v.blue_size = f->blue_mask_size;   fb_info_v.blue_shift = f->blue_mask_shift;
    fb_info_v.size = f->pitch * f->height;

    for (i = 0; i < 16; i++) {
        uint32_t rgb = fb_palette_rgb[i];
        fb_palette[i] = pack_channel((rgb >> 16) & 0xFF, f->red_mask_size,   f->red_mask_shift)
                      | pack_channel((rgb >> 8)  & 0xFF, f->green_mask_size, f->green_mask_shift)
                      | pack_channel(rgb & 0xFF,         f->blue_mask_size,  f->blue_mask_shift);
    }

    if (!fb_relayout()) {
        return 0;
    }
    fb_ready = 1;
    return 1;
}

/* ধাপ ১৩: ফন্ট থেকে ঘরের মাপ আর কলাম/সারি (fb_init আর setfont-এর পরে) */
int fb_relayout(void)
{
    fnt = font_get();
    if (!fnt) {
        return 0;
    }
    cell_w = fnt->width * fnt->scale_x + fnt->spacing;
    cell_h = fnt->height * fnt->scale_y;
    if (cell_w == 0 || cell_h == 0) {
        return 0;
    }
    cols = (uint32_t)(fb_w / cell_w);
    rows = (uint32_t)(fb_h / cell_h);
    return cols != 0 && rows != 0;
}

int fb_is_ready(void)      { return fb_ready; }
const struct fb_info_k *fb_info(void) { return fb_ready ? &fb_info_v : NULL; }
int fb_graphics_mode(void) { return fb_graphics; }

/* ধাপ ১৩: on = গ্রাফিক্স প্রোগ্রাম আঁকছে (কনসোলের আঁকা বন্ধ; অক্ষর-গ্রিড আর serial চলে),
 * off = কনসোল ফেরত — ডাকা ফাংশন তারপর tty_refresh() দিয়ে পুরো পর্দা আবার আঁকে */
void fb_set_graphics(int on)
{
    fb_graphics = on ? 1 : 0;
}
uint32_t fb_cols(void)     { return cols; }
uint32_t fb_rows(void)     { return rows; }
uint64_t fb_width(void)    { return fb_w; }
uint64_t fb_height(void)   { return fb_h; }
uint16_t fb_bpp(void)      { return fb_depth; }

static void fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color)
{
    uint64_t row, col;
    for (row = 0; row < h; row++) {
        volatile uint32_t *line = (volatile uint32_t *)(fb_addr + (y + row) * fb_pitch) + x;
        for (col = 0; col < w; col++) {
            line[col] = color;
        }
    }
}

void fb_put_cell(uint32_t col, uint32_t row, char ch, uint8_t fg, uint8_t bg)
{
    const uint8_t *glyph;
    uint32_t fgc, bgc, gy, gx, sy, sx;
    uint64_t px, py;

    if (!fb_ready || fb_graphics || col >= cols || row >= rows) {
        return;
    }
    glyph = fnt->glyphs + (uint32_t)(uint8_t)ch * fnt->height * fnt->bytes_per_row;
    fgc = fb_palette[fg & 0x0F];
    bgc = fb_palette[bg & 0x0F];
    px = (uint64_t)col * cell_w;
    py = (uint64_t)row * cell_h;

    for (gy = 0; gy < fnt->height; gy++) {
        const uint8_t *row_bits = glyph + gy * fnt->bytes_per_row;   /* ধাপ ১৩: চওড়া ফন্টও */
        for (sy = 0; sy < fnt->scale_y; sy++) {
            volatile uint32_t *line =
                (volatile uint32_t *)(fb_addr + (py + gy * fnt->scale_y + sy) * fb_pitch) + px;
            uint32_t x = 0;
            for (gx = 0; gx < fnt->width; gx++) {
                uint32_t c = (row_bits[gx >> 3] & (0x80u >> (gx & 7))) ? fgc : bgc;
                for (sx = 0; sx < fnt->scale_x; sx++) {
                    line[x++] = c;
                }
            }
            for (sx = 0; sx < fnt->spacing; sx++) {
                line[x++] = bgc;
            }
        }
    }
}

void fb_draw_cursor(uint32_t col, uint32_t row, uint8_t color)
{
    uint32_t thickness;
    if (!fb_ready || fb_graphics || col >= cols || row >= rows) {
        return;
    }
    /* অক্ষরের নিচে আন্ডারলাইন কার্সর (উচ্চতার ১/৮, কমপক্ষে 2 পিক্সেল) */
    thickness = cell_h / 8;
    if (thickness < 2) {
        thickness = 2;
    }
    fill_rect((uint64_t)col * cell_w, (uint64_t)row * cell_h + cell_h - thickness,
              cell_w, thickness, fb_palette[color & 0x0F]);
}

void fb_clear(uint8_t bg)
{
    if (!fb_ready || fb_graphics) {
        return;
    }
    fill_rect(0, 0, fb_w, fb_h, fb_palette[bg & 0x0F]);
}
