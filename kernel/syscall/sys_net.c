/* rozOS — নেটওয়ার্ক syscall (Linux x86-64 নম্বর) ও rozOS-এর নিজস্ব netinfo/netconfig
 *
 * socket fd সাধারণ fd-ই (vnode VNODE_SOCKET), তাই read/write/close আগের syscall দিয়েই চলে।
 * ইউজারের বাফার সবসময় কার্নেলের বাউন্স-বাফারে কপি হয় (sys_fs.c-র মতো)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/syscall.h>
#include <rozos/unix.h>
#include <rozos/net.h>
#include <rozos/vfs.h>
#include <rozos/uaccess.h>
#include <rozos/heap.h>
#include <rozos/errno.h>
#include <rozos/string.h>

#define IO_MAX (64 * 1024)

/* struct sockaddr_in (16 বাইট) */
struct k_sockaddr_in {
    uint16_t family;
    uint16_t port;            /* network order */
    uint32_t addr;            /* network order */
    uint8_t  zero[8];
};

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static int get_addr(uint64_t uaddr, uint64_t ulen, uint32_t *ip, uint16_t *port)
{
    struct k_sockaddr_in sa;
    if (ulen < 8) return -EINVAL;
    memset(&sa, 0, sizeof(sa));
    if (copy_from_user(&sa, (const void *)(uintptr_t)uaddr, ulen < sizeof(sa) ? ulen : sizeof(sa)) < 0) {
        return -EFAULT;
    }
    if (sa.family != AF_INET) return -EAFNOSUPPORT;
    *ip = sa.addr;
    *port = swap16(sa.port);
    return 0;
}

/* *ulen-এ জায়গা জেনে লেখা, তারপর আসল দৈর্ঘ্য *ulen-এ */
static int put_addr(uint64_t uaddr, uint64_t ulenp, uint32_t ip, uint16_t port)
{
    struct k_sockaddr_in sa;
    uint32_t len;
    if (!uaddr || !ulenp) return 0;
    if (copy_from_user(&len, (const void *)(uintptr_t)ulenp, sizeof(len)) < 0) return -EFAULT;
    memset(&sa, 0, sizeof(sa));
    sa.family = AF_INET;
    sa.port = swap16(port);
    sa.addr = ip;
    if (copy_to_user((void *)(uintptr_t)uaddr, &sa, len < sizeof(sa) ? len : sizeof(sa)) < 0) return -EFAULT;
    len = sizeof(sa);
    return copy_to_user((void *)(uintptr_t)ulenp, &len, sizeof(len));
}

/* fd → socket (vnode-এর ref ধরে রাখে; শেষে vnode_unref) */
static struct sock *get_sock(int fd, struct vnode **vn_out, int *err)
{
    struct vnode *vn = vfs_fd_vnode(fd);
    struct sock *s;
    if (!vn) { *err = -EBADF; return NULL; }
    s = sock_from_vnode(vn);
    if (!s) { vnode_unref(vn); *err = -ENOTSOCK; return NULL; }
    *vn_out = vn;
    return s;
}

static int install(struct vnode *vn)
{
    int fd = vfs_install_vnode(vn, O_RDWR);
    if (fd < 0) vnode_unref(vn);
    return fd;
}

static int64_t sys_socket(struct interrupt_frame *f, uint64_t domain, uint64_t type, uint64_t proto,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn;
    int r;
    (void)f; (void)a4; (void)a5; (void)a6;
    r = sock_create((int)domain, (int)type, (int)proto, &vn);
    return r < 0 ? r : install(vn);
}

static int64_t sys_connect(struct interrupt_frame *f, uint64_t fd, uint64_t uaddr, uint64_t len,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn;
    struct sock *s;
    uint32_t ip;
    uint16_t port;
    int r;
    (void)f; (void)a4; (void)a5; (void)a6;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = get_addr(uaddr, len, &ip, &port);
    if (r == 0) r = sock_connect(s, ip, port);
    vnode_unref(vn);
    return r;
}

static int64_t sys_bind(struct interrupt_frame *f, uint64_t fd, uint64_t uaddr, uint64_t len,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn;
    struct sock *s;
    uint32_t ip;
    uint16_t port;
    int r;
    (void)f; (void)a4; (void)a5; (void)a6;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = get_addr(uaddr, len, &ip, &port);
    if (r == 0) r = sock_bind(s, ip, port);
    vnode_unref(vn);
    return r;
}

static int64_t sys_listen(struct interrupt_frame *f, uint64_t fd, uint64_t backlog, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn;
    struct sock *s;
    int r;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = sock_listen(s, (int)backlog);
    vnode_unref(vn);
    return r;
}

