/* rozOS — নেটওয়ার্ক: lwIP-এর সাথে যোগ, netd থ্রেড, DHCP (rozos/net.h দেখুন)
 *
 * প্রতিটি NIC একটা lwIP netif। ফ্রেম পাঠানো: lwIP → netif_output → dev->transmit।
 * ফ্রেম পাওয়া: netd → dev->poll → pbuf → netif->input (ethernet_input)।
 * DHCP-তে ঠিকানা পেলে DNS সার্ভার /etc/resolv.conf-এ লেখা হয় (rozlibc-র resolver পড়ে)। */

#include <stdint.h>
#include <stdarg.h>
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
#include <rozos/random.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
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
    uint64_t dhcp_check_ms;                /* ধাপ ২২ঙ */
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

/* ---------- ধাপ ২২ঘ: ফ্রেম লগ (kmon `netlog`) ----------
 * শেষ ৩২টা আসা/যাওয়া ফ্রেমের সারাংশ: ধরন, MAC, IP, পোর্ট, DHCP বার্তার ধরন।
 * আসল PC-তে DHCP-র উত্তর আসছে কিনা দেখার জন্য। */
#define FLOG_N 32
struct flog {
    uint32_t ms;
    uint16_t len, type;                    /* type = ethertype */
    uint8_t dir;                           /* 'R' / 'T' */
    uint8_t proto, dhcp;                   /* IP প্রোটোকল; DHCP বার্তা (option 53) */
    uint8_t dst[6], src[6];
    uint8_t ip_src[4], ip_dst[4];
    uint16_t sport, dport;
};
static struct flog flog[FLOG_N];
static unsigned flog_pos, flog_total;
/* ধাপ ২২ঙ: DHCP-র ফ্রেম আলাদা (ব্যস্ত নেটওয়ার্কে অন্য ফ্রেমের ভিড়ে হারিয়ে যায়) আর
 * DHCP-র ঘটনার ইতিহাস (কখন চালু/বন্ধ, লিংক ওঠা-নামা, ঠিকানা পাওয়া) */
#define DLOG_N 16
#define EVT_N  16
static struct flog dlog[DLOG_N];
static unsigned dlog_pos, dlog_total;
static struct { uint32_t ms; char text[56]; } evt[EVT_N];
static unsigned evt_total;

static void net_event(const char *fmt, ...)
{
    va_list ap;
    unsigned i = evt_total++ % EVT_N;
    evt[i].ms = (uint32_t)timer_uptime_ms();
    va_start(ap, fmt);
    kvsnprintf(evt[i].text, sizeof(evt[i].text), fmt, ap);
    va_end(ap);
}
extern unsigned long rozos_dhcp_stat[8];  /* lwip/core/ipv4/dhcp.c */

static void flog_add(char dir, const uint8_t *f, size_t len)
{
    struct flog *e = &flog[flog_pos++ % FLOG_N];
    memset(e, 0, sizeof(*e));
    flog_total++;
    e->ms = (uint32_t)timer_uptime_ms();
    e->dir = (uint8_t)dir;
    e->len = (uint16_t)len;
    if (len < 14) return;
    memcpy(e->dst, f, 6);
    memcpy(e->src, f + 6, 6);
    e->type = (uint16_t)(f[12] << 8 | f[13]);
    if (e->type == 0x0806 && len >= 42) {                 /* ARP: প্রেরক আর খোঁজা IP */
        memcpy(e->ip_src, f + 28, 4);
        memcpy(e->ip_dst, f + 38, 4);
        e->proto = (uint8_t)f[21];                        /* 1 = request, 2 = reply */
    } else if (e->type == 0x0800 && len >= 34) {
        const uint8_t *ip = f + 14;
        size_t hl = (size_t)(ip[0] & 15) * 4;
        e->proto = ip[9];
        memcpy(e->ip_src, ip + 12, 4);
        memcpy(e->ip_dst, ip + 16, 4);
        if ((e->proto == 17 || e->proto == 6) && len >= 14 + hl + 4) {
            const uint8_t *u = ip + hl;
            e->sport = (uint16_t)(u[0] << 8 | u[1]);
            e->dport = (uint16_t)(u[2] << 8 | u[3]);
            /* DHCP: UDP 67/68, options 236+4 বাইট পরে (magic cookie) */
            if (e->proto == 17 && (e->sport == 67 || e->sport == 68)) {
                size_t o = 14 + hl + 8 + 240;
                while (o + 2 < len && f[o] != 255) {
                    if (f[o] == 0) { o++; continue; }
                    if (f[o] == 53) { e->dhcp = f[o + 2]; break; }
                    o += 2u + f[o + 1];
                }
                dlog[dlog_pos++ % DLOG_N] = *e;          /* ধাপ ২২ঙ */
                dlog_total++;
            }
        }
    }
}

