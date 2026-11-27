/* rozOS — socket স্তর: BSD socket-এর অর্থ lwIP-এর raw API-র ওপর (ধাপ ৯)
 *
 * প্রতিটি socket একটা vnode (VNODE_SOCKET) — তাই fd টেবিলে বসে, read/write/close
 * সাধারণ ফাইলের মতোই চলে, fork-এ ভাগ হয়।
 *
 *   SOCK_STREAM (TCP): connect, bind, listen, accept, send, recv, shutdown
 *   SOCK_DGRAM  (UDP): bind, connect (ডিফল্ট ঠিকানা), sendto, recvfrom
 *   SOCK_RAW + IPPROTO_ICMP: ping-এর জন্য — recv-এ IP হেডার সহ (Linux-এর মতো)
 *
 * lwIP-এর callback সবসময় netd থ্রেডে চলে; syscall-এর থ্রেড অপেক্ষা করে sched_block()
 * বা সময়সীমা থাকলে sched_sleep_ms() দিয়ে, callback তাকে sched_wake() করে।
 * কার্নেল মাঝপথে preempt হয় না আর ইন্টারাপ্ট lwIP ছোঁয় না, তাই এখানে lock লাগে না।
 * ঠিকানা (ip) সবসময় network byte order-এ, পোর্ট host order-এ। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/net.h>
#include <rozos/vfs.h>
#include <rozos/sched.h>
#include <rozos/timer.h>
#include <rozos/heap.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#define DGRAM_QUEUE    32
#define ACCEPT_QUEUE   16
#define RXQ_MAX         8             /* ধাপ ২৫ক: rx-এর পরে আরো কয়টা শিকল */
#define RXQ_CHAIN_MAX   0xF000        /* এক শিকলে সর্বোচ্চ (u16 tot_len-এর নিচে) */
#define CONNECT_TIMEOUT_MS 20000

enum sock_state {
    SS_NEW = 0,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_LISTENING,
    SS_CLOSED
};

struct dgram {
    struct pbuf *p;
    uint32_t ip;
    uint16_t port;
};

struct sock {
    struct vnode vn;
    int type, proto;
    enum sock_state state;
    int err;                          /* জমে থাকা ত্রুটি (-ERRNO), SO_ERROR */
    int peer_closed;                  /* TCP: অপর পক্ষ লেখা শেষ করেছে */
    int bound;
    union {
        struct tcp_pcb *tcp;
        struct udp_pcb *udp;
        struct raw_pcb *raw;
    } pcb;
    struct pbuf *rx;                  /* TCP: না-পড়া ডেটা */
    /* ধাপ ২৫ক: window এখন ২৫৬ KiB, কিন্তু একটা pbuf শিকলের tot_len ১৬-বিট (৬৪ KiB) —
     * তাই বাকিটা আলাদা শিকলে, পরপর। rx খালি হলেই rx_more[0] সামনে আসে। */
    struct pbuf *rx_more[RXQ_MAX];
    int rx_more_n;
    struct dgram dq[DGRAM_QUEUE];     /* UDP/RAW */
    int dq_head, dq_count;
    struct sock *pending[ACCEPT_QUEUE];
    int npending, backlog;
    struct thread *waiter;
    uint64_t rcvtimeo, sndtimeo;      /* মিলিসেকেন্ড, 0 = চিরকাল */
    int nonblock;
};

static const struct vnode_ops sock_ops;

/* ধাপ ২৫ক: rx-এর সাথে জোড়া যায় কি (tot_len u16 উপচে না যায়) */
static int rx_fits(struct pbuf *chain, struct pbuf *p)
{
    return (uint32_t)chain->tot_len + p->tot_len <= RXQ_CHAIN_MAX;
}

/* rx খালি হলে পরের শিকল সামনে */
static void rx_promote(struct sock *s)
{
    int i;
    if (s->rx || s->rx_more_n == 0) return;
    s->rx = s->rx_more[0];
    for (i = 1; i < s->rx_more_n; i++) s->rx_more[i - 1] = s->rx_more[i];
    s->rx_more_n--;
}

