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
#include <rozos/uaccess.h>
#include <rozos/heap.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/console.h>

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
#define S_IFDIR 0040000
#define S_IFREG 0100000

#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8

#define TCGETS     0x5401
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

/* ধাপ ৭: কনসোলের মাপ। 0 = স্ক্রিনের মাপ (ফ্রেমবাফার/VGA), স্ক্রিন না থাকলে 80x24।
 * serial টার্মিনালের মাপ আলাদা হলে `stty <rows> <cols>` (TIOCSWINSZ) দিয়ে বদলানো যায়। */
static uint16_t console_rows, console_cols;

static int user_path(uint64_t upath, char *out)
{
    char tmp[VFS_PATH_MAX];
    int64_t n = copy_string_from_user(tmp, (const char *)(uintptr_t)upath, sizeof(tmp));
    if (n < 0) {
        return (int)n;
    }
    return proc_abs_path(tmp, out, VFS_PATH_MAX);
}

static int64_t sys_read(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint8_t *kbuf;
    int64_t total = 0;
    (void)f; (void)a4; (void)a5; (void)a6;

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
        int64_t r = vfs_read((int)fd, kbuf, n);
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

static int fill_linux_stat(const struct vfs_stat *st, uint64_t ust)
{
    struct linux_stat ls;
    memset(&ls, 0, sizeof(ls));
    ls.st_dev = 1;
    ls.st_ino = st->inode;
    ls.st_nlink = st->nlink;
    ls.st_mode = (st->mode & 07777) |
                 (st->type == VNODE_DIR ? S_IFDIR : st->type == VNODE_CHARDEV ? S_IFCHR :
                  st->type == VNODE_SOCKET ? S_IFSOCK : S_IFREG);
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
        console_rows = ws[0];                    /* 0 = আবার স্ক্রিনের মাপ */
        console_cols = ws[1];
        return 0;
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
    syscall_register(SYS_truncate, sys_truncate, "truncate");
    syscall_register(SYS_ioctl, sys_ioctl, "ioctl");
}
