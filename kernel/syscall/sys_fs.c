/* rozOS — ফাইল-সংক্রান্ত syscall (Linux x86-64 ABI)
 *
 * ইউজারের বাফার সরাসরি VFS-এ দেওয়া হয় না: প্রথমে কার্নেলের বাউন্স-বাফারে
 * (সর্বোচ্চ 64 KiB করে) কপি, তারপর copy_to_user/copy_from_user — যাতে ভুল
 * ঠিকানা কার্নেলের ভেতরে page fault না ঘটায়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/syscall.h>
#include <rozos/proc.h>
#include <rozos/vfs.h>
#include <rozos/module.h>
#include <rozos/usb.h>
#include <rozos/block.h>
#include <rozos/fat.h>
#include <rozos/printf.h>
#include <rozos/devfs.h>
#include <rozos/uaccess.h>
#include <rozos/heap.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/console.h>
#include <rozos/sched.h>
#include <rozos/fbdev.h>
#include <rozos/pty.h>
#include <rozos/ttyctl.h>
#include <rozos/timer.h>
#include <arch/x86_64/cpu.h>

#define IO_CHUNK (64 * 1024)

/* Linux struct stat (x86-64, 144 বাইট) */
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime, st_atime_nsec;
    uint64_t st_mtime, st_mtime_nsec;
    uint64_t st_ctime, st_ctime_nsec;
    int64_t  reserved[3];
};
typedef char linux_stat_size_check[(sizeof(struct linux_stat) == 144) ? 1 : -1];

#define S_IFCHR 0020000
#define S_IFSOCK 0140000
#define S_IFIFO 0010000
#define S_IFDIR 0040000
#define S_IFREG 0100000

#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8

/* ধাপ ১২: rozOS-এর নিজস্ব — serial টার্মিনালের মাপ জিজ্ঞেস করে কনসোলের মাপ বসানো।
 * DETECT: হাতে (stty ROWS COLS) বসানো থাকলে সেটাই রাখে; DETECT_FORCE: সবসময় জিজ্ঞেস করে */
#define TIOCROZDETECT       0x54F0
#define TIOCROZDETECT_FORCE 0x54F1
#define TIOCROZSETFONT      0x54F2   /* ধাপ ১৩: struct rozos_font_req (setfont) */

struct rozos_font_req {
    uint32_t width, height;          /* পিক্সেল; width ≤ 16, height ≤ 32 */
    uint32_t flags;                  /* 1 = বুটের ফন্টে ফেরা (data লাগে না) */
    uint32_t pad;
    uint64_t data;                   /* 256 গ্লিফ, প্রতিটা height × ((width+7)/8) বাইট */
};

/* ধাপ ৭: কনসোলের মাপ। 0 = স্ক্রিনের মাপ (ফ্রেমবাফার/VGA), স্ক্রিন না থাকলে 80x24।
 * serial টার্মিনালের মাপ আলাদা হলে `stty <rows> <cols>` (TIOCSWINSZ) দিয়ে বদলানো যায়। */
static uint16_t console_rows, console_cols;
static int console_manual;               /* ধাপ ১২: stty ROWS COLS দিয়ে বসানো */

static int user_path(uint64_t upath, char *out)
{
    char tmp[VFS_PATH_MAX];
    int64_t n = copy_string_from_user(tmp, (const char *)(uintptr_t)upath, sizeof(tmp));
    if (n < 0) {
        return (int)n;
    }
    return proc_abs_path(tmp, out, VFS_PATH_MAX);
}

/* ধাপ ১৬ক: read-এর ভেতরটা আলাদা (readv আর pread64-ও এটা ব্যবহার করে) */
static int64_t do_read(int fd, uint64_t ubuf, uint64_t len)
{
    uint8_t *kbuf;
    int64_t total = 0;

    if (len == 0) {
        return 0;
    }
    if (!user_range_ok(ubuf, len, 1)) {
        return -EFAULT;
    }
    kbuf = (uint8_t *)kmalloc(len < IO_CHUNK ? len : IO_CHUNK);
    if (!kbuf) {
        return -ENOMEM;
    }
    while (len > 0) {
        size_t n = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
        int64_t r = vfs_read(fd, kbuf, n);
        if (r < 0) {
            if (total == 0) total = r;
            break;
        }
        if (r == 0) {
            break;
        }
        if (copy_to_user((void *)(uintptr_t)(ubuf + (uint64_t)total), kbuf, (size_t)r) < 0) {
            total = total ? total : -EFAULT;
            break;
        }
        total += r;
        len -= (uint64_t)r;
        if ((size_t)r < n) {
            break;                               /* ছোট পড়া (ডিভাইস বা ফাইলের শেষ) */
        }
    }
    kfree(kbuf);
    return total;
}

static int64_t sys_read(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a4; (void)a5; (void)a6;
    return do_read((int)fd, ubuf, len);
}

static int64_t do_write(int fd, uint64_t ubuf, uint64_t len)
{
    uint8_t *kbuf;
    int64_t total = 0;

    if (len == 0) {
        return 0;
    }
    if (!user_range_ok(ubuf, len, 0)) {
        return -EFAULT;
    }
    kbuf = (uint8_t *)kmalloc(len < IO_CHUNK ? len : IO_CHUNK);
    if (!kbuf) {
        return -ENOMEM;
    }
    while (len > 0) {
        size_t n = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
        int64_t r;
        memcpy(kbuf, (const void *)(uintptr_t)(ubuf + (uint64_t)total), n);
        r = vfs_write(fd, kbuf, n);
        if (r < 0) {
            if (total == 0) total = r;
            break;
        }
        total += r;
        len -= (uint64_t)r;
        if ((size_t)r < n) {
            break;
        }
    }
    kfree(kbuf);
    return total;
}