static void rx_free_all(struct sock *s)
{
    int i;
    if (s->rx) pbuf_free(s->rx);
    for (i = 0; i < s->rx_more_n; i++) pbuf_free(s->rx_more[i]);
    s->rx = NULL;
    s->rx_more_n = 0;
}

/* ---------- সহায়ক ---------- */

static int lwip_err(err_t e)
{
    switch (e) {
    case ERR_OK:         return 0;
    case ERR_MEM:        return -ENOMEM;
    case ERR_BUF:        return -ENOBUFS;
    case ERR_TIMEOUT:    return -ETIMEDOUT;
    case ERR_RTE:        return -EHOSTUNREACH;
    case ERR_INPROGRESS: return -EINPROGRESS;
    case ERR_VAL:        return -EINVAL;
    case ERR_WOULDBLOCK: return -EAGAIN;
    case ERR_USE:        return -EADDRINUSE;
    case ERR_ALREADY:    return -EALREADY;
    case ERR_ISCONN:     return -EISCONN;
    case ERR_CONN:       return -ENOTCONN;
    case ERR_IF:         return -ENETDOWN;
    case ERR_ABRT:       return -ECONNABORTED;
    case ERR_RST:        return -ECONNRESET;
    case ERR_CLSD:       return -ENOTCONN;
    default:             return -EIO;
    }
}

static void wake(struct sock *s)
{
    if (s && s->waiter) {
        sched_wake(s->waiter);
    }
}

/* ready(s) সত্য না হওয়া পর্যন্ত অপেক্ষা। timeout_ms = 0 মানে চিরকাল */
static int wait_for(struct sock *s, int (*ready)(struct sock *), uint64_t timeout_ms, int nonblock)
{
    uint64_t deadline = timeout_ms ? timer_uptime_ms() + timeout_ms : 0;

    while (!ready(s)) {
        uint64_t flags;
        if (sched_should_abort()) {
            return -EINTR;
        }
        if (nonblock || s->nonblock) {
            return -EAGAIN;
        }
        if (deadline && timer_uptime_ms() >= deadline) {
            return -ETIMEDOUT;
        }
        flags = cpu_irq_save();
        s->waiter = sched_current();
        if (!ready(s)) {
            if (deadline) {
                uint64_t left = deadline - timer_uptime_ms();
                sched_sleep_ms(left > 1000 ? 1000 : (left ? left : 1));
            } else {
                sched_block();
            }
        }
        s->waiter = NULL;
        cpu_irq_restore(flags);
    }
    return 0;
}

static struct sock *sock_alloc(int type, int proto)
{
    struct sock *s = (struct sock *)kzalloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->type = type;
    s->proto = proto;
    s->vn.type = VNODE_SOCKET;
    s->vn.mode = 0777;
    s->vn.refcount = 1;
    s->vn.nlink = 0;
    s->vn.ops = &sock_ops;
    s->vn.fs_data = s;
    return s;
}

struct sock *sock_from_vnode(struct vnode *vn)
{
    return (vn && vn->type == VNODE_SOCKET) ? (struct sock *)vn->fs_data : NULL;
}

/* ---------- TCP callback ---------- */

