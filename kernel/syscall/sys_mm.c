/* rozOS — মেমোরি syscall: brk, mmap, munmap, mprotect (Linux x86-64 ABI)
 *
 * সব পেজ সাথে সাথে বরাদ্দ ও শূন্য করা হয় (demand paging শুধু স্ট্যাকে)।
 * mmap: বেনামি (MAP_ANONYMOUS) বা ফাইলের private কপি (ফাইল থেকে পড়ে নেওয়া —
 * পরে লেখা ফাইলে ফেরত যায় না)।
 * ধাপ ১৩: MAP_SHARED — shm_open-এর fd (kernel/mm/shm.c), MAP_SHARED|MAP_ANONYMOUS
 * (fork-এর পর মা-সন্তান ভাগ করে), আর /dev/fb0 (ফ্রেমবাফার, fbdev.c)। সাধারণ ফাইলের
 * shared ম্যাপিং এখনো নয় (-ENODEV)। ভাগ করা পেজ VMM_SHARED — আনম্যাপে মুক্ত হয় না। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/syscall.h>
#include <rozos/proc.h>
#include <rozos/vmm.h>
#include <rozos/pmm.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/shm.h>
#include <rozos/uaccess.h>
#include <rozos/fbdev.h>


#define PROT_WRITE    0x2
#define PROT_EXEC     0x4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define PAGE_UP(x)   (((x) + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1))
#define PAGE_DOWN(x) ((x) & ~(VMM_PAGE_SIZE - 1))

static uint32_t prot_flags(uint64_t prot)
{
    uint32_t f = VMM_USER;
    if (prot & PROT_WRITE) f |= VMM_WRITE;
    if (prot & PROT_EXEC)  f |= VMM_EXEC;
    return f;
}

static void unmap_range(struct process *p, uint64_t start, uint64_t end)
{
    uint64_t va;
    for (va = start; va < end; va += VMM_PAGE_SIZE) {
        uint64_t phys = vmm_unmap_page(p->space, va);
        if (phys) {
            pmm_free_page(phys);
        }
    }
}

/* [start, end) নতুন শূন্য পেজে ম্যাপ; ব্যর্থ হলে যা ম্যাপ হয়েছিল ফেরত */
static int map_zero_range(struct process *p, uint64_t start, uint64_t end, uint32_t flags)
{
    uint64_t va;
    for (va = start; va < end; va += VMM_PAGE_SIZE) {
        uint64_t phys = pmm_alloc_zeroed_page();
        if (!phys || !vmm_map_page(p->space, va, phys, flags)) {
            if (phys) {
                pmm_free_page(phys);
            }
            unmap_range(p, start, va);
            return -ENOMEM;
        }
    }
    return 0;
}

static int64_t sys_brk(struct interrupt_frame *f, uint64_t addr, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t old_top, new_top;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Linux-এর মতো: ব্যর্থ হলে পুরোনো brk ফেরত (ত্রুটি নয়) */
    if (addr < p->brk_start || addr >= USER_MMAP_BASE) {
        return (int64_t)p->brk_end;
    }
    old_top = PAGE_UP(p->brk_end);
    new_top = PAGE_UP(addr);
    if (new_top > old_top) {
        if (map_zero_range(p, old_top, new_top, VMM_USER | VMM_WRITE) < 0) {
            return (int64_t)p->brk_end;
        }
    } else if (new_top < old_top) {
        unmap_range(p, new_top, old_top);
    }
    p->brk_end = addr;
    return (int64_t)addr;
}

