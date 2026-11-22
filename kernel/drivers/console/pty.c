/* rozOS — pseudo-terminal (PTY), ধাপ ১৪
 *
 * openpty() একজোড়া fd দেয়: master (টার্মিনাল-অ্যাপ, যেমন rozterm ধরে রাখে) আর slave
 * (তার ভেতরের shell-এর stdin/stdout/stderr)। slave দেখতে /dev/console-এর মতোই একটা
 * অক্ষর-ডিভাইস — rozsh, edit, make কোনো বদল ছাড়াই চলে; তাদের লেখা master-এ পড়া যায়,
 * আর master-এ লেখা (চাপা কী) slave-এর পড়ায় আসে।
 *
 *   master write → [in]  → slave read     (Ctrl+C এলে foreground গ্রুপে SIGINT, ttyctl)
 *   slave write  → [out] → master read
 *
 * বন্ধ হওয়া: master বন্ধ → slave-এর read 0 (EOF; shell শেষ হয়), write -EIO।
 * slave-এর সব fd বন্ধ → master-এর read 0 (টার্মিনাল-অ্যাপ জানে shell শেষ)।
 * মাপ (TIOCSWINSZ/TIOCGWINSZ, TIOCROZDETECT) আর pgrp/termios — যেকোনো মুখের ioctl-এ।
 * অপেক্ষা sched_relax() দিয়ে (pipe-এর মতো); বাফার বদলানো interrupt বন্ধ রেখে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/pty.h>
#include <rozos/ttyctl.h>
#include <rozos/vfs.h>
#include <rozos/heap.h>
#include <rozos/sched.h>
#include <rozos/proc.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

#define PTY_BUF 16384
#define PTY_MAX 64

struct ring {
    uint8_t *buf;
    size_t head, count;
};

struct pty {
    struct vnode master, slave;
    struct ring in, out;             /* in: master→slave, out: slave→master */
    int master_open, slave_open;
    struct ttyctl tc;
    int index;
};

static struct pty *ptys[PTY_MAX];
static const struct vnode_ops master_ops, slave_ops;

static size_t ring_put(struct ring *r, const uint8_t *src, size_t n)
{
    size_t done = 0;
    while (done < n && r->count < PTY_BUF) {
        size_t tail = (r->head + r->count) % PTY_BUF;
        size_t chunk = PTY_BUF - tail;
        if (chunk > PTY_BUF - r->count) chunk = PTY_BUF - r->count;
        if (chunk > n - done) chunk = n - done;
        memcpy(r->buf + tail, src + done, chunk);
        r->count += chunk;
        done += chunk;
    }
    return done;
}

static size_t ring_get(struct ring *r, uint8_t *dst, size_t n)
{
    size_t done = 0;
    while (done < n && r->count > 0) {
        size_t chunk = PTY_BUF - r->head;
        if (chunk > r->count) chunk = r->count;
        if (chunk > n - done) chunk = n - done;
        memcpy(dst + done, r->buf + r->head, chunk);
        r->head = (r->head + chunk) % PTY_BUF;
        r->count -= chunk;
        done += chunk;
    }
    return done;
}

static void maybe_free(struct pty *p)
{
    if (!p->master_open && !p->slave_open) {
        if (p->index >= 0 && p->index < PTY_MAX && ptys[p->index] == p) ptys[p->index] = NULL;
        kfree(p->in.buf);
        kfree(p->out.buf);
        kfree(p);
    }
}

/* পড়া: ডেটা থাকলে সাথে সাথে; না থাকলে অন্য মুখ খোলা থাকলে অপেক্ষা, নইলে 0 */
static int64_t do_read(struct pty *p, struct ring *r, const int *other_open, void *buf, size_t len)
{
    size_t n;
    if (len == 0) return 0;
    for (;;) {
        cpu_cli();
        if (r->count > 0) {
            n = ring_get(r, (uint8_t *)buf, len);
            cpu_sti();
            return (int64_t)n;
        }
        if (!*other_open) {
            cpu_sti();
            return 0;
        }
        if (sched_should_abort()) {
            cpu_sti();
            return -EINTR;
        }
        (void)p;
        sched_relax();
    }
}