static void tcp_setup(struct sock *s, struct tcp_pcb *pcb);

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct sock *s = (struct sock *)arg;
    (void)pcb;
    (void)err;
    if (!s) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p) {
        s->peer_closed = 1;
    } else if (!s->rx) {
        s->rx = p;
    } else if (s->rx_more_n == 0 && rx_fits(s->rx, p)) {
        pbuf_cat(s->rx, p);
    } else if (s->rx_more_n > 0 && rx_fits(s->rx_more[s->rx_more_n - 1], p)) {
        pbuf_cat(s->rx_more[s->rx_more_n - 1], p);      /* ধাপ ২৫ক */
    } else if (s->rx_more_n < RXQ_MAX) {
        s->rx_more[s->rx_more_n++] = p;
    } else {
        return ERR_MEM;                     /* জায়গা নেই: lwIP পরে আবার দেবে (refused data) */
    }
    wake(s);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    wake((struct sock *)arg);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    struct sock *s = (struct sock *)arg;
    if (!s) {
        return;
    }
    s->pcb.tcp = NULL;                       /* lwIP pcb আগেই মুক্ত করেছে */
    if (s->state == SS_CONNECTING && err == ERR_RST) {
        s->err = -ECONNREFUSED;
    } else if (s->state == SS_CONNECTING && err == ERR_ABRT) {
        s->err = -ETIMEDOUT;
    } else {
        s->err = lwip_err(err);
    }
    s->state = SS_CLOSED;
    wake(s);
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct sock *s = (struct sock *)arg;
    (void)pcb;
    if (s) {
        s->state = err == ERR_OK ? SS_CONNECTED : SS_CLOSED;
        if (err != ERR_OK) s->err = lwip_err(err);
        wake(s);
    }
    return ERR_OK;
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    struct sock *ls = (struct sock *)arg, *s;
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }
    if (!ls || ls->npending >= ls->backlog || ls->npending >= ACCEPT_QUEUE) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    s = sock_alloc(SOCK_STREAM, IPPROTO_TCP);
    if (!s) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    s->state = SS_CONNECTED;
    s->bound = 1;
    tcp_setup(s, newpcb);
    ls->pending[ls->npending++] = s;
    wake(ls);
    return ERR_OK;
}

static void tcp_setup(struct sock *s, struct tcp_pcb *pcb)
{
    s->pcb.tcp = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_sent(pcb, tcp_sent_cb);
    tcp_err(pcb, tcp_err_cb);
}

/* ---------- UDP ও RAW callback ---------- */

static int dq_push(struct sock *s, struct pbuf *p, uint32_t ip, uint16_t port)
{
    struct dgram *d;
    if (s->dq_count >= DGRAM_QUEUE) {
        return 0;
    }
    d = &s->dq[(s->dq_head + s->dq_count) % DGRAM_QUEUE];
    d->p = p;
    d->ip = ip;
    d->port = port;
    s->dq_count++;
    wake(s);
    return 1;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct sock *s = (struct sock *)arg;
    (void)pcb;
    if (!s || !dq_push(s, p, ip_2_ip4(addr)->addr, port)) {
        pbuf_free(p);
    }
}

/* ICMP: lwIP-কেও প্যাকেটটা দেখতে দেওয়া (0 ফেরত), আমরা নিজের কপি রাখি */
static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    struct sock *s = (struct sock *)arg;
    struct pbuf *q;
    (void)pcb;
    if (!s) {
        return 0;
    }
    q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
    if (q && !dq_push(s, q, ip_2_ip4(addr)->addr, 0)) {
        pbuf_free(q);
    }
    return 0;
}

/* ---------- তৈরি ও বন্ধ ---------- */

int sock_create(int domain, int type, int protocol, struct vnode **out)
{
    struct sock *s;
    int nonblock = (type & SOCK_NONBLOCK) != 0;

    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain != AF_INET) {
        return -EAFNOSUPPORT;
    }
    if (type == SOCK_STREAM && protocol != 0 && protocol != IPPROTO_TCP) return -EPROTONOSUPPORT;
    if (type == SOCK_DGRAM && protocol != 0 && protocol != IPPROTO_UDP) return -EPROTONOSUPPORT;
    if (type == SOCK_RAW && protocol != IPPROTO_ICMP) return -EPROTONOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) return -EPROTOTYPE;

    s = sock_alloc(type, protocol);
    if (!s) {
        return -ENOMEM;
    }
    s->nonblock = nonblock;
    if (type == SOCK_STREAM) {
        struct tcp_pcb *pcb = tcp_new();
        if (!pcb) { kfree(s); return -ENOBUFS; }
        tcp_setup(s, pcb);
    } else if (type == SOCK_DGRAM) {
        s->pcb.udp = udp_new();
        if (!s->pcb.udp) { kfree(s); return -ENOBUFS; }
        udp_recv(s->pcb.udp, udp_recv_cb, s);
    } else {
        s->pcb.raw = raw_new(IP_PROTO_ICMP);
        if (!s->pcb.raw) { kfree(s); return -ENOBUFS; }
        raw_recv(s->pcb.raw, raw_recv_cb, s);
    }
    *out = &s->vn;
    return 0;
}

