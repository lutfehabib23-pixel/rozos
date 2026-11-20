/* rozOS — /dev/fb0 (ধাপ ১৩): ফ্রেমবাফার ইউজার প্রোগ্রামের জন্য
 * বিস্তারিত drivers/console/fbdev.c-এর মাথায়। ioctl নম্বর আর struct libc-র
 * <sys/fb.h>-এর সাথে মিলতে হবে। */
#ifndef ROZOS_FBDEV_H
#define ROZOS_FBDEV_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/vmm.h>

#define FBIO_ROZ_GETINFO 0x46F0      /* struct rozos_fb_info */
#define KDSETMODE        0x4B3A      /* Linux-এর মতো */
#define KDGETMODE        0x4B3B
#define KD_TEXT          0
#define KD_GRAPHICS      1

struct rozos_fb_info {
    uint32_t width, height;          /* পিক্সেল */
    uint32_t pitch;                  /* এক লাইনের বাইট */
    uint32_t bpp;                    /* 32 */
    uint8_t  red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
    uint8_t  pad[2];
    uint64_t size;                   /* mmap-এর মাপ (pitch * height) */
    uint32_t text_cols, text_rows;   /* টেক্সট কনসোলের মাপ */
    uint32_t reserved[8];
};

void fbdev_init(void);               /* devfs-এর পরে: /dev/fb0 */
int  fbdev_is_vnode(struct vnode *vn);
int  fbdev_map(struct vmm_space *sp, uint64_t va, uint64_t off, uint64_t len, uint32_t flags);
int  fbdev_ioctl(struct vnode *vn, uint64_t req, uint64_t arg,
                 int (*copy_out)(void *udst, const void *src, size_t len));
void fbdev_proc_exit(int pid);       /* মালিক শেষ → কনসোল ফেরত */
void fbdev_force_text(void);         /* panic ইত্যাদি */

#endif