static int64_t sys_accept(struct interrupt_frame *f, uint64_t fd, uint64_t uaddr, uint64_t ulen,
                          uint64_t flags, uint64_t a5, uint64_t a6)
{
    struct vnode *vn, *cvn;
    struct sock *s;
    uint32_t ip = 0;
    uint16_t port = 0;
    int r;
    (void)f; (void)flags; (void)a5; (void)a6;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = sock_accept(s, &cvn, &ip, &port);
    vnode_unref(vn);
    if (r < 0) return r;
    r = install(cvn);
    if (r >= 0) put_addr(uaddr, ulen, ip, port);
    return r;
}

static int64_t sys_sendto(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                          uint64_t flags, uint64_t uaddr, uint64_t alen)
{
    struct vnode *vn;
    struct sock *s;
    uint32_t ip = 0;
    uint16_t port = 0;
    void *kbuf;
    int64_t r;
    int ir;
    (void)f;
    if (len > IO_MAX) len = IO_MAX;
    if (!(s = get_sock((int)fd, &vn, &ir))) return ir;
    if (uaddr && (ir = get_addr(uaddr, alen, &ip, &port)) < 0) { vnode_unref(vn); return ir; }
    kbuf = kmalloc(len ? len : 1);
    if (!kbuf) { vnode_unref(vn); return -ENOMEM; }
    if (copy_from_user(kbuf, (const void *)(uintptr_t)ubuf, len) < 0) r = -EFAULT;
    else r = sock_send(s, kbuf, len, (int)flags, uaddr != 0, ip, port);
    kfree(kbuf);
    vnode_unref(vn);
    return r;
}

static int64_t sys_recvfrom(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                            uint64_t flags, uint64_t uaddr, uint64_t ulenp)
{
    struct vnode *vn;
    struct sock *s;
    uint32_t ip = 0;
    uint16_t port = 0;
    void *kbuf;
    int64_t r;
    int ir;
    (void)f;
    if (len > IO_MAX) len = IO_MAX;
    if (!user_range_ok(ubuf, len, 1)) return -EFAULT;
    if (!(s = get_sock((int)fd, &vn, &ir))) return ir;
    kbuf = kmalloc(len ? len : 1);
    if (!kbuf) { vnode_unref(vn); return -ENOMEM; }
    r = sock_recv(s, kbuf, len, (int)flags, &ip, &port);
    if (r > 0 && copy_to_user((void *)(uintptr_t)ubuf, kbuf, (size_t)r) < 0) r = -EFAULT;
    if (r >= 0) put_addr(uaddr, ulenp, ip, port);
    kfree(kbuf);
    vnode_unref(vn);
    return r;
}

static int64_t sys_shutdown(struct interrupt_frame *f, uint64_t fd, uint64_t how, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn;
    struct sock *s;
    int r;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = sock_shutdown(s, (int)how);
    vnode_unref(vn);
    return r;
}

static int64_t do_name(uint64_t fd, uint64_t uaddr, uint64_t ulenp, int peer)
{
    struct vnode *vn;
    struct sock *s;
    uint32_t ip;
    uint16_t port;
    int r;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = sock_name(s, peer, &ip, &port);
    if (r == 0) r = put_addr(uaddr, ulenp, ip, port);
    vnode_unref(vn);
    return r;
}

static int64_t sys_getsockname(struct interrupt_frame *f, uint64_t fd, uint64_t a, uint64_t l,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a4; (void)a5; (void)a6;
    return do_name(fd, a, l, 0);
}

static int64_t sys_getpeername(struct interrupt_frame *f, uint64_t fd, uint64_t a, uint64_t l,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a4; (void)a5; (void)a6;
    return do_name(fd, a, l, 1);
}

static int64_t sys_setsockopt(struct interrupt_frame *f, uint64_t fd, uint64_t level, uint64_t name,
                              uint64_t uval, uint64_t len, uint64_t a6)
{
    struct vnode *vn;
    struct sock *s;
    uint8_t val[32];
    int r;
    (void)f; (void)a6;
    if (len > sizeof(val)) len = sizeof(val);
    if (copy_from_user(val, (const void *)(uintptr_t)uval, len) < 0) return -EFAULT;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = sock_setopt(s, (int)level, (int)name, val, len);
    vnode_unref(vn);
    return r;
}