/* callback সরানো — LISTEN অবস্থার pcb-তে recv/sent/err বসানো যায় না (lwIP assert) */
static void tcp_detach(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    if (pcb->state == LISTEN) {
        tcp_accept(pcb, NULL);
        return;
    }
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
}

static void sock_destroy(struct sock *s)
{
    int i;
    if (s->type == SOCK_STREAM) {
        if (s->pcb.tcp) {
            struct tcp_pcb *pcb = s->pcb.tcp;
            tcp_detach(pcb);
            if (s->state == SS_LISTENING) {
                tcp_close(pcb);
            } else if (tcp_close(pcb) != ERR_OK) {
                tcp_abort(pcb);                  /* মেমোরি না থাকলে সরাসরি RST */
            }
        }
        for (i = 0; i < s->npending; i++) {
            struct sock *c = s->pending[i];
            if (c->pcb.tcp) {
                tcp_detach(c->pcb.tcp);
                tcp_abort(c->pcb.tcp);
            }
            rx_free_all(c);                     /* ধাপ ২৫ক: rx_more সহ */
            kfree(c);
        }
        rx_free_all(s);
    } else {
        if (s->type == SOCK_DGRAM && s->pcb.udp) udp_remove(s->pcb.udp);
        if (s->type == SOCK_RAW && s->pcb.raw) raw_remove(s->pcb.raw);
        while (s->dq_count > 0) {
            pbuf_free(s->dq[s->dq_head].p);
            s->dq_head = (s->dq_head + 1) % DGRAM_QUEUE;
            s->dq_count--;
        }
    }
    kfree(s);
}

/* ---------- সংযোগ ---------- */

static int not_connecting(struct sock *s) { return s->state != SS_CONNECTING; }

int sock_bind(struct sock *s, uint32_t ip, uint16_t port)
{
    ip_addr_t a;
    err_t e;
    ip_addr_set_ip4_u32(&a, ip);
    if (s->type == SOCK_STREAM) {
        if (!s->pcb.tcp || s->state != SS_NEW) return -EINVAL;
        e = tcp_bind(s->pcb.tcp, &a, port);
    } else if (s->type == SOCK_DGRAM) {
        e = udp_bind(s->pcb.udp, &a, port);
    } else {
        e = raw_bind(s->pcb.raw, &a);
    }
    if (e == ERR_OK) s->bound = 1;
    return lwip_err(e);
}

int sock_connect(struct sock *s, uint32_t ip, uint16_t port)
{
    ip_addr_t a;
    int r;
    ip_addr_set_ip4_u32(&a, ip);

    if (s->type == SOCK_DGRAM) return lwip_err(udp_connect(s->pcb.udp, &a, port));
    if (s->type == SOCK_RAW) return lwip_err(raw_connect(s->pcb.raw, &a));

    if (s->state == SS_CONNECTED) return -EISCONN;
    if (s->state == SS_CONNECTING) return -EALREADY;
    if (s->state != SS_NEW || !s->pcb.tcp) return -EINVAL;
    s->err = 0;
    s->state = SS_CONNECTING;
    r = lwip_err(tcp_connect(s->pcb.tcp, &a, port, tcp_connected_cb));
    if (r < 0) {
        s->state = SS_NEW;
        return r;
    }
    if (s->nonblock) return -EINPROGRESS;
    r = wait_for(s, not_connecting, CONNECT_TIMEOUT_MS, 0);
    if (r == -ETIMEDOUT && s->pcb.tcp) {
        struct tcp_pcb *pcb = s->pcb.tcp;
        tcp_detach(pcb);
        tcp_abort(pcb);
        s->pcb.tcp = NULL;
        s->state = SS_CLOSED;
        return -ETIMEDOUT;
    }
    if (r < 0) return r;
    if (s->state != SS_CONNECTED) {
        r = s->err ? s->err : -ECONNREFUSED;
        s->err = 0;
        return r;
    }
    return 0;
}

