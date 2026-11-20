/* rozOS — নেটওয়ার্ক: lwIP-এর সাথে যোগ, netd থ্রেড, DHCP (rozos/net.h দেখুন)
 *
 * প্রতিটি NIC একটা lwIP netif। ফ্রেম পাঠানো: lwIP → netif_output → dev->transmit।
 * ফ্রেম পাওয়া: netd → dev->poll → pbuf → netif->input (ethernet_input)।
 * DHCP-তে ঠিকানা পেলে DNS সার্ভার /etc/resolv.conf-এ লেখা হয় (rozlibc-র resolver পড়ে)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/net.h>
#include <rozos/sched.h>
#include <rozos/timer.h>
#include <rozos/vfs.h>
#include <rozos/heap.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <arch/x86_64/cpu.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#define NET_MAX_DEVICES 4

struct nic {
    struct net_device *dev;
    struct netif netif;
    int dhcp;
    int announced;                         /* ঠিকানা পাওয়ার বার্তা দেখানো হয়েছে */
};

static struct nic nics[NET_MAX_DEVICES];
static int nic_count;
static int lwip_ready;
static uint8_t rx_frame[NET_FRAME_MAX + 64];
static uint32_t rand_state = 0x2545F491;

/* ---------- lwIP-এর প্রয়োজনীয় প্ল্যাটফর্ম ফাংশন ---------- */

uint32_t sys_now(void)
{
    return (uint32_t)timer_uptime_ms();
}

uint32_t rozos_net_rand(void)
{
    /* xorshift, বুটের TSC দিয়ে মেশানো */
    rand_state ^= (uint32_t)cpu_rdtsc();
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return rand_state;
}

/* ---------- netif ---------- */

static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    struct nic *n = (struct nic *)netif->state;
    static uint8_t frame[NET_FRAME_MAX];
    uint16_t len;

    if (p->tot_len > sizeof(frame)) {
        return ERR_BUF;
    }
    len = pbuf_copy_partial(p, frame, p->tot_len, 0);
    if (n->dev->transmit(n->dev, frame, len) < 0) {
        return ERR_IF;
    }
    return ERR_OK;
}

static err_t nic_netif_init(struct netif *netif)
{
    struct nic *n = (struct nic *)netif->state;
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = netif_output;
    netif->mtu = NET_MTU;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, n->dev->mac, 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

int net_register_device(struct net_device *dev)
{
    if (nic_count >= NET_MAX_DEVICES) {
        return -ENOSPC;
    }
    memset(dev->name, 0, sizeof(dev->name));
    dev->name[0] = 'e';
    dev->name[1] = 't';
    dev->name[2] = 'h';
    dev->name[3] = (char)('0' + nic_count);
    nics[nic_count++].dev = dev;
    return 0;
}

/* ---------- DNS → /etc/resolv.conf ---------- */

static void write_resolv_conf(void)
{
    const ip_addr_t *d = dns_getserver(0);
    char buf[64];
    int fd, n;

    if (!d || ip_addr_isany(d)) {
        return;
    }
    n = ksnprintf(buf, sizeof(buf), "nameserver %s\n", ipaddr_ntoa(d));
    fd = vfs_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        vfs_write(fd, buf, (size_t)n);
        vfs_close(fd);
    }
}

static void announce(struct nic *n)
{
    char ip[16], gw[16];
    struct netif *nf = &n->netif;
    strncpy(ip, ipaddr_ntoa(&nf->ip_addr), sizeof(ip) - 1);
    ip[15] = '\0';
    strncpy(gw, ipaddr_ntoa(&nf->gw), sizeof(gw) - 1);
    gw[15] = '\0';
    {
        uint32_t m = lwip_ntohl(ip_2_ip4(&nf->netmask)->addr);
        unsigned bits = 0;
        while (m & 0x80000000u) {
            bits++;
            m <<= 1;
        }
        kprintf("[net] %s: %s/%u via %s (%s)\n", n->dev->name, ip, bits, gw,
                n->dhcp ? "DHCP" : "static");
    }
    write_resolv_conf();
}

/* ---------- netd ---------- */