static int64_t sys_getsockopt(struct interrupt_frame *f, uint64_t fd, uint64_t level, uint64_t name,
                              uint64_t uval, uint64_t ulenp, uint64_t a6)
{
    struct vnode *vn;
    struct sock *s;
    uint8_t val[32];
    uint32_t ulen;
    size_t len;
    int r;
    (void)f; (void)a6;
    if (copy_from_user(&ulen, (const void *)(uintptr_t)ulenp, sizeof(ulen)) < 0) return -EFAULT;
    len = ulen > sizeof(val) ? sizeof(val) : ulen;
    if (!(s = get_sock((int)fd, &vn, &r))) return r;
    r = sock_getopt(s, (int)level, (int)name, val, &len);
    vnode_unref(vn);
    if (r < 0) return r;
    ulen = (uint32_t)len;
    if (copy_to_user((void *)(uintptr_t)uval, val, len) < 0) return -EFAULT;
    return copy_to_user((void *)(uintptr_t)ulenp, &ulen, sizeof(ulen));
}

/* rozOS: interface-এর তথ্য (ifconfig) — index-তম কার্ড */
static int64_t sys_rozos_netinfo(struct interrupt_frame *f, uint64_t index, uint64_t uinfo, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct rozos_netinfo info;
    int r;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    r = net_get_info((int)index, &info);
    if (r < 0) return r;
    return copy_to_user((void *)(uintptr_t)uinfo, &info, sizeof(info));
}

/* rozOS: হাতে ঠিকানা বসানো (ip = 0 → আবার DHCP) */
static int64_t sys_rozos_netconfig(struct interrupt_frame *f, uint64_t ip, uint64_t mask, uint64_t gw,
                                   uint64_t dns, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a5; (void)a6;
    return net_configure((uint32_t)ip, (uint32_t)mask, (uint32_t)gw, (uint32_t)dns);
}

/* ---------- ধাপ ১৪: AF_UNIX — আগে Unix socket কিনা দেখা, নইলে আগের (lwIP) পথ ---------- */

static struct vnode *unix_fd(int fd)
{
    struct vnode *vn = vfs_fd_vnode(fd);
    if (vn && unix_is_vnode(vn)) return vn;
    if (vn) vnode_unref(vn);
    return NULL;
}

/* struct sockaddr_un {sun_family; sun_path[108]} থেকে পথ */
static int get_unix_path(uint64_t uaddr, uint64_t ulen, char *path)
{
    uint8_t sa[2 + UNIX_PATH_MAX + 1];
    size_t n;
    if (ulen < 3 || ulen > sizeof(sa) - 1) return -EINVAL;
    memset(sa, 0, sizeof(sa));
    if (copy_from_user(sa, (const void *)(uintptr_t)uaddr, ulen) < 0) return -EFAULT;
    if (sa[0] != AF_UNIX_K || sa[1] != 0) return -EAFNOSUPPORT;
    for (n = 0; n < UNIX_PATH_MAX - 1 && sa[2 + n]; n++) path[n] = (char)sa[2 + n];
    path[n] = '\0';
    return n ? 0 : -EINVAL;
}

static int64_t sys_socket_x(struct interrupt_frame *f, uint64_t domain, uint64_t type, uint64_t proto,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    if (domain == AF_UNIX_K) {
        struct vnode *vn;
        int r = unix_create((int)type, &vn);
        (void)proto;
        return r < 0 ? r : install(vn);
    }
    return sys_socket(f, domain, type, proto, a4, a5, a6);
}

static int64_t sys_connect_x(struct interrupt_frame *f, uint64_t fd, uint64_t uaddr, uint64_t len,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn = unix_fd((int)fd);
    if (vn) {
        char path[UNIX_PATH_MAX];
        int r = get_unix_path(uaddr, len, path);
        if (r == 0) r = unix_connect(vn, path);
        vnode_unref(vn);
        return r;
    }
    return sys_connect(f, fd, uaddr, len, a4, a5, a6);
}

static int64_t sys_bind_x(struct interrupt_frame *f, uint64_t fd, uint64_t uaddr, uint64_t len,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn = unix_fd((int)fd);
    if (vn) {
        char path[UNIX_PATH_MAX];
        int r = get_unix_path(uaddr, len, path);
        if (r == 0) r = unix_bind(vn, path);
        vnode_unref(vn);
        return r;
    }
    return sys_bind(f, fd, uaddr, len, a4, a5, a6);
}

static int64_t sys_listen_x(struct interrupt_frame *f, uint64_t fd, uint64_t backlog, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn = unix_fd((int)fd);
    if (vn) {
        int r = unix_listen(vn, (int)backlog);
        vnode_unref(vn);
        return r;
    }
    return sys_listen(f, fd, backlog, a3, a4, a5, a6);
}