static int64_t do_write(struct pty *p, struct ring *r, const int *other_open, const void *buf,
                        size_t len, int is_input)
{
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;
    if (len == 0) return 0;
    for (;;) {
        size_t n, i;
        cpu_cli();
        if (!*other_open) {
            cpu_sti();
            return done ? (int64_t)done : -EIO;
        }
        if (is_input) {
            /* বাইট ধরে ধরে: Ctrl+C signal হয়ে গেলে বাইটটা বাদ */
            n = 0;
            for (i = done; i < len && r->count < PTY_BUF; i++) {
                if (in[i] == 3 && ttyctl_input(&p->tc, 3)) { n++; continue; }
                ring_put(r, in + i, 1);
                n++;
            }
        } else {
            n = ring_put(r, in + done, len - done);
        }
        done += n;
        if (done == len) {
            cpu_sti();
            return (int64_t)done;
        }
        if (sched_should_abort()) {
            cpu_sti();
            return done ? (int64_t)done : -EINTR;
        }
        sched_relax();
    }
}

static int64_t master_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct pty *p = (struct pty *)vn->fs_data;
    (void)off;
    return do_read(p, &p->out, &p->slave_open, buf, len);
}

static int64_t master_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct pty *p = (struct pty *)vn->fs_data;
    (void)off;
    return do_write(p, &p->in, &p->slave_open, buf, len, 1);
}

static int64_t slave_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct pty *p = (struct pty *)vn->fs_data;
    (void)off;
    return do_read(p, &p->in, &p->master_open, buf, len);
}

static int64_t slave_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct pty *p = (struct pty *)vn->fs_data;
    (void)off;
    return do_write(p, &p->out, &p->master_open, buf, len, 0);
}

static int master_poll(struct vnode *vn, int events)
{
    struct pty *p = (struct pty *)vn->fs_data;
    int r = 0;
    (void)events;
    if (p->out.count > 0) r |= VFS_POLLIN;
    if (!p->slave_open) r |= VFS_POLLIN | VFS_POLLHUP;
    if (p->in.count < PTY_BUF) r |= VFS_POLLOUT;
    return r;
}

static int slave_poll(struct vnode *vn, int events)
{
    struct pty *p = (struct pty *)vn->fs_data;
    int r = 0;
    (void)events;
    if (p->in.count > 0) r |= VFS_POLLIN;
    if (!p->master_open) r |= VFS_POLLIN | VFS_POLLHUP;
    if (p->out.count < PTY_BUF) r |= VFS_POLLOUT;
    return r;
}

static void master_release(struct vnode *vn)
{
    struct pty *p = (struct pty *)vn->fs_data;
    uint64_t fl = cpu_irq_save();
    p->master_open = 0;
    /* টার্মিনাল বন্ধ: ভেতরের প্রোগ্রামগুলোকে জানানো (Unix-এর SIGHUP) */
    if (p->tc.fg_pgid > 0) ttyctl_hangup(&p->tc);
    maybe_free(p);
    cpu_irq_restore(fl);
}

static void slave_release(struct vnode *vn)
{
    struct pty *p = (struct pty *)vn->fs_data;
    uint64_t fl = cpu_irq_save();
    p->slave_open = 0;
    maybe_free(p);
    cpu_irq_restore(fl);
}

static const struct vnode_ops master_ops = {
    master_read, master_write, NULL, NULL, NULL, NULL, NULL, master_release, master_poll, NULL /* rename (ধাপ ১৫ক) */
};

static const struct vnode_ops slave_ops = {
    slave_read, slave_write, NULL, NULL, NULL, NULL, NULL, slave_release, slave_poll, NULL /* rename (ধাপ ১৫ক) */
};