static void ip_str(char *b, const uint8_t *ip)
{
    ksnprintf(b, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void flog_print(const struct flog *e)
{
    static const char *const dname[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK",
                                         "RELEASE", "INFORM" };
    {
        char a[16], b[16];
        ip_str(a, e->ip_src);
        ip_str(b, e->ip_dst);
        kprintf("  %6u.%03u %c %4u  %02x:%02x:%02x>%02x:%02x:%02x ", e->ms / 1000, e->ms % 1000, e->dir, e->len,
                e->src[3], e->src[4], e->src[5], e->dst[3], e->dst[4], e->dst[5]);
        if (e->type == 0x0806) {
            kprintf("ARP %s %s -> %s\n", e->proto == 1 ? "who-has" : "reply", a, b);
        } else if (e->type == 0x0800) {
            kprintf("IP %s -> %s ", a, b);
            if (e->proto == 17) kprintf("UDP %u->%u", e->sport, e->dport);
            else if (e->proto == 6) kprintf("TCP %u->%u", e->sport, e->dport);
            else if (e->proto == 1) kprintf("ICMP");
            else kprintf("proto %u", e->proto);
            if (e->dhcp) kprintf("  DHCP %s", e->dhcp <= 8 ? dname[e->dhcp] : "?");
            kprintf("\n");
        } else {
            kprintf("type %04x\n", e->type);
        }
    }
}

/* only_dhcp = 1: শুধু ঘটনা, DHCP ফ্রেম আর গণনা (kmon `netlog dhcp`) */
void net_log_dump(int only_dhcp)
{
    unsigned i, n;
    int k;
    n = evt_total < EVT_N ? evt_total : EVT_N;
    kprintf("  events (since boot, oldest first):\n");
    for (i = evt_total - n; i < evt_total; i++)
        kprintf("  %6u.%03u  %s\n", evt[i % EVT_N].ms / 1000, evt[i % EVT_N].ms % 1000, evt[i % EVT_N].text);
    n = dlog_total < DLOG_N ? dlog_total : DLOG_N;
    kprintf("  DHCP frames: %u (last %u below)\n", dlog_total, n);
    for (i = dlog_total - n; i < dlog_total; i++) flog_print(&dlog[i % DLOG_N]);
    if (!only_dhcp) {
        n = flog_total < FLOG_N ? flog_total : FLOG_N;
        kprintf("  all frames: %u (last %u below, oldest first)\n", flog_total, n);
        for (i = flog_total - n; i < flog_total; i++) flog_print(&flog[i % FLOG_N]);
    }
    kprintf("  DHCP replies: got %lu, short %lu, other MAC %lu, old xid %lu, bad options %lu,\n"
            "                offer taken %lu, ack->address %lu, nak/unexpected %lu\n",
            rozos_dhcp_stat[0], rozos_dhcp_stat[1], rozos_dhcp_stat[2], rozos_dhcp_stat[3],
            rozos_dhcp_stat[4], rozos_dhcp_stat[5], rozos_dhcp_stat[6], rozos_dhcp_stat[7]);
    for (k = 0; k < nic_count; k++) {
        struct dhcp *d = netif_dhcp_data(&nics[k].netif);
        kprintf("  %s: DHCP state %d, tries %d%s\n", nics[k].dev->name, d ? d->state : -1, d ? d->tries : 0,
                d ? " (6 selecting, 1 requesting, 10 bound, 12 backing off)" : " (DHCP off)");
    }
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
    flog_add('T', frame, len);                   /* ধাপ ২২ঘ */
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
        net_event("%s: address %s (%s)", n->dev->name, ip, n->dhcp ? "DHCP" : "by hand");
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
                net_event("%s: link %s", n->dev->name, link ? "up" : "down");   /* ধাপ ২২ঙ */
                if (link) netif_set_link_up(&n->netif);
                else netif_set_link_down(&n->netif);
            }
            /* ধাপ ২২ঙ: DHCP চাওয়া হয়েছে অথচ বন্ধ (হাতে ঠিকানা নয়) — প্রতি ৫ সেকেন্ডে দেখে আবার চালু */
            if (n->dhcp && link && timer_uptime_ms() - n->dhcp_check_ms >= 5000) {
                struct dhcp *dd = netif_dhcp_data(&n->netif);
                n->dhcp_check_ms = timer_uptime_ms();
                if (!dd || dd->state == DHCP_STATE_OFF) {
                    net_event("%s: DHCP was off - started again", n->dev->name);
                    dhcp_start(&n->netif);
                }
            }
            /* এক ঘুরে সর্বোচ্চ 64টা ফ্রেম, যাতে টাইমারও সময় পায় */
            {
                int budget = 64;
                while (budget-- > 0 && (len = n->dev->poll(n->dev, rx_frame, sizeof(rx_frame))) > 0) {
                    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
                    flog_add('R', rx_frame, len);    /* ধাপ ২২ঘ */
                    random_add_entropy(cpu_rdtsc() ^ ((uint64_t)len << 48));
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
    cards += rtl8168_init();                 /* ধাপ ২২ */
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
        net_event("%s: DHCP start at boot (link %s)", n->dev->name, n->dev->link_up ? "up" : "down");
        if (dhcp_start(&n->netif) != ERR_OK) net_event("%s: dhcp_start failed", n->dev->name);
    }
    if (!sched_create_kthread("netd", netd, NULL)) {
        kprintf("[WARN] net: could not start netd\n");
    }
    (void)cards;
    return nic_count;
}

/* ---------- তথ্য ও হাতে কনফিগার ---------- */

struct net_device *net_get_device(int index)
{
    return (index >= 0 && index < nic_count) ? nics[index].dev : NULL;
}

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
    {                                            /* ধাপ ২২ঘ */
        struct dhcp *dd = netif_dhcp_data(&n->netif);
        out->dhcp_state = dd ? dd->state : 0;
        out->dhcp_tries = dd ? dd->tries : 0;
    }
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
        net_event("%s: DHCP restarted (ifconfig dhcp)", n->dev->name);
        dhcp_start(&n->netif);
        return 0;
    }
    dhcp_release_and_stop(&n->netif);
    n->dhcp = 0;
    net_event("%s: address set by hand - DHCP off", n->dev->name);
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
