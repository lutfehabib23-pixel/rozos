/* rozOS — devfs (rozos/devfs.h দেখুন)
 *
 * একটাই instance: root ডিরেক্টরি আর নিবন্ধিত ডিভাইসের সমতল তালিকা।
 * ডিভাইস vnode কখনো মুক্ত হয় না (nlink = 1 সবসময়)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/devfs.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/sched.h>
#include <rozos/string.h>
#include <rozos/console.h>
#include <rozos/serial.h>
#include <rozos/keyboard.h>
#include <rozos/timer.h>
#include <arch/x86_64/cpu.h>

struct devfs_device {
    struct vnode vn;
    char name[32];
    devfs_read_fn read;
    devfs_write_fn write;
    void *ctx;
};

static struct devfs_device devices[DEVFS_MAX_DEVICES];
static size_t device_count;
static struct vnode root;
static int root_ready;
static const struct vnode_ops dev_ops;
static const struct vnode_ops root_ops;

/* ---------- vnode অপারেশন ---------- */

static int64_t dev_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct devfs_device *d = (struct devfs_device *)vn;
    return d->read ? d->read(d->ctx, off, buf, len) : -EINVAL;
}

static int64_t dev_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct devfs_device *d = (struct devfs_device *)vn;
    return d->write ? d->write(d->ctx, off, buf, len) : -EINVAL;
}

static int root_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    size_t i;
    (void)dir;
    for (i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].name, name)) {
            vnode_ref(&devices[i].vn);
            *out = &devices[i].vn;
            return 0;
        }
    }
    return -ENOENT;
}

static int root_readdir(struct vnode *dir, uint64_t index, struct vfs_dirent *out)
{
    (void)dir;
    if (index >= device_count) {
        return 0;
    }
    strncpy(out->name, devices[index].name, VFS_NAME_MAX);
    out->name[VFS_NAME_MAX] = '\0';
    out->type = VNODE_CHARDEV;
    out->inode = devices[index].vn.inode;
    out->size = 0;
    return 1;
}

static const struct vnode_ops dev_ops = {
    dev_read, dev_write, NULL, NULL, NULL, NULL, NULL, NULL
};

/* create/unlink নেই — devfs-এ ফাইল বানানো বা মোছা যায় না */
static const struct vnode_ops root_ops = {
    NULL, NULL, NULL, root_lookup, NULL, NULL, root_readdir, NULL
};

int devfs_register(const char *name, devfs_read_fn read, devfs_write_fn write, void *ctx)
{
    struct devfs_device *d;
    if (device_count >= DEVFS_MAX_DEVICES) {
        return -ENOMEM;
    }
    if (strlen(name) >= sizeof(d->name)) {
        return -ENAMETOOLONG;
    }
    d = &devices[device_count];
    memset(d, 0, sizeof(*d));
    strcpy(d->name, name);
    d->read = read;
    d->write = write;
    d->ctx = ctx;
    d->vn.type = VNODE_CHARDEV;
    d->vn.mode = 0666;
    d->vn.inode = 1000 + device_count;
    d->vn.nlink = 1;
    d->vn.ops = &dev_ops;
    d->vn.fs_data = d;
    d->vn.mount = root.mount;
    device_count++;
    root.size = device_count;
    return 0;
}

static int devfs_mount(const void *arg, struct vnode **out)
{
    (void)arg;
    if (!root_ready) {
        return -ENODEV;
    }
    *out = &root;
    return 0;
}

static int devfs_statfs(struct vnode *r, struct vfs_statfs *out)
{
    (void)r;
    out->total_inodes = DEVFS_MAX_DEVICES;
    out->free_inodes = DEVFS_MAX_DEVICES - device_count;
    out->read_only = 1;
    out->source[0] = 'd'; out->source[1] = 'e'; out->source[2] = 'v'; out->source[3] = '\0';
    return 0;
}

const struct filesystem devfs_filesystem = { "devfs", devfs_mount, devfs_statfs };

/* ---------- ডিফল্ট ডিভাইস ---------- */

static int64_t null_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    (void)ctx; (void)off; (void)buf; (void)len;
    return 0;
}

static int64_t sink_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    (void)ctx; (void)off; (void)buf;
    return (int64_t)len;
}

static int64_t zero_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    (void)ctx; (void)off;
    memset(buf, 0, len);
    return (int64_t)len;
}

/* কিবোর্ড বা serial থেকে একটা কী — না থাকলে 0 */
static int try_any_key(int *key)
{
    char c;
    if (keyboard_try_getkey(key)) {
        return 1;
    }
    if (serial_try_getc(&c)) {
        *key = (c == '\r') ? '\n' : (c == 0x7F) ? '\b' : (unsigned char)c;
        return 1;
    }
    return 0;
}

