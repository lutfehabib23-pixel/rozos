/* rozOS — pipe (ধাপ ১৩)
 *
 * pipe() দুটো vnode বানায় (VNODE_FIFO): পড়ার মুখ আর লেখার মুখ, দুটোই একই struct pipe-এর
 * ঘুরন্ত বাফার (PIPE_BUF_SIZE) দেখে। প্রতিটা মুখ একটা fd (vfs_install_vnode); dup/fork-এ
 * file description ভাগ হয়, তাই শেষ fd বন্ধ হলে vnode-এর refcount শূন্য → release →
 * ওই মুখ "বন্ধ"। দুটো মুখই বন্ধ হলে struct pipe মুক্ত।
 *
 * নিয়ম (POSIX-এর মতো):
 *   read:  ডেটা থাকলে যা আছে (len পর্যন্ত); খালি আর লেখক নেই → 0 (EOF);
 *          খালি আর লেখক আছে → অপেক্ষা
 *   write: পাঠক নেই → -EPIPE; জায়গা না থাকলে অপেক্ষা; সব বাইট লেখা পর্যন্ত
 *          (PIPE_BUF পর্যন্ত লেখা একবারে — অন্য লেখকের সাথে মেশে না)
 *   poll:  POLLIN (ডেটা বা EOF), POLLOUT (জায়গা), POLLHUP (অন্য মুখ বন্ধ), POLLERR
 * অপেক্ষা sched_relax() দিয়ে (devfs-এর মতো); প্রসেস kill হলে -EINTR।
 * এক CPU — বাফার বদলানো হয় interrupt বন্ধ রেখে (preemption-ও তখন হয় না)।
 * read/write VFS lock ছাড়াই ডাকা হয় (vfs.c do_rw, socket-এর মতো)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/heap.h>
#include <rozos/sched.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

#define PIPE_BUF_SIZE 65536          /* Linux-এর ডিফল্ট মাপ */
#define PIPE_ATOMIC   4096           /* PIPE_BUF: এর ভেতরের লেখা ভাঙে না */

struct pipe {
    struct vnode rd, wr;             /* দুই মুখ — vnode-এর fs_data এই struct */
    uint8_t *buf;
    size_t head, count;              /* head = পড়ার জায়গা */
    int rd_open, wr_open;
    uint64_t id;
};

static uint64_t next_pipe_id = 1;
static const struct vnode_ops pipe_rd_ops, pipe_wr_ops;

static void pipe_maybe_free(struct pipe *p)
{
    if (!p->rd_open && !p->wr_open) {
        kfree(p->buf);
        kfree(p);
    }
}

static int64_t pipe_read(struct vnode *vn, uint64_t off, void *dst, size_t len)
{
    struct pipe *p = (struct pipe *)vn->fs_data;
    uint8_t *out = (uint8_t *)dst;
    size_t n = 0;
    (void)off;
    if (len == 0) return 0;
    for (;;) {
        cpu_cli();
        if (p->count > 0) {
            while (n < len && p->count > 0) {
                size_t chunk = PIPE_BUF_SIZE - p->head;
                if (chunk > p->count) chunk = p->count;
                if (chunk > len - n) chunk = len - n;
                memcpy(out + n, p->buf + p->head, chunk);
                p->head = (p->head + chunk) % PIPE_BUF_SIZE;
                p->count -= chunk;
                n += chunk;
            }
            cpu_sti();
            return (int64_t)n;
        }
        if (!p->wr_open) {
            cpu_sti();
            return 0;                                /* EOF */
        }
        if (sched_should_abort()) {
            cpu_sti();
            return -EINTR;
        }
        sched_relax();                               /* IF = 1 করে ফেরে */
    }
}