int sock_listen(struct sock *s, int backlog)
{
    struct tcp_pcb *lp;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (s->state == SS_LISTENING) { s->backlog = backlog; return 0; }
    if (s->state != SS_NEW || !s->pcb.tcp) return -EINVAL;
    if (!s->bound) {
        int r = sock_bind(s, 0, 0);
        if (r < 0) return r;
    }
    if (backlog < 1) backlog = 1;
    if (backlog > ACCEPT_QUEUE) backlog = ACCEPT_QUEUE;
    tcp_detach(s->pcb.tcp);
    lp = tcp_listen_with_backlog(s->pcb.tcp, (u8_t)backlog);
    if (!lp) {
        tcp_setup(s, s->pcb.tcp);
        return -ENOBUFS;
    }
    s->pcb.tcp = lp;
    s->backlog = backlog;
    s->state = SS_LISTENING;
    tcp_arg(lp, s);
    tcp_accept(lp, tcp_accept_cb);
    return 0;
}

static int has_pending(struct sock *s) { return s->npending > 0; }

int sock_accept(struct sock *s, struct vnode **out, uint32_t *ip, uint16_t *port)
{
    struct sock *c;
    int r;
    if (s->state != SS_LISTENING) return -EINVAL;
    r = wait_for(s, has_pending, s->rcvtimeo, 0);
    if (r < 0) return r == -ETIMEDOUT ? -EAGAIN : r;
    c = s->pending[0];
    memmove(&s->pending[0], &s->pending[1], sizeof(s->pending[0]) * (size_t)(s->npending - 1));
    s->npending--;
    if (c->pcb.tcp) {
        if (ip) *ip = ip_2_ip4(&c->pcb.tcp->remote_ip)->addr;
        if (port) *port = c->pcb.tcp->remote_port;
    }
    *out = &c->vn;
    return 0;
}

/* ---------- ডেটা ---------- */

static int tcp_writable(struct sock *s)
{
    return !s->pcb.tcp || s->err || s->state != SS_CONNECTED || tcp_sndbuf(s->pcb.tcp) > 0;
}

static int tcp_readable(struct sock *s)
{
    return s->rx || s->peer_closed || s->err || s->state != SS_CONNECTED;
}

static int dgram_ready(struct sock *s) { return s->dq_count > 0; }

