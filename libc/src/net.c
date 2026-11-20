/* rozlibc — socket wrapper, byte order, inet_*, DNS resolver (ধাপ ৯)
 *
 * নাম খোঁজার ক্রম: সংখ্যার ঠিকানা ("8.8.8.8") → /etc/hosts → DNS।
 * DNS: /etc/resolv.conf-এর "nameserver" (কার্নেল DHCP থেকে লেখে; না থাকলে 10.0.2.3,
 * QEMU-র DNS), UDP পোর্ট 53-এ A-রেকর্ডের প্রশ্ন, ২ সেকেন্ড অপেক্ষা, ৩ বার চেষ্টা। */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <rozos.h>

#define SYS_socket      41
#define SYS_connect     42
#define SYS_accept      43
#define SYS_sendto      44
#define SYS_recvfrom    45
#define SYS_shutdown    48
#define SYS_bind        49
#define SYS_listen      50
#define SYS_getsockname 51
#define SYS_getpeername 52
#define SYS_setsockopt  54
#define SYS_getsockopt  55

#define SC(n, a, b, c, d, e, f) __syscall_ret(__syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f)))

int socket(int d, int t, int p)                        { return (int)SC(SYS_socket, d, t, p, 0, 0, 0); }
int connect(int fd, const struct sockaddr *a, socklen_t l) { return (int)SC(SYS_connect, fd, a, l, 0, 0, 0); }
int bind(int fd, const struct sockaddr *a, socklen_t l)    { return (int)SC(SYS_bind, fd, a, l, 0, 0, 0); }
int listen(int fd, int b)                              { return (int)SC(SYS_listen, fd, b, 0, 0, 0, 0); }
int accept(int fd, struct sockaddr *a, socklen_t *l)   { return (int)SC(SYS_accept, fd, a, l, 0, 0, 0); }
int shutdown(int fd, int how)                          { return (int)SC(SYS_shutdown, fd, how, 0, 0, 0, 0); }
int getsockname(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC(SYS_getsockname, fd, a, l, 0, 0, 0); }
int getpeername(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC(SYS_getpeername, fd, a, l, 0, 0, 0); }
int setsockopt(int fd, int lv, int n, const void *v, socklen_t l) { return (int)SC(SYS_setsockopt, fd, lv, n, v, l, 0); }
int getsockopt(int fd, int lv, int n, void *v, socklen_t *l)      { return (int)SC(SYS_getsockopt, fd, lv, n, v, l, 0); }

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tl)
{
    return SC(SYS_sendto, fd, buf, len, flags, to, tl);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fl)
{
    return SC(SYS_recvfrom, fd, buf, len, flags, from, fl);
}

ssize_t send(int fd, const void *buf, size_t len, int flags) { return sendto(fd, buf, len, flags, NULL, 0); }
ssize_t recv(int fd, void *buf, size_t len, int flags)       { return recvfrom(fd, buf, len, flags, NULL, NULL); }

int rozos_netinfo(int index, struct rozos_netinfo *out)
{
    return (int)SC(ROZOS_SYS_NETINFO, index, out, 0, 0, 0, 0);
}

int rozos_netconfig(unsigned ip, unsigned mask, unsigned gw, unsigned dns)
{
    return (int)SC(ROZOS_SYS_NETCONFIG, ip, mask, gw, dns, 0, 0);
}

/* ---------- byte order (x86 = little endian) ---------- */

uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }
uint32_t htonl(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ---------- inet_* ---------- */

int inet_aton(const char *s, struct in_addr *out)
{
    unsigned long part[4];
    int n = 0;
    const char *p = s;
    for (;;) {
        char *end;
        if (!isdigit((unsigned char)*p)) return 0;
        part[n] = strtoul(p, &end, 10);
        if (part[n] > 255) return 0;
        n++;
        p = end;
        if (*p == '\0') break;
        if (*p != '.' || n == 4) return 0;
        p++;
    }
    if (n != 4) return 0;
    out->s_addr = htonl((uint32_t)((part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3]));
    return 1;
}

in_addr_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const unsigned char *b = (const unsigned char *)src;
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    if (snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]) >= (int)size) {
        errno = ENOSPC;
        return NULL;
    }
    return dst;
}