static int64_t sys_write(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a4; (void)a5; (void)a6;
    return do_write((int)fd, ubuf, len);
}

struct linux_iovec {
    uint64_t base;
    uint64_t len;
};

static int64_t sys_writev(struct interrupt_frame *f, uint64_t fd, uint64_t uiov, uint64_t cnt,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct linux_iovec iov;
    int64_t total = 0;
    uint64_t i;
    (void)f; (void)a4; (void)a5; (void)a6;

    if (cnt > 1024) {
        return -EINVAL;
    }
    for (i = 0; i < cnt; i++) {
        int64_t r;
        if (copy_from_user(&iov, (const void *)(uintptr_t)(uiov + i * sizeof(iov)), sizeof(iov)) < 0) {
            return total ? total : -EFAULT;
        }
        r = do_write((int)fd, iov.base, iov.len);
        if (r < 0) {
            return total ? total : r;
        }
        total += r;
        if ((uint64_t)r < iov.len) {
            break;
        }
    }
    return total;
}

/* ধাপ ১৬ক: readv — প্রতিটা iovec-এ পালা করে পড়া, ছোট পড়া হলে থামা */
static int64_t sys_readv(struct interrupt_frame *f, uint64_t fd, uint64_t uiov, uint64_t cnt,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct linux_iovec iov;
    int64_t total = 0;
    uint64_t i;
    (void)f; (void)a4; (void)a5; (void)a6;

    if (cnt > 1024) {
        return -EINVAL;
    }
    for (i = 0; i < cnt; i++) {
        int64_t r;
        if (copy_from_user(&iov, (const void *)(uintptr_t)(uiov + i * sizeof(iov)), sizeof(iov)) < 0) {
            return total ? total : -EFAULT;
        }
        r = do_read((int)fd, iov.base, iov.len);
        if (r < 0) {
            return total ? total : r;
        }
        total += r;
        if ((uint64_t)r < iov.len) {
            break;
        }
    }
    return total;
}

/* ধাপ ১৬ক: pread64/pwrite64 — অবস্থান সরিয়ে পড়া/লেখা, তারপর আগের অবস্থানে ফেরা
 * (ফাইলের offset বদলায় না; এক প্রসেসে থ্রেড নেই বলে এখন এটাই যথেষ্ট) */
static int64_t do_pio(int fd, uint64_t ubuf, uint64_t len, int64_t off, int writing)
{
    int64_t cur, r;
    if (off < 0) {
        return -EINVAL;
    }
    cur = vfs_seek(fd, 0, 1 /* SEEK_CUR */);
    if (cur < 0) {
        return cur == -EINVAL ? -ESPIPE : cur;
    }
    r = vfs_seek(fd, off, 0 /* SEEK_SET */);
    if (r < 0) {
        return r;
    }
    r = writing ? do_write(fd, ubuf, len) : do_read(fd, ubuf, len);
    vfs_seek(fd, cur, 0);
    return r;
}

static int64_t sys_pread64(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                           uint64_t off, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a5; (void)a6;
    return do_pio((int)fd, ubuf, len, (int64_t)off, 0);
}

static int64_t sys_pwrite64(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                            uint64_t off, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a5; (void)a6;
    return do_pio((int)fd, ubuf, len, (int64_t)off, 1);
}

/* ধাপ ১৬ক: fcntl — F_DUPFD, F_GETFD/SETFD, F_GETFL/SETFL, আর রেকর্ড-লক (F_GETLK/
 * SETLK/SETLKW: rozOS-এ একজনই লেখে বলে লক সবসময় পাওয়া যায় — sqlite ইত্যাদির জন্য) */
#define K_F_DUPFD   0
#define K_F_GETFD   1
#define K_F_SETFD   2
#define K_F_GETFL   3
#define K_F_SETFL   4
#define K_F_GETLK   5
#define K_F_SETLK   6
#define K_F_SETLKW  7
#define K_F_DUPFD_CLOEXEC 1030

static int64_t sys_fcntl(struct interrupt_frame *f, uint64_t fd, uint64_t cmd, uint64_t arg,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    int fl = vfs_fd_flags((int)fd);
    (void)f; (void)a4; (void)a5; (void)a6;
    if (fl < 0) {
        return fl;
    }
    switch ((int)cmd) {
    case K_F_DUPFD:
    case K_F_DUPFD_CLOEXEC: {
        /* arg-এর সমান বা বড় সবচেয়ে ছোট খালি fd: dup করে করে খোঁজা, মাঝেরগুলো বন্ধ */
        int tmp[64], nt = 0, nfd, i;
        if ((int64_t)arg < 0) return -EINVAL;
        for (;;) {
            nfd = vfs_dup((int)fd);
            if (nfd < 0 || nfd >= (int)arg || nt == 64) break;
            tmp[nt++] = nfd;
        }
        for (i = 0; i < nt; i++) vfs_close(tmp[i]);
        if (nfd >= 0 && nfd < (int)arg) {
            vfs_close(nfd);
            return -EMFILE;
        }
        return nfd;
    }
    case K_F_GETFD:
        return 0;
    case K_F_SETFD:
        return 0;                                   /* FD_CLOEXEC এখনো উপেক্ষা */
    case K_F_GETFL:
        return fl;
    case K_F_SETFL:
        return 0;                                   /* O_NONBLOCK/O_APPEND বদল উপেক্ষা */
    case K_F_GETLK: {
        int16_t unlck = 2;                          /* F_UNLCK: কেউ লক ধরে নেই */
        return arg ? copy_to_user((void *)(uintptr_t)arg, &unlck, sizeof(unlck)) : -EFAULT;
    }
    case K_F_SETLK:
    case K_F_SETLKW:
        return 0;
    default:
        return -EINVAL;
    }
}

