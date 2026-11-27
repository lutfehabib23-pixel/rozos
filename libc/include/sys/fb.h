/* rozlibc — /dev/fb0: ফ্রেমবাফার (ধাপ ১৩)
 *
 *   int fd = open("/dev/fb0", O_RDWR);
 *   struct rozos_fb_info fi;
 *   ioctl(fd, FBIO_ROZ_GETINFO, &fi);          // fi.width, fi.height, fi.pitch ...
 *   uint32_t *px = mmap(NULL, fi.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
 *   px[y * (fi.pitch / 4) + x] = rgb;          // রঙ: fi.red_shift ইত্যাদি দিয়ে
 *
 * mmap করলেই কনসোল আঁকা থামায়; প্রোগ্রাম শেষ হলে (crash হলেও) টেক্সট ফিরে আসে।
 * নিজে বদলাতে: ioctl(fd, KDSETMODE, KD_TEXT / KD_GRAPHICS)।
 * সহজ উপায়: librozgfx (<rozgfx.h>) — এসব নিজে করে। kernel/include/rozos/fbdev.h-এর
 * সাথে মিলতে হবে। */
#ifndef _SYS_FB_H
#define _SYS_FB_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define FBIO_ROZ_GETINFO 0x46F0
#define KDSETMODE        0x4B3A
#define KDGETMODE        0x4B3B
#define KD_TEXT          0
#define KD_GRAPHICS      1

struct rozos_fb_info {
    uint32_t width, height;          /* পিক্সেল */
    uint32_t pitch;                  /* এক লাইনের বাইট */
    uint32_t bpp;                    /* 32 */
    uint8_t  red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
    uint8_t  pad[2];
    uint64_t size;                   /* mmap-এর মাপ */
    uint32_t text_cols, text_rows;
    uint32_t reserved[8];
};

#ifdef __cplusplus
}
#endif
#endif
