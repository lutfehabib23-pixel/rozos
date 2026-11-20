/* rozOS — ifconfig: নেটওয়ার্ক কার্ডের ঠিকানা ও গণনা দেখা/বদলানো (ধাপ ৯)
 *
 *   ifconfig                                  সব কার্ড
 *   ifconfig eth0 <ip> [netmask <m>] [gw <g>] [dns <d>]   হাতে ঠিকানা
 *   ifconfig eth0 dhcp                        আবার DHCP */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <rozos.h>

static const char *ip_str(unsigned a)
{
    static char buf[4][16];
    static int k;
    struct in_addr x;
    x.s_addr = a;
    k = (k + 1) % 4;
    inet_ntop(AF_INET, &x, buf[k], sizeof(buf[k]));
    return buf[k];
}

static void show(const struct rozos_netinfo *n)
{
    printf("%s: %s  link %s  %s\n", n->name, n->driver, n->link_up ? "up" : "down",
           n->dhcp ? "(DHCP)" : "(static)");
    printf("    ether %02x:%02x:%02x:%02x:%02x:%02x\n", n->mac[0], n->mac[1], n->mac[2], n->mac[3],
           n->mac[4], n->mac[5]);
    if (n->configured) {
        printf("    inet %s  netmask %s  gateway %s  dns %s\n", ip_str(n->ip), ip_str(n->netmask),
               ip_str(n->gateway), ip_str(n->dns));
    } else {
        printf("    no address yet%s\n", n->dhcp ? " (waiting for DHCP)" : "");
    }
    printf("    RX %lu packets, %lu bytes, %lu dropped\n", n->rx_packets, n->rx_bytes, n->rx_dropped);
    printf("    TX %lu packets, %lu bytes, %lu errors\n", n->tx_packets, n->tx_bytes, n->tx_errors);
}

static int parse(const char *s, unsigned *out)
{
    struct in_addr a;
    if (!inet_aton(s, &a)) {
        fprintf(stderr, "ifconfig: bad address '%s'\n", s);
        return -1;
    }
    *out = a.s_addr;
    return 0;
}

int main(int argc, char **argv)
{
    struct rozos_netinfo n;
    int i;

    if (argc <= 2) {
        int shown = 0;
        for (i = 0; rozos_netinfo(i, &n) == 0; i++) {
            if (argc == 2 && strcmp(argv[1], n.name)) continue;
            if (shown++) printf("\n");
            show(&n);
        }
        if (!shown) {
            fprintf(stderr, "ifconfig: no network card%s\n", argc == 2 ? " with that name" : "");
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "eth0")) {
        fprintf(stderr, "ifconfig: only eth0 can be configured\n");
        return 1;
    }
    if (!strcmp(argv[2], "dhcp")) {
        if (rozos_netconfig(0, 0, 0, 0) < 0) { perror("ifconfig"); return 1; }
        printf("eth0: DHCP restarted - check again with `ifconfig` in a moment\n");
        return 0;
    }
    {
        unsigned ip, mask = 0, gw = 0, dns = 0;
        if (parse(argv[2], &ip) < 0) return 1;
        mask = htonl(0xFFFFFF00);
        for (i = 3; i + 1 < argc; i += 2) {
            unsigned *dst = !strcmp(argv[i], "netmask") ? &mask : !strcmp(argv[i], "gw") ? &gw :
                            !strcmp(argv[i], "dns") ? &dns : NULL;
            if (!dst) { fprintf(stderr, "ifconfig: unknown option '%s'\n", argv[i]); return 1; }
            if (parse(argv[i + 1], dst) < 0) return 1;
        }
        if (rozos_netconfig(ip, mask, gw, dns) < 0) { perror("ifconfig"); return 1; }
    }
    return 0;
}
