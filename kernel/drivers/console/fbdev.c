/* rozOS — /dev/fb0: ফ্রেমবাফার ইউজার প্রোগ্রামের জন্য (ধাপ ১৩, rozos/fbdev.h)
 *
 *   ioctl(fd, FBIO_ROZ_GETINFO, &info)   মাপ, pitch, bpp, রঙের বিন্যাস (struct rozos_fb_info)
 *   mmap(NULL, info.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
 *                                        পর্দার পিক্সেল সরাসরি (write-combining, VMM_SHARED)
 *   ioctl(fd, KDSETMODE, KD_GRAPHICS / KD_TEXT)   কনসোলের আঁকা বন্ধ / ফেরত
 *   read/write/lseek                     offset = পর্দার বাইট (Linux fbdev-এর মতো)
 *
 * mmap করলেই গ্রাফিক্স মোড (কনসোল আর আঁকে না, যাতে প্রোগ্রামের ছবি নষ্ট না হয়)।
 * মালিক প্রসেস শেষ হলে (crash হলেও) কনসোল নিজে থেকেই ফিরে আসে (fbdev_proc_exit),
 * KD_TEXT দিলেও। কনসোলের অক্ষর-গ্রিড আর serial সবসময় চলে, তাই পরে কিছু হারায় না। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/fbdev.h>
#include <rozos/console.h>
#include <rozos/devfs.h>
#include <rozos/vmm.h>
#include <rozos/proc.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

static struct vnode *fb_vn;
static int owner_pid;                 /* গ্রাফিক্স মোডের মালিক (0 = কেউ না) */

static void set_text(void)
{
    if (fb_graphics_mode()) {
        fb_set_graphics(0);
        tty_refresh();
    }
    owner_pid = 0;
}

static void set_graphics(void)
{
    struct process *p = proc_current();
    fb_set_graphics(1);
    owner_pid = p ? p->pid : 0;
}

static int64_t fb0_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    const struct fb_info_k *fi = fb_info();
    (void)ctx;
    if (!fi) return -ENODEV;
    if (off >= fi->size) return 0;
    if (len > fi->size - off) len = (size_t)(fi->size - off);
    memcpy(buf, (const void *)(uintptr_t)(fi->virt + off), len);
    return (int64_t)len;
}

static int64_t fb0_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    const struct fb_info_k *fi = fb_info();
    (void)ctx;
    if (!fi) return -ENODEV;
    if (off >= fi->size) return -ENOSPC;
    if (len > fi->size - off) len = (size_t)(fi->size - off);
    memcpy((void *)(uintptr_t)(fi->virt + off), buf, len);
    return (int64_t)len;
}

/* offset = ফাইলের অবস্থান (ধাপ ১৩-এ vfs: ডিভাইসেও lseek চলে, read/write-এ এগোয়) */

void fbdev_init(void)
{
    struct vnode *vn;
    if (!fb_info()) return;
    if (devfs_register("fb0", fb0_read, fb0_write, NULL) < 0) return;
    if (vfs_lookup("/dev/fb0", &vn) == 0) {
        fb_vn = vn;                    /* devfs-এর vnode কখনো মুক্ত হয় না */
    }
}

int fbdev_is_vnode(struct vnode *vn)
{
    return vn && vn == fb_vn;
}

int fbdev_map(struct vmm_space *sp, uint64_t va, uint64_t off, uint64_t len, uint32_t flags)
{
    const struct fb_info_k *fi = fb_info();
    uint64_t i, size;
    if (!fi) return -ENODEV;
    size = (fi->size + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    if (off >= size || len > size - off) return -ENXIO;
    for (i = 0; i < len; i += VMM_PAGE_SIZE) {
        if (!vmm_map_page(sp, va + i, fi->phys + off + i, flags | VMM_SHARED | VMM_WC)) {
            uint64_t j;
            for (j = 0; j < i; j += VMM_PAGE_SIZE) vmm_unmap_page(sp, va + j);
            return -ENOMEM;
        }
    }
    set_graphics();
    return 0;
}

int fbdev_ioctl(struct vnode *vn, uint64_t req, uint64_t arg, int (*copy_out)(void *, const void *, size_t))
{
    const struct fb_info_k *fi = fb_info();
    if (!fbdev_is_vnode(vn)) return -ENOTTY;
    if (!fi) return -ENODEV;
    if (req == FBIO_ROZ_GETINFO) {
        struct rozos_fb_info info;
        memset(&info, 0, sizeof(info));
        info.width = fi->width;
        info.height = fi->height;
        info.pitch = fi->pitch;
        info.bpp = fi->bpp;
        info.red_size = fi->red_size;     info.red_shift = fi->red_shift;
        info.green_size = fi->green_size; info.green_shift = fi->green_shift;
        info.blue_size = fi->blue_size;   info.blue_shift = fi->blue_shift;
        info.size = fi->size;
        info.text_cols = tty_cols();
        info.text_rows = tty_rows();
        return copy_out((void *)(uintptr_t)arg, &info, sizeof(info));
    }
    if (req == KDSETMODE) {
        if (arg == KD_GRAPHICS) set_graphics();
        else if (arg == KD_TEXT) set_text();
        else return -EINVAL;
        return 0;
    }
    if (req == KDGETMODE) {
        int m = fb_graphics_mode() ? KD_GRAPHICS : KD_TEXT;
        return copy_out((void *)(uintptr_t)arg, &m, sizeof(m));
    }
    return -ENOTTY;
}

void fbdev_proc_exit(int pid)
{
    if (owner_pid && pid == owner_pid) set_text();
}

void fbdev_force_text(void)
{
    set_text();
}
