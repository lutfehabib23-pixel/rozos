/* rozOS — ping: ICMP echo দিয়ে কোনো কম্পিউটার সাড়া দেয় কিনা দেখা (ধাপ ৯)
 *
 *   ping [-c count] host       (host = নাম বা ঠিকানা; ডিফল্ট ৪ বার)
 *
 * raw ICMP socket: পাঠাই ICMP হেডার + ডেটা, পাই IP হেডার সহ পুরো প্যাকেট। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <rozos.h>

struct icmp_echo {
    unsigned char type, code;
    unsigned short checksum, id, seq;
    unsigned char data[48];
};

static unsigned short checksum(const void *buf, int len)
{
    const unsigned short *p = (const unsigned short *)buf;
    unsigned long sum = 0;
    for (; len > 1; len -= 2) sum += *p++;
    if (len) sum += *(const unsigned char *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

int main(int argc, char **argv)
{
    struct sockaddr_in to;
    struct hostent *he;
    struct timeval tv;
    int fd, count = 4, i, received = 0, opt;
    long min = -1, max = 0, total = 0;
    unsigned short id = (unsigned short)getpid();
    const char *host;

    while ((opt = getopt(argc, argv, "c:")) != -1) {
        if (opt == 'c') count = atoi(optarg);
        else return 2;
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: ping [-c count] <host>\n");
        return 2;
    }
    host = argv[optind];
    he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "ping: %s: %s\n", host, h_errno == TRY_AGAIN ? "DNS did not answer" : "unknown host");
        return 2;
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    memcpy(&to.sin_addr, he->h_addr, 4);

    fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        perror("ping: socket");
        return 2;
    }
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    printf("PING %s (%s): %d data bytes\n", host, inet_ntoa(to.sin_addr), (int)sizeof(((struct icmp_echo *)0)->data));

    for (i = 0; i < count; i++) {
        struct icmp_echo req;
        unsigned char reply[1500];
        long t0 = rozos_now_ms(), rtt;
        int got = 0;

        memset(&req, 0, sizeof(req));
        req.type = 8;                               /* echo request */
        req.id = htons(id);
        req.seq = htons((unsigned short)i);
        memcpy(req.data, "rozOS ping - hello from ring 3 ! 0123456789abcd", sizeof(req.data));
        req.checksum = checksum(&req, sizeof(req));
        if (sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            perror("ping: sendto");
            break;
        }
        /* নিজের id আর seq-এর উত্তর না আসা পর্যন্ত (বা ১ সেকেন্ড) */
        while (rozos_now_ms() - t0 < 1000) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, &fl);
            int ihl;
            struct icmp_echo *r;
            if (n < 0) break;
            ihl = (reply[0] & 0x0F) * 4;
            if (n < ihl + 8) continue;
            r = (struct icmp_echo *)(reply + ihl);
            if (r->type != 0 || ntohs(r->id) != id || ntohs(r->seq) != (unsigned short)i) continue;
            rtt = rozos_now_ms() - t0;
            printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%ld ms\n", (int)(n - ihl),
                   inet_ntoa(from.sin_addr), i, reply[8], rtt);
            received++;
            total += rtt;
            if (min < 0 || rtt < min) min = rtt;
            if (rtt > max) max = rtt;
            got = 1;
            break;
        }
        if (!got) printf("Request timeout for icmp_seq %d\n", i);
        if (i + 1 < count) {
            long left = 1000 - (rozos_now_ms() - t0);
            if (left > 0) usleep((unsigned long)left * 1000);
        }
    }
    close(fd);
    printf("--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n", host, count,
           received, count ? (count - received) * 100 / count : 0);
    if (received) printf("round-trip min/avg/max = %ld/%ld/%ld ms\n", min, total / received, max);
    return received ? 0 : 1;
}
