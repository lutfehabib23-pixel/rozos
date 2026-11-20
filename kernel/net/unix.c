/* rozOS — Unix domain socket (AF_UNIX, SOCK_STREAM), ধাপ ১৪
 *
 * একই কম্পিউটারের প্রোগ্রামদের মধ্যে কথা বলার পথ — নেটওয়ার্ক (lwIP) ছাড়াই। rozwm
 * (window server) /tmp/.rozwm নামে listen করে; প্রতিটা অ্যাপ connect করে।
 *
 *   server: socket(AF_UNIX) → bind("/tmp/.rozwm") → listen → accept (poll-এ POLLIN)
 *   client: socket(AF_UNIX) → connect("/tmp/.rozwm") → read/write/send/recv
 *
 * নামগুলো কার্নেলের নিজের তালিকায় থাকে (ডিস্কে কোনো ফাইল হয় না); listen করা socket
 * বন্ধ হলে নাম মুছে যায়। প্রতিটা সংযোগে দুই দিকের দুটো ঘুরন্ত বাফার (UNIX_BUF)।
 * নিয়ম pipe-এর মতো: অপর পক্ষ বন্ধ → read 0, write -EPIPE। MSG_DONTWAIT (বা socket-এর
 * nonblock) দিলে অপেক্ষা নয়: -EAGAIN — window server যাতে কোনো ঝিমানো অ্যাপের জন্য
 * আটকে না যায়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/unix.h>
#include <rozos/vfs.h>
#include <rozos/heap.h>
#include <rozos/sched.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

#define UNIX_BUF     32768
#define UNIX_BACKLOG 16
#define UNIX_NAMES   32

enum { US_NEW = 0, US_LISTEN, US_CONNECTED, US_CLOSED };

struct upipe {                        /* এক দিকের বাফার — দুই পক্ষ ভাগ করে */
    uint8_t *buf;
    size_t head, count;
    int refs;                         /* কয়টা socket এটা দেখে (২ → ১ → ০ মুক্ত) */
};

struct usock {
    struct vnode vn;
    int state;
    int nonblock;
    char path[UNIX_PATH_MAX];         /* bind করা নাম (listen) */
    struct usock *peer;               /* সংযুক্ত অপর পক্ষ (বন্ধ হলে NULL) */
    struct upipe *rx, *tx;            /* আমার পড়ার / লেখার বাফার */
    struct usock *pending[UNIX_BACKLOG];
    int npending, backlog;
    int peer_closed;
};

static struct usock *names[UNIX_NAMES];
static const struct vnode_ops unix_ops;

static struct usock *usock_new(void)
{
    struct usock *s = (struct usock *)kzalloc(sizeof(*s));
    if (!s) return NULL;
    s->vn.type = VNODE_SOCKET;
    s->vn.mode = 0777;
    s->vn.refcount = 1;
    s->vn.nlink = 0;
    s->vn.ops = &unix_ops;
    s->vn.fs_data = s;
    return s;
}

static struct upipe *upipe_new(void)
{
    struct upipe *p = (struct upipe *)kzalloc(sizeof(*p));
    if (!p) return NULL;
    p->buf = (uint8_t *)kmalloc(UNIX_BUF);
    if (!p->buf) {
        kfree(p);
        return NULL;
    }
    p->refs = 2;
    return p;
}

static void upipe_put(struct upipe *p)
{
    if (p && --p->refs == 0) {
        kfree(p->buf);
        kfree(p);
    }
}

int unix_is_vnode(struct vnode *vn)
{
    return vn && vn->ops == &unix_ops;
}

int unix_create(int type, struct vnode **out)
{
    struct usock *s;
    if ((type & 0xF) != 1) return -EPROTONOSUPPORT;        /* শুধু SOCK_STREAM */
    s = usock_new();
    if (!s) return -ENOMEM;
    if (type & 04000) s->nonblock = 1;                     /* SOCK_NONBLOCK */
    *out = &s->vn;
    return 0;
}

int unix_set_nonblock(struct vnode *vn, int on)
{
    ((struct usock *)vn->fs_data)->nonblock = on ? 1 : 0;
    return 0;
}

static int find_name(const char *path)
{
    int i;
    for (i = 0; i < UNIX_NAMES; i++) {
        if (names[i] && !strcmp(names[i]->path, path)) return i;
    }
    return -1;
}