int64_t sock_send(struct sock *s, const void *buf, size_t len, int flags,
                  int has_addr, uint32_t ip, uint16_t port)
{
    const uint8_t *p = (const uint8_t *)buf;
    int dontwait = (flags & MSG_DONTWAIT) != 0;

    if (s->type == SOCK_STREAM) {
        size_t done = 0;
        if (s->state != SS_CONNECTED) return s->err ? s->err : -ENOTCONN;
        while (done < len) {
            size_t room, n;
            err_t e;
            int r = wait_for(s, tcp_writable, s->sndtimeo, dontwait);
            if (r < 0) return done ? (int64_t)done : r;
            if (!s->pcb.tcp || s->state != SS_CONNECTED) {
                r = s->err ? s->err : -ECONNRESET;
                return done ? (int64_t)done : r;
            }
            room = tcp_sndbuf(s->pcb.tcp);
            n = len - done;
            if (n > room) n = room;
            if (n > 0xFFFF) n = 0xFFFF;
            e = tcp_write(s->pcb.tcp, p + done, (u16_t)n, TCP_WRITE_FLAG_COPY);
            if (e == ERR_MEM) {
                tcp_output(s->pcb.tcp);
                sched_sleep_ms(1);                   /* কিউ খালি হওয়ার সুযোগ */
                continue;
            }
            if (e != ERR_OK) return done ? (int64_t)done : lwip_err(e);
            done += n;
            tcp_output(s->pcb.tcp);
        }
        return (int64_t)done;
    } else {
        struct pbuf *pb;
        ip_addr_t a;
        err_t e;
        if (len > 65000) return -EMSGSIZE;
        pb = pbuf_alloc(s->type == SOCK_RAW ? PBUF_IP : PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (!pb) return -ENOBUFS;
        memcpy(pb->payload, p, len);
        ip_addr_set_ip4_u32(&a, ip);
        if (s->type == SOCK_DGRAM) {
            if (has_addr) e = udp_sendto(s->pcb.udp, pb, &a, port);
            else if (!ip_addr_isany(&s->pcb.udp->remote_ip)) e = udp_send(s->pcb.udp, pb);
            else e = ERR_CONN;
        } else {
            if (has_addr) e = raw_sendto(s->pcb.raw, pb, &a);
            else e = raw_send(s->pcb.raw, pb);
        }
        pbuf_free(pb);
        if (e == ERR_CONN) return -EDESTADDRREQ;
        return e == ERR_OK ? (int64_t)len : lwip_err(e);
    }
}

int64_t sock_recv(struct sock *s, void *buf, size_t len, int flags, uint32_t *ip, uint16_t *port)
{
    int peek = (flags & MSG_PEEK) != 0, r;

    if (s->type == SOCK_STREAM) {
        u16_t n;
        if (s->state == SS_NEW || s->state == SS_LISTENING) return -ENOTCONN;
        r = wait_for(s, tcp_readable, s->rcvtimeo, (flags & MSG_DONTWAIT) != 0);
        if (r < 0) return r == -ETIMEDOUT ? -EAGAIN : r;
        if (!s->rx) {
            if (s->peer_closed) return 0;           /* EOF */
            r = s->err;
            s->err = 0;
            return r ? r : 0;
        }
        if (len > 0xFFFF) len = 0xFFFF;
        n = pbuf_copy_partial(s->rx, buf, (u16_t)len, 0);
        if (!peek) {
            s->rx = pbuf_free_header(s->rx, n);
            rx_promote(s);                          /* ধাপ ২৫ক */
            if (s->pcb.tcp) tcp_recved(s->pcb.tcp, n);
        }
        if (ip && s->pcb.tcp) *ip = ip_2_ip4(&s->pcb.tcp->remote_ip)->addr;
        if (port && s->pcb.tcp) *port = s->pcb.tcp->remote_port;
        return n;
    } else {
        struct dgram *d;
        u16_t n;
        r = wait_for(s, dgram_ready, s->rcvtimeo, (flags & MSG_DONTWAIT) != 0);
        if (r < 0) return r == -ETIMEDOUT ? -EAGAIN : r;
        d = &s->dq[s->dq_head];
        n = pbuf_copy_partial(d->p, buf, (u16_t)(len > 0xFFFF ? 0xFFFF : len), 0);
        if (ip) *ip = d->ip;
        if (port) *port = d->port;
        if (!peek) {
            pbuf_free(d->p);
            s->dq_head = (s->dq_head + 1) % DGRAM_QUEUE;
            s->dq_count--;
        }
        return n;
    }
}

int sock_shutdown(struct sock *s, int how)
{
    if (s->type != SOCK_STREAM) return 0;
    if (!s->pcb.tcp || s->state != SS_CONNECTED) return -ENOTCONN;
    return lwip_err(tcp_shutdown(s->pcb.tcp, how == 0 || how == 2, how == 1 || how == 2));
}

int sock_name(struct sock *s, int peer, uint32_t *ip, uint16_t *port)
{
    *ip = 0;
    *port = 0;
    if (s->type == SOCK_STREAM && s->pcb.tcp) {
        if (peer && s->state != SS_CONNECTED) return -ENOTCONN;
        *ip = ip_2_ip4(peer ? &s->pcb.tcp->remote_ip : &s->pcb.tcp->local_ip)->addr;
        *port = peer ? s->pcb.tcp->remote_port : s->pcb.tcp->local_port;
    } else if (s->type == SOCK_DGRAM && s->pcb.udp) {
        *ip = ip_2_ip4(peer ? &s->pcb.udp->remote_ip : &s->pcb.udp->local_ip)->addr;
        *port = peer ? s->pcb.udp->remote_port : s->pcb.udp->local_port;
        if (peer && !*ip) return -ENOTCONN;
    } else if (peer) {
        return -ENOTCONN;
    }
    return 0;
}

/* struct timeval (সেকেন্ড, মাইক্রোসেকেন্ড) → মিলিসেকেন্ড */
static uint64_t timeval_ms(const void *val, size_t len)
{
    int64_t tv[2];
    if (len < sizeof(tv)) return 0;
    memcpy(tv, val, sizeof(tv));
    if (tv[0] < 0 || tv[1] < 0) return 0;
    return (uint64_t)tv[0] * 1000 + (uint64_t)tv[1] / 1000;
}

int sock_setopt(struct sock *s, int level, int name, const void *val, size_t len)
{
    int on = 0;
    if (len >= sizeof(int)) memcpy(&on, val, sizeof(int));
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_RCVTIMEO: s->rcvtimeo = timeval_ms(val, len); return 0;
        case SO_SNDTIMEO: s->sndtimeo = timeval_ms(val, len); return 0;
        case SO_REUSEADDR:
        case SO_KEEPALIVE:
        case SO_BROADCAST: {
            struct ip_pcb *ipcb = s->type == SOCK_STREAM ? (struct ip_pcb *)(void *)s->pcb.tcp :
                                  s->type == SOCK_DGRAM ? (struct ip_pcb *)(void *)s->pcb.udp :
                                  (struct ip_pcb *)(void *)s->pcb.raw;
            u8_t opt = name == SO_REUSEADDR ? SOF_REUSEADDR : name == SO_KEEPALIVE ? SOF_KEEPALIVE : SOF_BROADCAST;
            if (!ipcb) return -EINVAL;
            if (on) ip_set_option(ipcb, opt);
            else ip_reset_option(ipcb, opt);
            return 0;
        }
        }
        return -ENOPROTOOPT;
    }
    if (level == IPPROTO_TCP && name == 1 /* TCP_NODELAY */ && s->type == SOCK_STREAM && s->pcb.tcp) {
        if (on) tcp_nagle_disable(s->pcb.tcp);
        else tcp_nagle_enable(s->pcb.tcp);
        return 0;
    }
    return -ENOPROTOOPT;
}