/* ধাপ ৭: বিশেষ কী → ANSI/VT100 escape sequence (serial টার্মিনাল যেমন পাঠায়), যাতে
 * shell ও এডিটর কিবোর্ড আর serial দুটো থেকেই একইভাবে তীর-কী ইত্যাদি পায় */
static const char *key_sequence(int key)
{
    switch (key) {
    case KEY_UP:        return "\x1b[A";
    case KEY_DOWN:      return "\x1b[B";
    case KEY_RIGHT:     return "\x1b[C";
    case KEY_LEFT:      return "\x1b[D";
    case KEY_HOME:      return "\x1b[H";
    case KEY_END:       return "\x1b[F";
    case KEY_INSERT:    return "\x1b[2~";
    case KEY_DELETE:    return "\x1b[3~";
    case KEY_PAGE_UP:   return "\x1b[5~";
    case KEY_PAGE_DOWN: return "\x1b[6~";
    default:            return NULL;      /* F-কী এখনো নয় — /dev/kbd-তে পাওয়া যায় */
    }
}

/* আগের read-এ জায়গা না পাওয়া sequence-এর বাকি অংশ */
static char pending_seq[8];
static size_t pending_len, pending_pos;

/* অন্তত একটা অক্ষর না আসা পর্যন্ত অপেক্ষা, তারপর যা জমে আছে তা */
static int64_t console_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    char *out = (char *)buf;
    size_t n = 0;
    int key;
    (void)ctx; (void)off;

    if (len == 0) {
        return 0;
    }
    while (n == 0) {
        cpu_cli();
        while (n < len && pending_pos < pending_len) {
            out[n++] = pending_seq[pending_pos++];
        }
        while (n < len && try_any_key(&key)) {
            if (key < 0x100) {
                out[n++] = (char)key;
            } else {
                const char *seq = key_sequence(key);
                if (seq) {
                    pending_len = strlen(seq);
                    memcpy(pending_seq, seq, pending_len);
                    pending_pos = 0;
                    while (n < len && pending_pos < pending_len) {
                        out[n++] = pending_seq[pending_pos++];
                    }
                }
            }
        }
        if (n == 0) {
            if (sched_should_abort()) {       /* ধাপ ৬: প্রসেস kill হয়েছে */
                cpu_sti();
                return -EINTR;
            }
            sched_relax();
        } else {
            cpu_sti();
        }
    }
    return (int64_t)n;
}

static int64_t console_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    (void)ctx; (void)off;
    tty_write((const char *)buf, len);
    return (int64_t)len;
}

static int64_t serial_dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    char *out = (char *)buf;
    size_t n = 0;
    char c;
    (void)ctx; (void)off;

    if (len == 0 || !serial_is_ready()) {
        return 0;
    }
    while (n == 0) {
        while (n < len && serial_try_getc(&c)) {
            out[n++] = c;
        }
        if (n == 0) {
            if (sched_should_abort()) {
                return -EINTR;
            }
            timer_sleep_ms(1);           /* serial-এ এখনো ইন্টারাপ্ট নেই */
        }
    }
    return (int64_t)n;
}

static int64_t serial_dev_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    (void)ctx; (void)off;
    serial_write((const char *)buf, len);
    return (int64_t)len;
}

/* /dev/kbd: প্রতিটি কী int32 হিসেবে (ASCII বা KEY_*); len অন্তত 4 হতে হবে */
static int64_t kbd_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    int32_t *out = (int32_t *)buf;
    size_t n = 0, max = len / sizeof(int32_t);
    int key;
    (void)ctx; (void)off;

    if (max == 0) {
        return -EINVAL;
    }
    while (n == 0) {
        cpu_cli();
        while (n < max && keyboard_try_getkey(&key)) {
            out[n++] = key;
        }
        if (n == 0) {
            if (sched_should_abort()) {
                cpu_sti();
                return -EINTR;
            }
            sched_relax();
        } else {
            cpu_sti();
        }
    }
    return (int64_t)(n * sizeof(int32_t));
}

int devfs_init(void)
{
    /* ধাপ ৫: একবারই চলে, আর আগে নিবন্ধিত ডিভাইস (যদি থাকে) মোছে না */
    if (root_ready) {
        return 0;
    }
    memset(&root, 0, sizeof(root));
    root.type = VNODE_DIR;
    root.mode = 0755;
    root.inode = 1;
    root.nlink = 1;
    root.ops = &root_ops;
    root.size = device_count;
    root_ready = 1;

    devfs_register("null", null_read, sink_write, NULL);
    devfs_register("zero", zero_read, sink_write, NULL);
    devfs_register("console", console_read, console_write, NULL);
    devfs_register("serial0", serial_dev_read, serial_dev_write, NULL);
    devfs_register("kbd", kbd_read, NULL, NULL);
    return vfs_register_filesystem(&devfs_filesystem);
}