int unix_bind(struct vnode *vn, const char *path)
{
    struct usock *s = (struct usock *)vn->fs_data;
    uint64_t fl;
    int i;
    if (!path[0] || strlen(path) >= UNIX_PATH_MAX) return -EINVAL;
    if (s->path[0] || s->state != US_NEW) return -EINVAL;
    fl = cpu_irq_save();
    if (find_name(path) >= 0) {
        cpu_irq_restore(fl);
        return -EADDRINUSE;
    }
    for (i = 0; i < UNIX_NAMES && names[i]; i++) {
    }
    if (i == UNIX_NAMES) {
        cpu_irq_restore(fl);
        return -ENOMEM;
    }
    strcpy(s->path, path);
    names[i] = s;
    cpu_irq_restore(fl);
    return 0;
}

int unix_listen(struct vnode *vn, int backlog)
{
    struct usock *s = (struct usock *)vn->fs_data;
    if (!s->path[0]) return -EDESTADDRREQ;
    if (s->state != US_NEW && s->state != US_LISTEN) return -EINVAL;
    s->state = US_LISTEN;
    s->backlog = backlog < 1 ? 1 : backlog > UNIX_BACKLOG ? UNIX_BACKLOG : backlog;
    return 0;
}

int unix_connect(struct vnode *vn, const char *path)
{
    struct usock *s = (struct usock *)vn->fs_data, *l, *a;
    struct upipe *p1, *p2;
    uint64_t fl;
    int i;
    if (s->state != US_NEW) return s->state == US_CONNECTED ? -EISCONN : -EINVAL;
    a = usock_new();
    p1 = upipe_new();
    p2 = upipe_new();
    if (!a || !p1 || !p2) {
        kfree(a);
        if (p1) { kfree(p1->buf); kfree(p1); }
        if (p2) { kfree(p2->buf); kfree(p2); }
        return -ENOMEM;
    }
    fl = cpu_irq_save();
    i = find_name(path);
    l = i >= 0 ? names[i] : NULL;
    if (!l || l->state != US_LISTEN) {
        cpu_irq_restore(fl);
        kfree(a);
        kfree(p1->buf); kfree(p1); kfree(p2->buf); kfree(p2);
        return -ECONNREFUSED;
    }
    if (l->npending >= l->backlog) {
        cpu_irq_restore(fl);
        kfree(a);
        kfree(p1->buf); kfree(p1); kfree(p2->buf); kfree(p2);
        return -EAGAIN;
    }
    /* সার্ভারের দিকের নতুন socket (accept দেবে) — এখনই সংযুক্ত */
    s->rx = p1; s->tx = p2;
    a->rx = p2; a->tx = p1;
    s->peer = a; a->peer = s;
    s->state = a->state = US_CONNECTED;
    strcpy(a->path, l->path);
    l->pending[l->npending++] = a;
    cpu_irq_restore(fl);
    return 0;
}

int unix_accept(struct vnode *vn, struct vnode **out, int nonblock)
{
    struct usock *s = (struct usock *)vn->fs_data;
    for (;;) {
        cpu_cli();
        if (s->state != US_LISTEN) {
            cpu_sti();
            return -EINVAL;
        }
        if (s->npending > 0) {
            struct usock *a = s->pending[0];
            s->npending--;
            memmove(s->pending, s->pending + 1, (size_t)s->npending * sizeof(s->pending[0]));
            cpu_sti();
            *out = &a->vn;
            return 0;
        }
        if (nonblock || s->nonblock) {
            cpu_sti();
            return -EAGAIN;
        }
        if (sched_should_abort()) {
            cpu_sti();
            return -EINTR;
        }
        sched_relax();
    }
}