int sock_getopt(struct sock *s, int level, int name, void *val, size_t *len)
{
    int v;
    if (level != SOL_SOCKET || *len < sizeof(int)) return -ENOPROTOOPT;
    switch (name) {
    case SO_ERROR: v = -s->err; s->err = 0; break;
    case 3 /* SO_TYPE */: v = s->type; break;
    default: return -ENOPROTOOPT;
    }
    memcpy(val, &v, sizeof(int));
    *len = sizeof(int);
    return 0;
}

/* ---------- vnode ops: read/write/close সাধারণ fd দিয়ে ---------- */

static int64_t sock_vn_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)off;
    return sock_recv((struct sock *)vn->fs_data, buf, len, 0, NULL, NULL);
}

static int64_t sock_vn_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)off;
    return sock_send((struct sock *)vn->fs_data, buf, len, 0, 0, 0, 0);
}

static void sock_vn_release(struct vnode *vn)
{
    sock_destroy((struct sock *)vn->fs_data);
}

/* ধাপ ১৩: poll() — এখনই পড়া/লেখা যাবে কিনা (আটকে না থেকে) */
static int sock_vn_poll(struct vnode *vn, int events)
{
    struct sock *s = (struct sock *)vn->fs_data;
    int r = 0;
    (void)events;
    if (s->err) r |= VFS_POLLERR;
    if (s->type == SOCK_STREAM) {
        if (s->state == SS_LISTENING) {
            if (s->npending > 0) r |= VFS_POLLIN;
        } else if (s->state == SS_CONNECTED) {
            if (s->rx || s->peer_closed) r |= VFS_POLLIN;
            if (s->peer_closed) r |= VFS_POLLHUP;
            if (s->pcb.tcp && tcp_sndbuf(s->pcb.tcp) > 0) r |= VFS_POLLOUT;
        } else if (s->state != SS_NEW && s->state != SS_CONNECTING) {
            r |= VFS_POLLIN | VFS_POLLHUP;          /* বন্ধ: read() সাথে সাথে 0 দেবে */
        }
    } else {
        if (s->dq_count > 0) r |= VFS_POLLIN;
        r |= VFS_POLLOUT;
    }
    return r;
}

static const struct vnode_ops sock_ops = {
    sock_vn_read,
    sock_vn_write,
    NULL, NULL, NULL, NULL, NULL,
    sock_vn_release,
    sock_vn_poll, NULL /* rename (ধাপ ১৫ক) */, NULL /* setflags (ধাপ ১৯ছ) */
};