/* ধাপ ১৬ক: fsync/fdatasync — লেখা সরাসরি block layer-এ যায়, তাই শুধু fd যাচাই */
static int64_t sys_fsync(struct interrupt_frame *f, uint64_t fd, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    int fl = vfs_fd_flags((int)fd);
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return fl < 0 ? fl : 0;
}

static int64_t sys_open(struct interrupt_frame *f, uint64_t upath, uint64_t flags, uint64_t mode,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    int err = user_path(upath, path);
    (void)f; (void)mode; (void)a4; (void)a5; (void)a6;
    if (err < 0) {
        return err;
    }
    /* O_CLOEXEC (0x80000), O_NOCTTY, O_LARGEFILE ইত্যাদি এখন উপেক্ষা */
    return vfs_open(path, (int)(flags & (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_DIRECTORY)));
}

static int64_t sys_close(struct interrupt_frame *f, uint64_t fd, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return vfs_close((int)fd);
}

static int64_t sys_lseek(struct interrupt_frame *f, uint64_t fd, uint64_t off, uint64_t whence,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a4; (void)a5; (void)a6;
    return vfs_seek((int)fd, (int64_t)off, (int)whence);
}

static int64_t sys_dup(struct interrupt_frame *f, uint64_t fd, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return vfs_dup((int)fd);
}

static int64_t sys_dup2(struct interrupt_frame *f, uint64_t oldfd, uint64_t newfd, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    return vfs_dup2((int)oldfd, (int)newfd);
}

/* ---------- ধাপ ১৩: pipe, poll ---------- */