static int64_t pipe_write(struct vnode *vn, uint64_t off, const void *src, size_t len)
{
    struct pipe *p = (struct pipe *)vn->fs_data;
    const uint8_t *in = (const uint8_t *)src;
    size_t n = 0;
    (void)off;
    if (len == 0) return 0;
    for (;;) {
        size_t room;
        cpu_cli();
        if (!p->rd_open) {
            cpu_sti();
            return n ? (int64_t)n : -EPIPE;
        }
        room = PIPE_BUF_SIZE - p->count;
        /* ছোট লেখা (PIPE_ATOMIC পর্যন্ত) একবারেই — পুরো জায়গা না হলে অপেক্ষা */
        if (room > 0 && !(len - n <= PIPE_ATOMIC && room < len - n)) {
            while (n < len && p->count < PIPE_BUF_SIZE) {
                size_t tail = (p->head + p->count) % PIPE_BUF_SIZE;
                size_t chunk = PIPE_BUF_SIZE - tail;
                if (chunk > PIPE_BUF_SIZE - p->count) chunk = PIPE_BUF_SIZE - p->count;
                if (chunk > len - n) chunk = len - n;
                memcpy(p->buf + tail, in + n, chunk);
                p->count += chunk;
                n += chunk;
            }
            if (n == len) {
                cpu_sti();
                return (int64_t)n;
            }
        }
        if (sched_should_abort()) {
            cpu_sti();
            return n ? (int64_t)n : -EINTR;
        }
        sched_relax();
    }
}

static int pipe_rd_poll(struct vnode *vn, int events)
{
    struct pipe *p = (struct pipe *)vn->fs_data;
    int r = 0;
    (void)events;
    if (p->count > 0) r |= VFS_POLLIN;
    if (!p->wr_open) r |= VFS_POLLIN | VFS_POLLHUP;   /* read() সাথে সাথে 0 দেবে */
    return r;
}

static int pipe_wr_poll(struct vnode *vn, int events)
{
    struct pipe *p = (struct pipe *)vn->fs_data;
    (void)events;
    if (!p->rd_open) return VFS_POLLERR | VFS_POLLOUT;  /* write() সাথে সাথে EPIPE */
    return p->count < PIPE_BUF_SIZE ? VFS_POLLOUT : 0;
}

static void pipe_rd_release(struct vnode *vn)
{
    struct pipe *p = (struct pipe *)vn->fs_data;
    uint64_t flags = cpu_irq_save();
    p->rd_open = 0;
    cpu_irq_restore(flags);
    pipe_maybe_free(p);
}

static void pipe_wr_release(struct vnode *vn)
{
    struct pipe *p = (struct pipe *)vn->fs_data;
    uint64_t flags = cpu_irq_save();
    p->wr_open = 0;
    cpu_irq_restore(flags);
    pipe_maybe_free(p);
}

static const struct vnode_ops pipe_rd_ops = {
    pipe_read, NULL, NULL, NULL, NULL, NULL, NULL, pipe_rd_release, pipe_rd_poll
};

static const struct vnode_ops pipe_wr_ops = {
    NULL, pipe_write, NULL, NULL, NULL, NULL, NULL, pipe_wr_release, pipe_wr_poll
};

static void init_end(struct pipe *p, struct vnode *vn, const struct vnode_ops *ops, uint64_t ino)
{
    vn->type = VNODE_FIFO;
    vn->mode = 0600;
    vn->inode = ino;
    vn->refcount = 1;
    vn->nlink = 0;                    /* কোনো ডিরেক্টরিতে নেই — refcount 0 হলেই release */
    vn->ops = ops;
    vn->fs_data = p;
}

int vfs_pipe(int fds[2])
{
    struct pipe *p = (struct pipe *)kzalloc(sizeof(*p));
    int r, w;
    if (!p) return -ENOMEM;
    p->buf = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!p->buf) {
        kfree(p);
        return -ENOMEM;
    }
    p->id = next_pipe_id++;
    p->rd_open = p->wr_open = 1;
    init_end(p, &p->rd, &pipe_rd_ops, 0x70000000ULL + p->id * 2);
    init_end(p, &p->wr, &pipe_wr_ops, 0x70000000ULL + p->id * 2 + 1);

    r = vfs_install_vnode(&p->rd, O_RDONLY);
    if (r < 0) {
        kfree(p->buf);
        kfree(p);
        return r;
    }
    w = vfs_install_vnode(&p->wr, O_WRONLY);
    if (w < 0) {
        p->wr_open = 0;                    /* লেখার মুখ কখনো খোলা হয়নি */
        vfs_close(r);                      /* → pipe_rd_release → মুক্ত */
        return w;
    }
    fds[0] = r;
    fds[1] = w;
    return 0;
}