int64_t unix_recv(struct vnode *vn, void *buf, size_t len, int dontwait)
{
    struct usock *s = (struct usock *)vn->fs_data;
    uint8_t *out = (uint8_t *)buf;
    if (len == 0) return 0;
    if (s->state != US_CONNECTED && s->state != US_CLOSED) return -ENOTCONN;
    for (;;) {
        struct upipe *p;
        cpu_cli();
        p = s->rx;
        if (p && p->count > 0) {
            size_t n = 0;
            while (n < len && p->count > 0) {
                size_t chunk = UNIX_BUF - p->head;
                if (chunk > p->count) chunk = p->count;
                if (chunk > len - n) chunk = len - n;
                memcpy(out + n, p->buf + p->head, chunk);
                p->head = (p->head + chunk) % UNIX_BUF;
                p->count -= chunk;
                n += chunk;
            }
            cpu_sti();
            return (int64_t)n;
        }
        if (s->peer_closed || !p) {
            cpu_sti();
            return 0;
        }
        if (dontwait || s->nonblock) {
            cpu_sti();
            return -EAGAIN;
        }
        if (sched_should_abort()) {
            cpu_sti();
            return -EINTR;
        }
        sched_relax();
    }
}

int64_t unix_send(struct vnode *vn, const void *buf, size_t len, int dontwait)
{
    struct usock *s = (struct usock *)vn->fs_data;
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;
    if (len == 0) return 0;
    if (s->state != US_CONNECTED && s->state != US_CLOSED) return -ENOTCONN;
    for (;;) {
        struct upipe *p;
        cpu_cli();
        p = s->tx;
        if (s->peer_closed || !p) {
            cpu_sti();
            return done ? (int64_t)done : -EPIPE;
        }
        while (done < len && p->count < UNIX_BUF) {
            size_t tail = (p->head + p->count) % UNIX_BUF;
            size_t chunk = UNIX_BUF - tail;
            if (chunk > UNIX_BUF - p->count) chunk = UNIX_BUF - p->count;
            if (chunk > len - done) chunk = len - done;
            memcpy(p->buf + tail, in + done, chunk);
            p->count += chunk;
            done += chunk;
        }
        if (done == len) {
            cpu_sti();
            return (int64_t)done;
        }
        if (dontwait || s->nonblock) {
            cpu_sti();
            return done ? (int64_t)done : -EAGAIN;
        }
        if (sched_should_abort()) {
            cpu_sti();
            return done ? (int64_t)done : -EINTR;
        }
        sched_relax();
    }
}

static int64_t unix_vn_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)off;
    return unix_recv(vn, buf, len, 0);
}

static int64_t unix_vn_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)off;
    return unix_send(vn, buf, len, 0);
}

static int unix_vn_poll(struct vnode *vn, int events)
{
    struct usock *s = (struct usock *)vn->fs_data;
    int r = 0;
    (void)events;
    if (s->state == US_LISTEN) return s->npending > 0 ? VFS_POLLIN : 0;
    if (s->state != US_CONNECTED && s->state != US_CLOSED) return 0;
    if ((s->rx && s->rx->count > 0) || s->peer_closed) r |= VFS_POLLIN;
    if (s->peer_closed) r |= VFS_POLLHUP;
    else if (s->tx && s->tx->count < UNIX_BUF) r |= VFS_POLLOUT;
    return r;
}

static void unix_vn_release(struct vnode *vn)
{
    struct usock *s = (struct usock *)vn->fs_data;
    uint64_t fl = cpu_irq_save();
    int i;
    if (s->state == US_LISTEN || s->path[0]) {
        for (i = 0; i < UNIX_NAMES; i++) if (names[i] == s) names[i] = NULL;
    }
    /* accept না হওয়া সংযোগগুলো বন্ধ */
    for (i = 0; i < s->npending; i++) {
        struct usock *a = s->pending[i];
        if (a->peer) { a->peer->peer_closed = 1; a->peer->peer = NULL; }
        upipe_put(a->rx);
        upipe_put(a->tx);
        kfree(a);
    }
    s->npending = 0;
    if (s->peer) {
        s->peer->peer_closed = 1;
        s->peer->peer = NULL;
    }
    upipe_put(s->rx);
    upipe_put(s->tx);
    cpu_irq_restore(fl);
    kfree(s);
}

static const struct vnode_ops unix_ops = {
    unix_vn_read, unix_vn_write, NULL, NULL, NULL, NULL, NULL, unix_vn_release, unix_vn_poll
};

int unix_shutdown(struct vnode *vn, int how)
{
    struct usock *s = (struct usock *)vn->fs_data;
    (void)how;
    if (s->state != US_CONNECTED) return -ENOTCONN;
    return 0;
}