static int64_t sys_pipe2(struct interrupt_frame *f, uint64_t ufds, uint64_t flags, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    int fds[2], r;
    (void)f; (void)flags; (void)a3; (void)a4; (void)a5; (void)a6;   /* O_CLOEXEC/O_NONBLOCK এখনো নয় */
    r = vfs_pipe(fds);
    if (r < 0) return r;
    if (copy_to_user((void *)(uintptr_t)ufds, fds, sizeof(fds)) < 0) {
        vfs_close(fds[0]);
        vfs_close(fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_pipe(struct interrupt_frame *f, uint64_t ufds, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a6;
    return sys_pipe2(f, ufds, 0, a3, a4, a5, 0);
}

/* ধাপ ১৪ */
/* ---------- ধাপ ১৫ক: কার্নেল মডিউল ---------- */
static int64_t sys_init_module(struct interrupt_frame *f, uint64_t uimg, uint64_t len, uint64_t uname,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    char name[32] = "", err[160] = "";
    void *img;
    int r;
    (void)f; (void)a4; (void)a5; (void)a6;
    if (len == 0 || len > 16 * 1024 * 1024) return -EINVAL;
    if (uname && copy_string_from_user(name, (const char *)(uintptr_t)uname, sizeof(name)) < 0) return -EFAULT;
    img = kmalloc(len);
    if (!img) return -ENOMEM;
    if (copy_from_user(img, (const void *)(uintptr_t)uimg, len) < 0) { kfree(img); return -EFAULT; }
    r = module_load(img, len, name[0] ? name : NULL, err, sizeof(err));
    kfree(img);
    if (r < 0 && err[0]) kprintf("[module] %s: %s\n", name[0] ? name : "?", err);
    return r;
}

static int64_t sys_delete_module(struct interrupt_frame *f, uint64_t uname, uint64_t flags, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    char name[32];
    (void)f; (void)flags; (void)a3; (void)a4; (void)a5; (void)a6;
    if (copy_string_from_user(name, (const char *)(uintptr_t)uname, sizeof(name)) < 0) return -EFAULT;
    return module_unload(name);
}

static int64_t sys_modlist(struct interrupt_frame *f, uint64_t ubuf, uint64_t len, uint64_t what,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    char *k;
    int64_t n;
    (void)f; (void)a4; (void)a5; (void)a6;
    if (len == 0) return 0;
    if (len > 65536) len = 65536;
    k = (char *)kmalloc(len);
    if (!k) return -ENOMEM;
    n = module_list(k, len, what == 1);
    if (copy_to_user((void *)(uintptr_t)ubuf, k, (size_t)n + 1) < 0) n = -EFAULT;
    kfree(k);
    return n;
}

/* ---------- ধাপ ১৫ক: mount / umount ---------- */
static int64_t sys_mount(struct interrupt_frame *f, uint64_t usrc, uint64_t utgt, uint64_t utype,
                         uint64_t flags, uint64_t a5, uint64_t a6)
{
    char src[64], tgt[VFS_PATH_MAX], type[16] = "auto";
    const char *dev, *fs;
    struct block_device *bd;
    int err;
    (void)f; (void)flags; (void)a5; (void)a6;
    if (copy_string_from_user(src, (const char *)(uintptr_t)usrc, sizeof(src)) < 0) return -EFAULT;
    err = user_path(utgt, tgt);
    if (err < 0) return err;
    if (utype && copy_string_from_user(type, (const char *)(uintptr_t)utype, sizeof(type)) < 0) return -EFAULT;
    if (!strcmp(type, "tmpfs")) return vfs_mount(tgt, "tmpfs", NULL);
    dev = !strncmp(src, "/dev/", 5) ? src + 5 : src;
    bd = block_find(dev);
    if (!bd) return -ENODEV;
    fs = type;
    if (!strcmp(type, "auto") || !type[0]) {
        const char *p = fs_probe_name(bd);
        fs = !strcmp(p, "ext2") ? "ext2" : !strncmp(p, "fat", 3) ? "fat" : NULL;
        if (!fs) return -EINVAL;                 /* চেনা ফাইলসিস্টেম নেই */
    } else if (!strncmp(type, "vfat", 4) || !strncmp(type, "fat", 3)) {
        fs = "fat";
    }
    {
        size_t i;                                /* একই ডিভাইস দুবার নয় */
        for (i = 0; i < vfs_mount_count(); i++) {
            const struct mount *m = vfs_mount_at(i);
            if (m && !strcmp(m->source, dev)) return -EBUSY;
        }
    }
    {
        int r = vfs_mount(tgt, fs, dev);
        if (r == 0 && block_is_read_only(bd))
            kprintf("[mount] /dev/%s is read-only (a protected disk - boot option diskwrite allows writing)\n", dev);
        return r;
    }
}

static int64_t sys_umount2(struct interrupt_frame *f, uint64_t utgt, uint64_t flags, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    char tgt[VFS_PATH_MAX];
    int err = user_path(utgt, tgt);
    (void)f; (void)flags; (void)a3; (void)a4; (void)a5; (void)a6;
    return err < 0 ? err : vfs_umount(tgt);
}

/* ================= ধাপ ১৯চ: statfs (137) — ডিস্কের মাপ/খালি জায়গা ================= */

struct linux_statfs {
    int64_t  f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen, f_frsize, f_flags, f_spare[4];
};

static int64_t sys_statfs(struct interrupt_frame *f, uint64_t upath, uint64_t ubuf, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    struct vfs_statfs sf;
    struct linux_statfs out;
    const char *fsname = "";
    size_t i, best = 0;
    int err = user_path(upath, path);
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (err < 0) return err;
    err = vfs_statfs(path, &sf);
    if (err < 0) return err;
    for (i = 0; i < vfs_mount_count(); i++) {          /* path যে mount-এ (সবচেয়ে লম্বা মিল) */
        const struct mount *m = vfs_mount_at(i);
        size_t l = strlen(m->path);
        if (l >= best && !strncmp(path, m->path, l) && (l == 1 || path[l] == '/' || path[l] == '\0')) {
            best = l;
            fsname = m->fs->name;
        }
    }
    memset(&out, 0, sizeof(out));
    out.f_type = !strcmp(fsname, "ext2") ? 0xEF53 : !strcmp(fsname, "fat") ? 0x4d44 :
                 !strcmp(fsname, "tmpfs") ? 0x01021994 : !strcmp(fsname, "devfs") ? 0x1373 : 0;
    out.f_bsize = out.f_frsize = (int64_t)sf.block_size;
    out.f_blocks = sf.total_blocks;
    out.f_bfree = out.f_bavail = sf.free_blocks;
    out.f_files = sf.total_inodes;
    out.f_ffree = sf.free_inodes;
    out.f_namelen = 255;
    out.f_flags = sf.read_only ? 1 : 0;                /* ST_RDONLY */
    return copy_to_user((void *)(uintptr_t)ubuf, &out, sizeof(out)) < 0 ? -EFAULT : 0;
}

static int64_t sys_mounts(struct interrupt_frame *f, uint64_t ubuf, uint64_t len, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    char *k;
    size_t i, o = 0;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (len == 0) return 0;
    if (len > 16384) len = 16384;
    k = (char *)kzalloc(len);
    if (!k) return -ENOMEM;
    for (i = 0; i < vfs_mount_count() && o + 1 < len; i++) {
        const struct mount *m = vfs_mount_at(i);
        o += (size_t)ksnprintf(k + o, len - o, "%s %s %s\n", m->source[0] ? m->source : "-", m->path, m->fs->name);
    }
    if (o >= len) o = len - 1;
    if (copy_to_user((void *)(uintptr_t)ubuf, k, o + 1) < 0) o = (size_t)-EFAULT;
    kfree(k);
    return (int64_t)o;
}

/* ধাপ ১৫খ: lsusb */
static int64_t sys_usblist(struct interrupt_frame *f, uint64_t ubuf, uint64_t len, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    char *k;
    int64_t n;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (len == 0) return 0;
    if (len > 16384) len = 16384;
    k = (char *)kzalloc(len);
    if (!k) return -ENOMEM;
    n = usb_list(k, len);
    if (copy_to_user((void *)(uintptr_t)ubuf, k, (size_t)n + 1) < 0) n = -EFAULT;
    kfree(k);
    return n;
}

static int64_t sys_openpty(struct interrupt_frame *f, uint64_t ufds, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    int fds[2], r;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    r = pty_open(fds);
    if (r < 0) return r;
    if (copy_to_user((void *)(uintptr_t)ufds, fds, sizeof(fds)) < 0) {
        vfs_close(fds[0]);
        vfs_close(fds[1]);
        return -EFAULT;
    }
    return 0;
}

struct k_pollfd {
    int32_t fd;
    int16_t events, revents;
};

#define POLL_MAX_FDS 256

/* poll(fds, nfds, timeout_ms): timeout < 0 = চিরকাল, 0 = শুধু দেখা।
 * অপেক্ষা sched_relax() দিয়ে (devfs/pipe-এর মতো), প্রতিবার সব fd আবার দেখা। */
/* ধাপ ১৬ক: poll-এর অপেক্ষার অংশ আলাদা (select-ও ব্যবহার করে) — ফেরত তৈরি fd-এর
 * সংখ্যা বা -EINTR; k[i].revents ভরে দেয় */
static int poll_wait(struct k_pollfd *k, size_t nfds, int64_t tmo)
{
    uint64_t end = 0;
    int ready;
    size_t i;
    if (tmo > 0) end = timer_uptime_ms() + (uint64_t)tmo;
    for (;;) {
        ready = 0;
        for (i = 0; i < nfds; i++) {
            int ev, want;
            k[i].revents = 0;
            if (k[i].fd < 0) continue;
            want = (uint16_t)k[i].events | VFS_POLLERR | VFS_POLLHUP | VFS_POLLNVAL;
            ev = vfs_poll_fd(k[i].fd, k[i].events);
            k[i].revents = (int16_t)(ev & want);
            if (k[i].revents) ready++;
        }
        if (ready || tmo == 0 || (tmo > 0 && timer_uptime_ms() >= end)) break;
        cpu_cli();
        if (sched_should_abort()) {
            cpu_sti();
            return -EINTR;
        }
        sched_relax();                                  /* IF = 1 করে ফেরে */
    }
    return ready;
}

static int64_t sys_poll(struct interrupt_frame *f, uint64_t ufds, uint64_t nfds, uint64_t timeout,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct k_pollfd *k;
    int64_t tmo = (int64_t)(int32_t)timeout;
    int ready;
    size_t bytes;
    (void)f; (void)a4; (void)a5; (void)a6;
    if (nfds > POLL_MAX_FDS) return -EINVAL;
    bytes = nfds * sizeof(*k);
    k = (struct k_pollfd *)kmalloc(bytes ? bytes : 1);
    if (!k) return -ENOMEM;
    if (bytes && copy_from_user(k, (const void *)(uintptr_t)ufds, bytes) < 0) {
        kfree(k);
        return -EFAULT;
    }
    ready = poll_wait(k, nfds, tmo);
    if (ready < 0) {
        kfree(k);
        return ready;
    }
    if (bytes && copy_to_user((void *)(uintptr_t)ufds, k, bytes) < 0) {
        kfree(k);
        return -EFAULT;
    }
    kfree(k);
    return ready;
}

/* ধাপ ১৬ক: select(nfds, read, write, except, timeval) — fd_set (1024 বিট) থেকে
 * pollfd-এর তালিকা বানিয়ে poll_wait; ফেরার সময় শুধু তৈরি fd-গুলোর বিট থাকে */
#define K_FD_SETSIZE 1024
#define K_POLLIN   0x001
#define K_POLLPRI  0x002
#define K_POLLOUT  0x004

static int64_t sys_select(struct interrupt_frame *f, uint64_t nfds, uint64_t urd, uint64_t uwr,
                          uint64_t uex, uint64_t utv, uint64_t a6)
{
    uint64_t rd[K_FD_SETSIZE / 64], wr[K_FD_SETSIZE / 64], ex[K_FD_SETSIZE / 64];
    struct k_pollfd *k;
    int64_t tmo = -1;
    size_t setbytes, n = 0, i;
    int ready, count = 0;
    (void)f; (void)a6;
    if ((int64_t)nfds < 0 || nfds > K_FD_SETSIZE) return -EINVAL;
    setbytes = ((nfds + 63) / 64) * 8;
    memset(rd, 0, sizeof(rd));
    memset(wr, 0, sizeof(wr));
    memset(ex, 0, sizeof(ex));
    if ((urd && copy_from_user(rd, (const void *)(uintptr_t)urd, setbytes) < 0) ||
        (uwr && copy_from_user(wr, (const void *)(uintptr_t)uwr, setbytes) < 0) ||
        (uex && copy_from_user(ex, (const void *)(uintptr_t)uex, setbytes) < 0)) {
        return -EFAULT;
    }
    if (utv) {
        int64_t tv[2];
        if (copy_from_user(tv, (const void *)(uintptr_t)utv, sizeof(tv)) < 0) return -EFAULT;
        if (tv[0] < 0 || tv[1] < 0) return -EINVAL;
        tmo = tv[0] * 1000 + tv[1] / 1000;
        if (tmo == 0 && tv[1] > 0) tmo = 1;
    }
    k = (struct k_pollfd *)kmalloc(nfds ? nfds * sizeof(*k) : sizeof(*k));
    if (!k) return -ENOMEM;
    for (i = 0; i < nfds; i++) {
        int ev = 0;
        uint64_t bit = 1ULL << (i % 64);
        if (rd[i / 64] & bit) ev |= K_POLLIN;
        if (wr[i / 64] & bit) ev |= K_POLLOUT;
        if (ex[i / 64] & bit) ev |= K_POLLPRI;
        if (!ev) continue;
        k[n].fd = (int32_t)i;
        k[n].events = (int16_t)ev;
        k[n].revents = 0;
        n++;
    }
    ready = poll_wait(k, n, tmo);
    if (ready < 0) {
        kfree(k);
        return ready;
    }
    memset(rd, 0, setbytes);
    memset(wr, 0, setbytes);
    memset(ex, 0, setbytes);
    for (i = 0; i < n; i++) {
        int fd = k[i].fd, re = k[i].revents;
        uint64_t bit = 1ULL << (fd % 64);
        if (re & VFS_POLLNVAL) {
            kfree(k);
            return -EBADF;
        }
        if ((k[i].events & K_POLLIN) && (re & (K_POLLIN | VFS_POLLHUP | VFS_POLLERR))) {
            rd[fd / 64] |= bit; count++;
        }
        if ((k[i].events & K_POLLOUT) && (re & (K_POLLOUT | VFS_POLLERR))) {
            wr[fd / 64] |= bit; count++;
        }
        if ((k[i].events & K_POLLPRI) && (re & K_POLLPRI)) {
            ex[fd / 64] |= bit; count++;
        }
    }
    kfree(k);
    if ((urd && copy_to_user((void *)(uintptr_t)urd, rd, setbytes) < 0) ||
        (uwr && copy_to_user((void *)(uintptr_t)uwr, wr, setbytes) < 0) ||
        (uex && copy_to_user((void *)(uintptr_t)uex, ex, setbytes) < 0)) {
        return -EFAULT;
    }
    return count;
}

static int fill_linux_stat(const struct vfs_stat *st, uint64_t ust)
{
    struct linux_stat ls;
    memset(&ls, 0, sizeof(ls));
    ls.st_dev = 1;
    ls.st_ino = st->inode;
    ls.st_nlink = st->nlink;
    ls.st_mode = (st->mode & 07777) |
                 (st->type == VNODE_DIR ? S_IFDIR : st->type == VNODE_CHARDEV ? S_IFCHR :
                  st->type == VNODE_SOCKET ? S_IFSOCK : st->type == VNODE_FIFO ? S_IFIFO : S_IFREG);
    ls.st_size = (int64_t)st->size;
    ls.st_blksize = 4096;
    ls.st_blocks = (int64_t)((st->size + 511) / 512);
    ls.st_atime = ls.st_mtime = ls.st_ctime = st->mtime;
    return copy_to_user((void *)(uintptr_t)ust, &ls, sizeof(ls));
}

static int64_t sys_stat(struct interrupt_frame *f, uint64_t upath, uint64_t ust, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    int err = user_path(upath, path);
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (err < 0) {
        return err;
    }
    err = vfs_stat(path, &st);
    return err < 0 ? err : fill_linux_stat(&st, ust);
}

static int64_t sys_fstat(struct interrupt_frame *f, uint64_t fd, uint64_t ust, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vfs_stat st;
    int err = vfs_fstat((int)fd, &st);
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    return err < 0 ? err : fill_linux_stat(&st, ust);
}

/* struct linux_dirent64 { u64 ino; s64 off; u16 reclen; u8 type; char name[]; } */
static int64_t sys_getdents64(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vfs_dirent de;
    uint8_t rec[8 + 8 + 2 + 1 + VFS_NAME_MAX + 1 + 8];
    uint64_t used = 0;
    (void)f; (void)a4; (void)a5; (void)a6;

    for (;;) {
        int64_t pos = vfs_seek((int)fd, 0, SEEK_CUR);
        size_t nlen, reclen;
        uint64_t v64;
        uint16_t v16;
        int r;

        if (pos < 0) {
            return used ? (int64_t)used : pos;
        }
        r = vfs_readdir((int)fd, &de);
        if (r < 0) {
            return used ? (int64_t)used : r;
        }
        if (r == 0) {
            return (int64_t)used;
        }
        nlen = strlen(de.name);
        reclen = (19 + nlen + 1 + 7) & ~(size_t)7;
        if (used + reclen > len) {
            vfs_seek((int)fd, pos, SEEK_SET);   /* পরের ডাকে আবার এই এন্ট্রি */
            return used ? (int64_t)used : -EINVAL;
        }
        memset(rec, 0, reclen);
        v64 = de.inode;                 memcpy(rec, &v64, 8);
        v64 = (uint64_t)pos + 1;        memcpy(rec + 8, &v64, 8);
        v16 = (uint16_t)reclen;         memcpy(rec + 16, &v16, 2);
        rec[18] = de.type == VNODE_DIR ? DT_DIR : de.type == VNODE_CHARDEV ? DT_CHR : DT_REG;
        memcpy(rec + 19, de.name, nlen + 1);
        if (copy_to_user((void *)(uintptr_t)(ubuf + used), rec, reclen) < 0) {
            vfs_seek((int)fd, pos, SEEK_SET);
            return used ? (int64_t)used : -EFAULT;
        }
        used += reclen;
    }
}

/* "/a/./b/../c/" → "/a/c" */
static void normalize_path(const char *in, char *out, size_t size)
{
    size_t o = 0;
    const char *p = in;

    out[0] = '\0';
    while (*p) {
        const char *s;
        size_t n;
        while (*p == '/') p++;
        s = p;
        while (*p && *p != '/') p++;
        n = (size_t)(p - s);
        if (n == 0 || (n == 1 && s[0] == '.')) {
            continue;
        }
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = '\0';
            continue;
        }
        if (o + 1 + n + 1 > size) {
            break;
        }
        out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
        out[o] = '\0';
    }
    if (o == 0) {
        strcpy(out, "/");
    }
}

static int64_t sys_chdir(struct interrupt_frame *f, uint64_t upath, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    struct process *p = proc_current();
    int err = user_path(upath, path);
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (err < 0) {
        return err;
    }
    err = vfs_stat(path, &st);
    if (err < 0) {
        return err;
    }
    if (st.type != VNODE_DIR) {
        return -ENOTDIR;
    }
    normalize_path(path, p->cwd, sizeof(p->cwd));
    return 0;
}

static int64_t sys_getcwd(struct interrupt_frame *f, uint64_t ubuf, uint64_t size, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    size_t n = strlen(p->cwd) + 1;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (size < n) {
        return -ERANGE;
    }
    if (copy_to_user((void *)(uintptr_t)ubuf, p->cwd, n) < 0) {
        return -EFAULT;
    }
    return (int64_t)n;
}

static int64_t sys_mkdir(struct interrupt_frame *f, uint64_t upath, uint64_t mode, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    int err = user_path(upath, path);
    (void)f; (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    return err < 0 ? err : vfs_mkdir(path);
}

static int64_t sys_rmdir(struct interrupt_frame *f, uint64_t upath, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    int err = user_path(upath, path);
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return err < 0 ? err : vfs_rmdir(path);
}

static int64_t sys_unlink(struct interrupt_frame *f, uint64_t upath, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    int err = user_path(upath, path);
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return err < 0 ? err : vfs_unlink(path);
}

/* ধাপ ১৫ক: rename(old, new) — একই ফাইলসিস্টেমে; না হলে -EXDEV (mv তখন কপি করে) */
static int64_t sys_rename(struct interrupt_frame *f, uint64_t uold, uint64_t unew, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    int err = user_path(uold, a);
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (err < 0) return err;
    err = user_path(unew, b);
    return err < 0 ? err : vfs_rename(a, b);
}

static int64_t sys_truncate(struct interrupt_frame *f, uint64_t upath, uint64_t len, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    char path[VFS_PATH_MAX];
    int err = user_path(upath, path);
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    return err < 0 ? err : vfs_truncate(path, len);
}

/* টার্মিনাল-সংক্রান্ত সামান্য সাপোর্ট: libc-র isatty() আর জানালার মাপ */
static int64_t sys_ioctl(struct interrupt_frame *f, uint64_t fd, uint64_t req, uint64_t arg,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vfs_stat st;
    int err = vfs_fstat((int)fd, &st);
    (void)f; (void)a4; (void)a5; (void)a6;

    if (err < 0) {
        return err;
    }
    if (st.type != VNODE_CHARDEV) {
        return -ENOTTY;
    }
    {
        /* ধাপ ১৩: /dev/fb0-এর নিজস্ব ioctl; ধাপ ১৪: PTY, কনসোলের pgrp/termios */
        struct vnode *vn = vfs_fd_vnode((int)fd);
        if (vn && fbdev_is_vnode(vn)) {
            int r = fbdev_ioctl(vn, req, arg, copy_to_user);
            vnode_unref(vn);
            return r;
        }
        if (vn && pty_is_vnode(vn)) {
            int r = pty_ioctl(vn, req, arg, copy_from_user, copy_to_user);
            vnode_unref(vn);
            return r;
        }
        if (vn) {
            const char *nm = devfs_vnode_name(vn);
            vnode_unref(vn);
            if (nm && (!strcmp(nm, "console") || !strcmp(nm, "serial0"))) {
                int r = ttyctl_ioctl(!strcmp(nm, "console") ? console_ttyctl() : serial_ttyctl(),
                                     req, arg, copy_from_user, copy_to_user);
                if (r != -ENOTTY) return r;
            }
        }
    }
    if (req == TIOCGWINSZ) {
        uint16_t ws[4];
        ws[0] = console_rows ? console_rows : (tty_rows() ? (uint16_t)tty_rows() : 24);
        ws[1] = console_cols ? console_cols : (tty_cols() ? (uint16_t)tty_cols() : 80);
        ws[2] = ws[3] = 0;
        return copy_to_user((void *)(uintptr_t)arg, ws, sizeof(ws));
    }
    if (req == TIOCSWINSZ) {
        uint16_t ws[4];
        if (copy_from_user(ws, (const void *)(uintptr_t)arg, sizeof(ws)) < 0) {
            return -EFAULT;
        }
        if (ws[0] > 500 || ws[1] > 500) {
            return -EINVAL;
        }
        {
            int changed = console_rows != ws[0] || console_cols != ws[1];
            console_rows = ws[0];                /* 0 = আবার স্ক্রিনের মাপ */
            console_cols = ws[1];
            console_manual = ws[0] != 0;
            /* ধাপ ১৬খ-১: সামনের কাজকে SIGWINCH */
            if (changed && console_ttyctl()->fg_pgid > 0) proc_kill_group(console_ttyctl()->fg_pgid, SIGWINCH);
        }
        return 0;
    }
    if (req == TIOCROZSETFONT) {
        struct rozos_font_req fr;
        uint8_t *buf;
        size_t bytes;
        int r;
        if (copy_from_user(&fr, (const void *)(uintptr_t)arg, sizeof(fr)) < 0) return -EFAULT;
        if (fr.flags & 1) {
            font_reset();
            tty_font_changed();
            return 0;
        }
        if (fr.width == 0 || fr.width > 16 || fr.height == 0 || fr.height > 32) return -EINVAL;
        bytes = (size_t)256 * fr.height * ((fr.width + 7) / 8);
        buf = (uint8_t *)kmalloc(bytes);
        if (!buf) return -ENOMEM;
        if (copy_from_user(buf, (const void *)(uintptr_t)fr.data, bytes) < 0) {
            kfree(buf);
            return -EFAULT;
        }
        r = font_set(buf, fr.width, fr.height);
        kfree(buf);
        if (r < 0) return -EINVAL;
        tty_font_changed();
        console_rows = console_cols = 0;     /* টেক্সট মাপ বদলেছে */
        console_manual = 0;
        return 0;
    }
    if (req == TIOCROZDETECT || req == TIOCROZDETECT_FORCE) {
        uint16_t ws[4];
        uint32_t r, c;
        if (req == TIOCROZDETECT_FORCE || !console_manual) {
            if (console_query_size(&r, &c) == 0) {
                /* স্ক্রিন (ফ্রেমবাফার) থাকলে দুটোর ছোটটা — দুই জায়গাতেই ঠিক দেখায় */
                if (tty_rows() && r > tty_rows()) r = tty_rows();
                if (tty_cols() && c > tty_cols()) c = tty_cols();
                console_rows = (uint16_t)r;
                console_cols = (uint16_t)c;
                console_manual = 0;
            } else if (req == TIOCROZDETECT_FORCE) {
                console_rows = console_cols = 0;  /* উত্তর নেই: স্ক্রিনের মাপ */
                console_manual = 0;
            }
        }
        ws[0] = console_rows ? console_rows : (tty_rows() ? (uint16_t)tty_rows() : 24);
        ws[1] = console_cols ? console_cols : (tty_cols() ? (uint16_t)tty_cols() : 80);
        ws[2] = ws[3] = 0;
        return arg ? copy_to_user((void *)(uintptr_t)arg, ws, sizeof(ws)) : 0;
    }
    if (req == TCGETS) {
        uint8_t termios[60];
        memset(termios, 0, sizeof(termios));
        return copy_to_user((void *)(uintptr_t)arg, termios, sizeof(termios));
    }
    return -ENOTTY;
}

void sys_fs_register(void)
{
    syscall_register(SYS_read, sys_read, "read");
    syscall_register(SYS_write, sys_write, "write");
    syscall_register(SYS_writev, sys_writev, "writev");
    syscall_register(SYS_open, sys_open, "open");
    syscall_register(SYS_close, sys_close, "close");
    syscall_register(SYS_lseek, sys_lseek, "lseek");
    syscall_register(SYS_dup, sys_dup, "dup");
    syscall_register(SYS_dup2, sys_dup2, "dup2");
    syscall_register(SYS_stat, sys_stat, "stat");
    syscall_register(SYS_fstat, sys_fstat, "fstat");
    syscall_register(SYS_getdents64, sys_getdents64, "getdents64");
    syscall_register(SYS_chdir, sys_chdir, "chdir");
    syscall_register(SYS_getcwd, sys_getcwd, "getcwd");
    syscall_register(SYS_mkdir, sys_mkdir, "mkdir");
    syscall_register(SYS_rmdir, sys_rmdir, "rmdir");
    syscall_register(SYS_unlink, sys_unlink, "unlink");
    syscall_register(SYS_rename, sys_rename, "rename");     /* ধাপ ১৫ক */
    syscall_register(SYS_truncate, sys_truncate, "truncate");
    syscall_register(SYS_ioctl, sys_ioctl, "ioctl");
    syscall_register(SYS_pipe, sys_pipe, "pipe");          /* ধাপ ১৩ */
    syscall_register(SYS_pipe2, sys_pipe2, "pipe2");
    syscall_register(SYS_poll, sys_poll, "poll");
    syscall_register(SYS_rozos_openpty, sys_openpty, "openpty");   /* ধাপ ১৪ */
    syscall_register(SYS_init_module, sys_init_module, "init_module");       /* ধাপ ১৫ক */
    syscall_register(SYS_delete_module, sys_delete_module, "delete_module");
    syscall_register(SYS_rozos_modlist, sys_modlist, "modlist");
    syscall_register(SYS_mount, sys_mount, "mount");
    syscall_register(SYS_umount2, sys_umount2, "umount2");
    syscall_register(SYS_rozos_mounts, sys_mounts, "mounts");
    syscall_register(SYS_statfs, sys_statfs, "statfs");                 /* ধাপ ১৯চ */
    syscall_register(SYS_rozos_usblist, sys_usblist, "usblist");      /* ধাপ ১৫খ */
    syscall_register(SYS_readv, sys_readv, "readv");                  /* ধাপ ১৬ক */
    syscall_register(SYS_pread64, sys_pread64, "pread64");
    syscall_register(SYS_pwrite64, sys_pwrite64, "pwrite64");
    syscall_register(SYS_select, sys_select, "select");
    syscall_register(SYS_fcntl, sys_fcntl, "fcntl");
    syscall_register(SYS_fsync, sys_fsync, "fsync");
    syscall_register(SYS_fdatasync, sys_fsync, "fdatasync");
}