static int64_t sys_mmap(struct interrupt_frame *f, uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t off)
{
    struct process *p = proc_current();
    uint64_t size = PAGE_UP(len), start;
    uint32_t vf = prot_flags(prot);
    int err;
    (void)f;

    if (len == 0 || size < len || (off & (VMM_PAGE_SIZE - 1))) {
        return -EINVAL;
    }
    if (!(flags & (MAP_PRIVATE | MAP_SHARED))) {
        return -EINVAL;
    }
    if ((flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS)) {
        /* ধাপ ১৩: shm বা /dev/fb0 — নিচে জায়গা ঠিক করে ম্যাপ */
        struct vnode *vn = vfs_fd_vnode((int)fd);
        struct shm_obj *o;
        int is_fb;
        if (!vn) return -EBADF;
        o = shm_from_vnode(vn);
        is_fb = !o && fbdev_is_vnode(vn);
        if (!o && !is_fb) {
            vnode_unref(vn);
            return -ENODEV;                      /* সাধারণ ফাইলের shared ম্যাপিং এখনো নেই */
        }
        if ((prot & PROT_WRITE) && (vfs_fd_flags((int)fd) & O_ACCMODE) == O_RDONLY) {
            vnode_unref(vn);
            return -EACCES;
        }
        if (flags & MAP_FIXED) {
            if ((addr & (VMM_PAGE_SIZE - 1)) || addr < USER_MIN_ADDR ||
                addr + size < addr || addr + size > USER_STACK_TOP - USER_STACK_MAX) {
                vnode_unref(vn);
                return -EINVAL;
            }
            start = addr;
            unmap_range(p, start, start + size);
        } else {
            if (p->mmap_next + size > USER_MMAP_END || p->mmap_next + size < p->mmap_next) {
                vnode_unref(vn);
                return -ENOMEM;
            }
            start = p->mmap_next;
            p->mmap_next += size + VMM_PAGE_SIZE;
        }
        err = o ? shm_map(o, p->space, start, off, size, vf)
                : fbdev_map(p->space, start, off, size, vf);
        vnode_unref(vn);
        return err < 0 ? err : (int64_t)start;
    }

    if (flags & MAP_FIXED) {
        if ((addr & (VMM_PAGE_SIZE - 1)) || addr < USER_MIN_ADDR ||
            addr + size < addr || addr + size > USER_STACK_TOP - USER_STACK_MAX) {
            return -EINVAL;
        }
        start = addr;
        unmap_range(p, start, start + size);
    } else {
        if (p->mmap_next + size > USER_MMAP_END || p->mmap_next + size < p->mmap_next) {
            return -ENOMEM;
        }
        start = p->mmap_next;
        p->mmap_next += size + VMM_PAGE_SIZE;   /* মাঝে একটা ফাঁকা পেজ: overflow ধরা পড়ে */
    }

    if ((flags & MAP_SHARED) && (flags & MAP_ANONYMOUS)) {
        /* ধাপ ১৩: নামহীন ভাগ করা মেমোরি — fork-এর পর মা-সন্তান একই পেজ দেখে */
        struct shm_obj *o = shm_anon_create(size);
        if (!o) return -ENOMEM;
        err = shm_map(o, p->space, start, 0, size, vf);
        shm_anon_done(o);
        return err < 0 ? err : (int64_t)start;
    }

    err = map_zero_range(p, start, start + size, vf);
    if (err < 0) {
        return err;
    }

    if (!(flags & MAP_ANONYMOUS)) {
        /* ফাইলের অংশ পড়ে নেওয়া — HHDM দিয়ে, যাতে PROT_READ-only হলেও লেখা যায় */
        uint64_t done = 0;
        if (vfs_seek((int)fd, (int64_t)off, SEEK_SET) < 0) {
            unmap_range(p, start, start + size);
            return -EBADF;
        }
        while (done < len) {
            uint64_t phys = vmm_virt_to_phys(p->space, start + done);
            size_t chunk = (size_t)(len - done < VMM_PAGE_SIZE ? len - done : VMM_PAGE_SIZE);
            int64_t n = vfs_read((int)fd, pmm_phys_to_virt(phys), chunk);
            if (n < 0) {
                unmap_range(p, start, start + size);
                return n;
            }
            if (n == 0) {
                break;                           /* ফাইলের শেষ — বাকিটা শূন্য */
            }
            done += (uint64_t)n;
        }
    }
    return (int64_t)start;
}

static int64_t sys_munmap(struct interrupt_frame *f, uint64_t addr, uint64_t len, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t end = PAGE_UP(addr + len);
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;

    if ((addr & (VMM_PAGE_SIZE - 1)) || len == 0 || end < addr || end > VMM_USER_TOP) {
        return -EINVAL;
    }
    unmap_range(p, addr, end);
    return 0;
}

static int64_t sys_mprotect(struct interrupt_frame *f, uint64_t addr, uint64_t len, uint64_t prot,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t va, end = PAGE_UP(addr + len);
    uint32_t vf = prot_flags(prot);
    (void)f; (void)a4; (void)a5; (void)a6;

    if ((addr & (VMM_PAGE_SIZE - 1)) || end < addr || end > VMM_USER_TOP) {
        return -EINVAL;
    }
    for (va = addr; va < end; va += VMM_PAGE_SIZE) {
        uint64_t phys;
        uint32_t old;
        if (!vmm_query(p->space, va, &phys, &old)) {
            return -ENOMEM;
        }
        /* ধাপ ১৩: ভাগ করা/ক্যাশের ধরন রাখা (shm, ফ্রেমবাফারের WC) */
        vmm_map_page(p->space, va, phys, vf | (old & (VMM_SHARED | VMM_WC | VMM_NOCACHE)));
    }
    return 0;
}

/* ---------- ধাপ ১৩: shm_open / shm_unlink / ftruncate ---------- */

static int64_t sys_shm_open(struct interrupt_frame *f, uint64_t uname, uint64_t flags, uint64_t mode,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    char name[SHM_NAME_MAX + 2];
    (void)f; (void)mode; (void)a4; (void)a5; (void)a6;
    if (copy_string_from_user(name, (const char *)(uintptr_t)uname, sizeof(name)) < 0) return -EFAULT;
    return shm_open_fd(name, (int)flags);
}

static int64_t sys_shm_unlink(struct interrupt_frame *f, uint64_t uname, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    char name[SHM_NAME_MAX + 2];
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (copy_string_from_user(name, (const char *)(uintptr_t)uname, sizeof(name)) < 0) return -EFAULT;
    return shm_unlink_name(name);
}

static int64_t sys_ftruncate(struct interrupt_frame *f, uint64_t fd, uint64_t size, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if ((int64_t)size < 0) return -EINVAL;
    return vfs_ftruncate((int)fd, size);
}

void sys_mm_register(void)
{
    syscall_register(SYS_rozos_shm_open, sys_shm_open, "shm_open");     /* ধাপ ১৩ */
    syscall_register(SYS_rozos_shm_unlink, sys_shm_unlink, "shm_unlink");
    syscall_register(SYS_ftruncate, sys_ftruncate, "ftruncate");
    syscall_register(SYS_brk, sys_brk, "brk");
    syscall_register(SYS_mmap, sys_mmap, "mmap");
    syscall_register(SYS_munmap, sys_munmap, "munmap");
    syscall_register(SYS_mprotect, sys_mprotect, "mprotect");
}