static int netd(void *arg)
{
    int i;
    (void)arg;
    for (;;) {
        for (i = 0; i < nic_count; i++) {
            struct nic *n = &nics[i];
            size_t len;
            int link = n->dev->link_status ? n->dev->link_status(n->dev) : 1;

            if (link != n->dev->link_up) {
                n->dev->link_up = link;
                if (link) netif_set_link_up(&n->netif);
                else netif_set_link_down(&n->netif);
            }
            /* এক ঘুরে সর্বোচ্চ 64টা ফ্রেম, যাতে টাইমারও সময় পায় */
            {
                int budget = 64;
                while (budget-- > 0 && (len = n->dev->poll(n->dev, rx_frame, sizeof(rx_frame))) > 0) {
                    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
                    if (!p) {
                        n->dev->rx_dropped++;
                        continue;
                    }
                    pbuf_take(p, rx_frame, (u16_t)len);
                    if (n->netif.input(p, &n->netif) != ERR_OK) {
                        pbuf_free(p);
                    }
                }
            }
            if (!n->announced && !ip4_addr_isany_val(*netif_ip4_addr(&n->netif))) {
                n->announced = 1;
                announce(n);
            }
        }
        netif_poll_all();                        /* loopback-এর জমা প্যাকেট */
        sys_check_timeouts();
        sched_sleep_ms(1);
    }
    return 0;
}

int net_init(void)
{
    int i, cards;

    cards = e1000_init();
    if (nic_count == 0) {
        return 0;
    }
    lwip_init();
    lwip_ready = 1;
    for (i = 0; i < nic_count; i++) {
        struct nic *n = &nics[i];
        netif_add(&n->netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, n, nic_netif_init,
                  ethernet_input);
        netif_set_hostname(&n->netif, "rozos");
        if (i == 0) {
            netif_set_default(&n->netif);
        }
        netif_set_up(&n->netif);
        if (n->dev->link_up) {
            netif_set_link_up(&n->netif);
        }
        n->dhcp = 1;
        dhcp_start(&n->netif);
    }
    if (!sched_create_kthread("netd", netd, NULL)) {
        kprintf("[WARN] net: could not start netd\n");
    }
    (void)cards;
    return nic_count;
}

/* ---------- তথ্য ও হাতে কনফিগার ---------- */

int net_get_info(int index, struct rozos_netinfo *out)
{
    struct nic *n;
    const ip_addr_t *d;

    if (index < 0 || index >= nic_count) {
        return -ENODEV;
    }
    n = &nics[index];
    memset(out, 0, sizeof(*out));
    strncpy(out->name, n->dev->name, sizeof(out->name) - 1);
    strncpy(out->driver, n->dev->driver ? n->dev->driver : "?", sizeof(out->driver) - 1);
    memcpy(out->mac, n->dev->mac, 6);
    out->link_up = (uint8_t)n->dev->link_up;
    out->dhcp = (uint8_t)n->dhcp;
    out->ip = ip_2_ip4(&n->netif.ip_addr)->addr;
    out->netmask = ip_2_ip4(&n->netif.netmask)->addr;
    out->gateway = ip_2_ip4(&n->netif.gw)->addr;
    out->configured = out->ip != 0;
    d = dns_getserver(0);
    out->dns = d ? ip_2_ip4(d)->addr : 0;
    out->rx_packets = n->dev->rx_packets;
    out->tx_packets = n->dev->tx_packets;
    out->rx_bytes = n->dev->rx_bytes;
    out->tx_bytes = n->dev->tx_bytes;
    out->rx_dropped = n->dev->rx_dropped;
    out->tx_errors = n->dev->tx_errors;
    return 0;
}

int net_configure(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns)
{
    struct nic *n;
    ip4_addr_t a, m, g;

    if (nic_count == 0) {
        return -ENODEV;
    }
    n = &nics[0];
    if (ip == 0) {
        n->dhcp = 1;
        n->announced = 0;
        netif_set_addr(&n->netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        dhcp_start(&n->netif);
        return 0;
    }
    dhcp_release_and_stop(&n->netif);
    n->dhcp = 0;
    a.addr = ip;
    m.addr = netmask;
    g.addr = gateway;
    netif_set_addr(&n->netif, &a, &m, &g);
    if (dns) {
        ip_addr_t d;
        ip_addr_set_ip4_u32(&d, dns);
        dns_setserver(0, &d);
    }
    n->announced = 0;                            /* netd বার্তা দেখাবে ও resolv.conf লিখবে */
    return 0;
}

int net_ready(void)
{
    return lwip_ready;
}
