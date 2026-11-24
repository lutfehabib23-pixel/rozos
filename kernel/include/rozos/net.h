/* rozOS — নেটওয়ার্ক (ধাপ ৯)
 *
 * স্তর:  NIC ড্রাইভার (e1000, ধাপ ২২ থেকে rtl8168) → struct net_device → lwIP (kernel/net/lwip, IPv4)
 *        → socket স্তর (kernel/net/socket.c) → BSD socket syscall (Linux নম্বর)
 *
 * "netd" কার্নেল-থ্রেড প্রতি মিলিসেকেন্ডে ড্রাইভার poll করে প্যাকেট lwIP-তে দেয় আর
 * lwIP-এর টাইমার চালায়। ইন্টারাপ্ট ব্যবহার হয় না (PCI INTx রাউটিং-এর জন্য ACPI AML
 * লাগত; MSI পরে)। ঠিকানা DHCP দিয়ে; DNS সার্ভার /etc/resolv.conf-এ লেখা হয়। */
#ifndef ROZOS_NET_H
#define ROZOS_NET_H

#include <stdint.h>
#include <stddef.h>

#define NET_MTU      1500
#define NET_FRAME_MAX 1518

struct net_device {
    char name[8];                         /* "eth0" */
    const char *driver;                   /* "e1000" */
    uint8_t mac[6];
    int link_up;
    /* ফ্রেম পাঠানো (CRC ছাড়া); সফল হলে 0 */
    int (*transmit)(struct net_device *dev, const void *frame, size_t len);
    /* একটা পাওয়া ফ্রেম buf-এ কপি; ফেরত দৈর্ঘ্য, না থাকলে 0 */
    size_t (*poll)(struct net_device *dev, void *buf, size_t max);
    int (*link_status)(struct net_device *dev);
    void *priv;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
    /* ধাপ ২২: kmon `nic`-এর জন্য ড্রাইভারের নিজস্ব বিবরণ (না থাকলে NULL) */
    void (*diag)(struct net_device *dev);
};

int  net_register_device(struct net_device *dev);
int  e1000_init(void);                    /* পাওয়া কার্ডের সংখ্যা */
int  rtl8168_init(void);                  /* ধাপ ২২: Realtek RTL8111/8168 */
/* index-তম কার্ড (kmon `nic`); না থাকলে NULL */
struct net_device *net_get_device(int index);

/* বুটে: ড্রাইভার, lwIP, DHCP, netd থ্রেড। ফেরত NIC-এর সংখ্যা */
int  net_init(void);

/* ifconfig/kmon-এর জন্য (ঠিকানাগুলো network byte order) */
struct rozos_netinfo {
    char     name[8];
    char     driver[16];
    uint8_t  mac[6];
    uint8_t  link_up;
    uint8_t  configured;                  /* ঠিকানা আছে */
    uint8_t  dhcp;                        /* 1 = DHCP, 0 = হাতে বসানো */
    uint8_t  reserved[3];
    uint32_t ip, netmask, gateway, dns;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
};

int net_get_info(int index, struct rozos_netinfo *out);   /* 0 = পাওয়া গেছে, -ENODEV = নেই */
/* হাতে ঠিকানা বসানো (ip = 0 হলে আবার DHCP) */
int net_configure(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns);

/* socket স্তর (socket.c) — syscall/sys_net.c ব্যবহার করে */
struct vnode;
struct sock;
int  sock_create(int domain, int type, int protocol, struct vnode **out);
struct sock *sock_from_vnode(struct vnode *vn);
int  sock_connect(struct sock *s, uint32_t ip, uint16_t port);
int  sock_bind(struct sock *s, uint32_t ip, uint16_t port);
int  sock_listen(struct sock *s, int backlog);
int  sock_accept(struct sock *s, struct vnode **out, uint32_t *ip, uint16_t *port);
int64_t sock_send(struct sock *s, const void *buf, size_t len, int flags,
                  int has_addr, uint32_t ip, uint16_t port);
int64_t sock_recv(struct sock *s, void *buf, size_t len, int flags,
                  uint32_t *ip, uint16_t *port);
int  sock_shutdown(struct sock *s, int how);
int  sock_name(struct sock *s, int peer, uint32_t *ip, uint16_t *port);
int  sock_setopt(struct sock *s, int level, int name, const void *val, size_t len);
int  sock_getopt(struct sock *s, int level, int name, void *val, size_t *len);

/* Linux-এর মান */
#define AF_INET        2
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000
#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_ERROR       4
#define SO_BROADCAST   6
#define SO_KEEPALIVE   9
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define MSG_PEEK       0x2
#define MSG_DONTWAIT   0x40

#endif