static void init_end(struct pty *p, struct vnode *vn, const struct vnode_ops *ops, uint64_t ino)
{
    vn->type = VNODE_CHARDEV;            /* isatty, ioctl — কনসোলের মতো */
    vn->mode = 0620;
    vn->inode = ino;
    vn->refcount = 1;
    vn->nlink = 0;
    vn->ops = ops;
    vn->fs_data = p;
}

int pty_open(int fds[2])
{
    struct pty *p;
    int i, m, s;
    uint64_t fl = cpu_irq_save();
    for (i = 0; i < PTY_MAX && ptys[i]; i++) {
    }
    if (i == PTY_MAX) {
        cpu_irq_restore(fl);
        return -EAGAIN;
    }
    p = (struct pty *)kzalloc(sizeof(*p));
    if (!p) {
        cpu_irq_restore(fl);
        return -ENOMEM;
    }
    ptys[i] = p;
    p->index = i;
    cpu_irq_restore(fl);
    p->in.buf = (uint8_t *)kmalloc(PTY_BUF);
    p->out.buf = (uint8_t *)kmalloc(PTY_BUF);
    if (!p->in.buf || !p->out.buf) {
        maybe_free(p);
        return -ENOMEM;
    }
    ttyctl_init(&p->tc);
    p->master_open = p->slave_open = 1;
    init_end(p, &p->master, &master_ops, 0x50000000ULL + (uint64_t)i * 2);
    init_end(p, &p->slave, &slave_ops, 0x50000000ULL + (uint64_t)i * 2 + 1);
    m = vfs_install_vnode(&p->master, O_RDWR);
    if (m < 0) {
        p->master_open = p->slave_open = 0;
        maybe_free(p);
        return m;
    }
    s = vfs_install_vnode(&p->slave, O_RDWR);
    if (s < 0) {
        p->slave_open = 0;
        vfs_close(m);
        return s;
    }
    fds[0] = m;
    fds[1] = s;
    return 0;
}

int pty_is_vnode(struct vnode *vn)
{
    return vn && (vn->ops == &master_ops || vn->ops == &slave_ops);
}

int pty_ioctl(struct vnode *vn, uint64_t req, uint64_t arg,
              int (*copy_in)(void *, const void *, size_t),
              int (*copy_out)(void *, const void *, size_t))
{
    struct pty *p = (struct pty *)vn->fs_data;
    int r;
    if (req == TIOCGWINSZ || req == PTY_TIOCROZDETECT || req == PTY_TIOCROZDETECT_FORCE) {
        uint16_t ws[4];
        ws[0] = p->tc.rows;
        ws[1] = p->tc.cols;
        ws[2] = ws[3] = 0;
        return arg ? copy_out((void *)(uintptr_t)arg, ws, sizeof(ws)) : 0;
    }
    if (req == TIOCSWINSZ) {
        uint16_t ws[4];
        if (copy_in(ws, (const void *)(uintptr_t)arg, sizeof(ws)) < 0) return -EFAULT;
        if (ws[0] == 0 || ws[1] == 0 || ws[0] > 500 || ws[1] > 500) return -EINVAL;
        {
            int changed = p->tc.rows != ws[0] || p->tc.cols != ws[1];
            p->tc.rows = ws[0];
            p->tc.cols = ws[1];
            /* ধাপ ১৬খ-১: মাপ বদলালে সামনের কাজকে SIGWINCH (ডিফল্টে উপেক্ষা) */
            if (changed && p->tc.fg_pgid > 0) proc_kill_group(p->tc.fg_pgid, SIGWINCH);
        }
        return 0;
    }
    r = ttyctl_ioctl(&p->tc, req, arg, copy_in, copy_out);
    return r;
}

int pty_count(void)
{
    int i, n = 0;
    for (i = 0; i < PTY_MAX; i++) if (ptys[i]) n++;
    return n;
}