char *inet_ntoa(struct in_addr a)
{
    static char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

int inet_pton(int af, const char *s, void *dst)
{
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    return inet_aton(s, (struct in_addr *)dst);
}

/* ---------- নাম খোঁজা ---------- */

int h_errno;

static int lookup_hosts(const char *name, uint32_t *ip)
{
    FILE *f = fopen("/etc/hosts", "r");
    char line[256];
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *save, *addr, *tok;
        struct in_addr a;
        line[strcspn(line, "#\n")] = '\0';
        addr = strtok_r(line, " \t", &save);
        if (!addr || !inet_aton(addr, &a)) continue;
        while ((tok = strtok_r(NULL, " \t", &save))) {
            if (!strcasecmp(tok, name)) {
                *ip = a.s_addr;
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

static uint32_t nameserver(void)
{
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[128];
    struct in_addr a;
    a.s_addr = htonl(0x0A000203);                 /* 10.0.2.3: QEMU user-mode DNS */
    if (!f) return a.s_addr;
    while (fgets(line, sizeof(line), f)) {
        char ns[64];
        if (sscanf(line, "nameserver %63s", ns) == 1 && inet_aton(ns, &a)) break;
    }
    fclose(f);
    return a.s_addr;
}

/* DNS নাম লেখা: "www.google.com" → 3www6google3com0 */
static int encode_name(const char *name, unsigned char *out, int max)
{
    int n = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        int len = dot ? (int)(dot - name) : (int)strlen(name);
        if (len == 0 || len > 63 || n + len + 2 > max) return -1;
        out[n++] = (unsigned char)len;
        memcpy(out + n, name, (size_t)len);
        n += len;
        name += len;
        if (*name == '.') name++;
    }
    out[n++] = 0;
    return n;
}

/* উত্তরের ভেতরের নাম পার হওয়া (compression pointer সহ); ফেরত নতুন অবস্থান বা -1 */
static int skip_name(const unsigned char *m, int len, int pos)
{
    while (pos < len) {
        unsigned char c = m[pos];
        if (c == 0) return pos + 1;
        if ((c & 0xC0) == 0xC0) return pos + 2;
        pos += c + 1;
    }
    return -1;
}

static int dns_query(const char *name, uint32_t *ip)
{
    unsigned char q[512], r[512];
    struct sockaddr_in ns;
    struct timeval tv;
    int fd, qlen, n, tries;
    uint16_t id = (uint16_t)(rozos_now_ms() ^ getpid());

    memset(q, 0, 12);
    q[0] = (unsigned char)(id >> 8);
    q[1] = (unsigned char)id;
    q[2] = 0x01;                                  /* recursion desired */
    q[5] = 1;                                     /* ১টা প্রশ্ন */
    n = encode_name(name, q + 12, (int)sizeof(q) - 16);
    if (n < 0) return EAI_NONAME;
    qlen = 12 + n;
    q[qlen++] = 0; q[qlen++] = 1;                 /* type A */
    q[qlen++] = 0; q[qlen++] = 1;                 /* class IN */

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return EAI_SYSTEM;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&ns, 0, sizeof(ns));
    ns.sin_family = AF_INET;
    ns.sin_port = htons(53);
    ns.sin_addr.s_addr = nameserver();

    for (tries = 0; tries < 3; tries++) {
        int pos, an, i;
        if (sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&ns, sizeof(ns)) < 0) break;
        n = (int)recv(fd, r, sizeof(r), 0);
        if (n < 12) continue;                     /* সময় শেষ — আবার */
        if (r[0] != q[0] || r[1] != q[1]) continue;
        if ((r[3] & 0x0F) == 3) { close(fd); return EAI_NONAME; }   /* NXDOMAIN */
        if ((r[3] & 0x0F) != 0) { close(fd); return EAI_FAIL; }     /* SERVFAIL ইত্যাদি */
        an = (r[6] << 8) | r[7];
        pos = skip_name(r, n, 12);
        if (pos < 0) break;
        pos += 4;
        for (i = 0; i < an && pos > 0 && pos + 10 <= n; i++) {
            int type, rdlen;
            pos = skip_name(r, n, pos);
            if (pos < 0 || pos + 10 > n) break;
            type = (r[pos] << 8) | r[pos + 1];
            rdlen = (r[pos + 8] << 8) | r[pos + 9];
            pos += 10;
            if (type == 1 && rdlen == 4 && pos + 4 <= n) {      /* A রেকর্ড (CNAME পার হয়ে) */
                memcpy(ip, r + pos, 4);
                close(fd);
                return 0;
            }
            pos += rdlen;
        }
        close(fd);
        return EAI_NONAME;
    }
    close(fd);
    return EAI_AGAIN;
}

static int resolve(const char *name, uint32_t *ip)
{
    struct in_addr a;
    if (inet_aton(name, &a)) { *ip = a.s_addr; return 0; }
    if (!strcasecmp(name, "localhost")) { *ip = htonl(INADDR_LOOPBACK); return 0; }
    if (lookup_hosts(name, ip)) return 0;
    return dns_query(name, ip);
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static char namebuf[256];
    static uint32_t addr;
    static char *list[2];
    static char *aliases[1];
    int r = resolve(name, &addr);
    if (r != 0) {
        h_errno = r == EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
        return NULL;
    }
    strncpy(namebuf, name, sizeof(namebuf) - 1);
    list[0] = (char *)&addr;
    list[1] = NULL;
    aliases[0] = NULL;
    he.h_name = namebuf;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    return &he;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res)
{
    struct addrinfo *ai;
    struct sockaddr_in *sa;
    uint32_t ip = 0;
    long port = 0;
    int r;

    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) return EAI_FAMILY;
    if (service) {
        char *end;
        port = strtol(service, &end, 10);
        if (*end) {
            if (!strcmp(service, "http")) port = 80;
            else if (!strcmp(service, "https")) port = 443;
            else if (!strcmp(service, "domain")) port = 53;
            else return EAI_SERVICE;
        }
    }
    if (node) {
        if (hints && (hints->ai_flags & AI_NUMERICHOST)) {
            struct in_addr a;
            if (!inet_aton(node, &a)) return EAI_NONAME;
            ip = a.s_addr;
        } else if ((r = resolve(node, &ip)) != 0) {
            return r;
        }
    } else {
        ip = (hints && (hints->ai_flags & AI_PASSIVE)) ? INADDR_ANY : htonl(INADDR_LOOPBACK);
    }
    ai = (struct addrinfo *)calloc(1, sizeof(*ai) + sizeof(*sa));
    if (!ai) return EAI_MEMORY;
    sa = (struct sockaddr_in *)(ai + 1);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    sa->sin_addr.s_addr = ip;
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = sizeof(*sa);
    ai->ai_addr = (struct sockaddr *)sa;
    if (node && hints && (hints->ai_flags & AI_CANONNAME)) ai->ai_canonname = strdup(node);
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int err)
{
    switch (err) {
    case 0:           return "success";
    case EAI_NONAME:  return "name not found";
    case EAI_AGAIN:   return "DNS server did not answer (try again)";
    case EAI_FAIL:    return "DNS failure";
    case EAI_FAMILY:  return "address family not supported";
    case EAI_MEMORY:  return "out of memory";
    case EAI_SERVICE: return "unknown service";
    case EAI_SYSTEM:  return "system error";
    }
    return "unknown error";
}

/* ---------- ধাপ ১০: র‍্যান্ডম ---------- */

#include <sys/random.h>

ssize_t getrandom(void *buf, size_t len, unsigned flags)
{
    return __syscall_ret(__syscall3(318, buf, len, flags));
}

int getentropy(void *buf, size_t len)
{
    if (len > 256) {
        errno = EIO;
        return -1;
    }
    return getrandom(buf, len, 0) == (ssize_t)len ? 0 : -1;
}