static int64_t sys_accept_x(struct interrupt_frame *f, uint64_t fd, uint64_t uaddr, uint64_t ulen,
                            uint64_t flags, uint64_t a5, uint64_t a6)
{
    struct vnode *vn = unix_fd((int)fd), *cvn;
    if (vn) {
        int r = unix_accept(vn, &cvn, 0);
        vnode_unref(vn);
        if (r < 0) return r;
        if (flags & 04000) unix_set_nonblock(cvn, 1);     /* accept4(SOCK_NONBLOCK) */
        if (uaddr && ulen) {
            uint32_t z = 2;
            uint16_t fam = AF_UNIX_K;
            copy_to_user((void *)(uintptr_t)uaddr, &fam, sizeof(fam));
            copy_to_user((void *)(uintptr_t)ulen, &z, sizeof(z));
        }
        return install(cvn);
    }
    return sys_accept(f, fd, uaddr, ulen, flags, a5, a6);
}

static int64_t sys_sendto_x(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                            uint64_t flags, uint64_t uaddr, uint64_t alen)
{
    struct vnode *vn = unix_fd((int)fd);
    if (vn) {
        void *kbuf;
        int64_t r;
        if (len > IO_MAX) len = IO_MAX;
        kbuf = kmalloc(len ? len : 1);
        if (!kbuf) { vnode_unref(vn); return -ENOMEM; }
        if (copy_from_user(kbuf, (const void *)(uintptr_t)ubuf, len) < 0) r = -EFAULT;
        else r = unix_send(vn, kbuf, len, (flags & MSG_DONTWAIT) != 0);
        kfree(kbuf);
        vnode_unref(vn);
        return r;
    }
    return sys_sendto(f, fd, ubuf, len, flags, uaddr, alen);
}

static int64_t sys_recvfrom_x(struct interrupt_frame *f, uint64_t fd, uint64_t ubuf, uint64_t len,
                              uint64_t flags, uint64_t uaddr, uint64_t ulenp)
{
    struct vnode *vn = unix_fd((int)fd);
    if (vn) {
        void *kbuf;
        int64_t r;
        if (len > IO_MAX) len = IO_MAX;
        if (!user_range_ok(ubuf, len, 1)) { vnode_unref(vn); return -EFAULT; }
        kbuf = kmalloc(len ? len : 1);
        if (!kbuf) { vnode_unref(vn); return -ENOMEM; }
        r = unix_recv(vn, kbuf, len, (flags & MSG_DONTWAIT) != 0);
        if (r > 0 && copy_to_user((void *)(uintptr_t)ubuf, kbuf, (size_t)r) < 0) r = -EFAULT;
        kfree(kbuf);
        vnode_unref(vn);
        return r;
    }
    return sys_recvfrom(f, fd, ubuf, len, flags, uaddr, ulenp);
}

static int64_t sys_shutdown_x(struct interrupt_frame *f, uint64_t fd, uint64_t how, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn = unix_fd((int)fd);
    if (vn) {
        int r = unix_shutdown(vn, (int)how);
        vnode_unref(vn);
        return r;
    }
    return sys_shutdown(f, fd, how, a3, a4, a5, a6);
}

static int64_t sys_sockopt_x(struct interrupt_frame *f, uint64_t fd, uint64_t level, uint64_t name,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct vnode *vn = unix_fd((int)fd);
    if (vn) {
        vnode_unref(vn);
        return 0;                            /* Unix socket-এ বিকল্প উপেক্ষা */
    }
    return sys_setsockopt(f, fd, level, name, a4, a5, a6);
}

void sys_net_register(void)
{
    syscall_register(SYS_socket, sys_socket_x, "socket");
    syscall_register(SYS_connect, sys_connect_x, "connect");
    syscall_register(SYS_accept, sys_accept_x, "accept");
    syscall_register(SYS_accept4, sys_accept_x, "accept4");
    syscall_register(SYS_sendto, sys_sendto_x, "sendto");
    syscall_register(SYS_recvfrom, sys_recvfrom_x, "recvfrom");
    syscall_register(SYS_shutdown, sys_shutdown_x, "shutdown");
    syscall_register(SYS_bind, sys_bind_x, "bind");
    syscall_register(SYS_listen, sys_listen_x, "listen");
    syscall_register(SYS_getsockname, sys_getsockname, "getsockname");
    syscall_register(SYS_getpeername, sys_getpeername, "getpeername");
    syscall_register(SYS_setsockopt, sys_sockopt_x, "setsockopt");
    syscall_register(SYS_getsockopt, sys_getsockopt, "getsockopt");
    syscall_register(SYS_rozos_netinfo, sys_rozos_netinfo, "rozos_netinfo");
    syscall_register(SYS_rozos_netconfig, sys_rozos_netconfig, "rozos_netconfig");
}
