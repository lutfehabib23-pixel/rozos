/* rozOS — rozhttp: http/https ক্লায়েন্ট (rozhttp.h দেখুন)
 *
 * TLS: BearSSL-এর "full" client প্রোফাইল (TLS 1.0-1.2, ECDHE/RSA, AES-GCM/CBC,
 * ChaCha20-Poly1305)। বিশ্বস্ত CA-গুলো /etc/ssl/certs.pem (PEM) থেকে প্রথম https
 * সংযোগে একবার পড়া হয়। সার্টিফিকেটের মেয়াদ যাচাইয়ে time() — rozOS-এর ঘড়ি।
 * TLS-এর র‍্যান্ডম আসে /dev/urandom থেকে (কার্নেলের ChaCha20 CSPRNG)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include "rozhttp.h"

#ifdef ROZOS_HAVE_BEARSSL
#include <bearssl.h>
#endif

#define CA_FILE    "/etc/ssl/certs.pem"
#define HDR_MAX    (32 * 1024)
/* ধাপ ১৯ছ বাগ-ফিক্স: হেডারের সাথে শরীরের যতটা আগেই এসেছে (HDR_MAX পর্যন্ত) সব শরীরের বাফারে
 * আঁটতে হবে — আগে বাফার ১৬ KiB ছিল, rozOS-এ এক read()-এ ২২ KiB এলে উপচে পাশের সংযোগ-তথ্য
 * (fd, tls) নষ্ট করত (বড় http ডাউনলোডে segfault বা আটকে যাওয়া) */
#define BODY_BUF   (HDR_MAX + 16384)

/* ---------- সংযোগ (সাধারণ TCP বা TLS) ---------- */

struct conn {
    int fd;
    int tls;
#ifdef ROZOS_HAVE_BEARSSL
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
#endif
};

#ifdef ROZOS_HAVE_BEARSSL

/* ---------- বিশ্বস্ত CA (trust anchor) ---------- */

static br_x509_trust_anchor *anchors;
static size_t n_anchors, cap_anchors;
static int anchors_loaded;

struct bytes {
    unsigned char *p;
    size_t len, cap;
};

static void bytes_append(void *ctx, const void *data, size_t len)
{
    struct bytes *b = (struct bytes *)ctx;
    if (b->len + len > b->cap) {
        size_t nc = (b->len + len) * 2 + 256;
        unsigned char *np = (unsigned char *)realloc(b->p, nc);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
}

static unsigned char *dup_bytes(const unsigned char *p, size_t n)
{
    unsigned char *d = (unsigned char *)malloc(n ? n : 1);
    if (d) memcpy(d, p, n);
    return d;
}

/* একটা DER সার্টিফিকেট থেকে trust anchor (DN + পাবলিক কী) */
static void add_anchor(const unsigned char *der, size_t len)
{
    br_x509_decoder_context dc;
    struct bytes dn = { NULL, 0, 0 };
    br_x509_pkey *pk;
    br_x509_trust_anchor *ta;

    br_x509_decoder_init(&dc, bytes_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || !dn.p) {
        free(dn.p);
        return;
    }
    if (n_anchors == cap_anchors) {
        size_t nc = cap_anchors ? cap_anchors * 2 : 64;
        br_x509_trust_anchor *na = (br_x509_trust_anchor *)realloc(anchors, nc * sizeof(*na));
        if (!na) { free(dn.p); return; }
        anchors = na;
        cap_anchors = nc;
    }
    ta = &anchors[n_anchors];
    memset(ta, 0, sizeof(*ta));
    ta->dn.data = dn.p;
    ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = dup_bytes(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = dup_bytes(pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = dup_bytes(pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    } else {
        free(dn.p);
        return;
    }
    n_anchors++;
}

static void load_anchors(void)
{
    FILE *f;
    br_pem_decoder_context pc;
    struct bytes der = { NULL, 0, 0 };
    unsigned char buf[4096];
    size_t n;
    int in_cert = 0;

    if (anchors_loaded) return;
    anchors_loaded = 1;
    f = fopen(CA_FILE, "r");
    if (!f) return;
    br_pem_decoder_init(&pc);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        unsigned char *p = buf;
        while (n > 0) {
            size_t used = br_pem_decoder_push(&pc, p, n);
            p += used;
            n -= used;
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                in_cert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") ||
                          !strcmp(br_pem_decoder_name(&pc), "X509 CERTIFICATE");
                der.len = 0;
                br_pem_decoder_setdest(&pc, in_cert ? bytes_append : NULL, in_cert ? &der : NULL);
                break;
            case BR_PEM_END_OBJ:
                if (in_cert && der.len) add_anchor(der.p, der.len);
                in_cert = 0;
                break;
            case BR_PEM_ERROR:
                in_cert = 0;
                br_pem_decoder_init(&pc);
                break;
            default:
                break;
            }
        }
    }
    free(der.p);
    fclose(f);
}

int rozhttp_trust_anchor_count(void)
{
    load_anchors();
    return (int)n_anchors;
}

int rozhttp_tls_available(void) { return 1; }

static const char *tls_error_name(int e)
{
    switch (e) {
    case BR_ERR_X509_EXPIRED:         return "certificate expired or not yet valid (is the clock right? try `date`)";
    case BR_ERR_X509_NOT_TRUSTED:     return "certificate not signed by a trusted CA (/etc/ssl/certs.pem)";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate is for a different host name";
    case BR_ERR_BAD_VERSION:          return "server does not speak TLS 1.0-1.2";
    case BR_ERR_BAD_CIPHER_SUITE:     return "no common cipher suite";
    case BR_ERR_IO:                   return "connection closed during the handshake";
    case BR_ERR_X509_UNSUPPORTED:     return "unsupported certificate";
    case BR_ERR_X509_LIMIT_EXCEEDED:  return "certificate chain too long";
    }
    return NULL;
}

static int sock_read_cb(void *ctx, unsigned char *buf, size_t len)
{
    ssize_t n = read(*(int *)ctx, buf, len);
    return n > 0 ? (int)n : -1;
}

static int sock_write_cb(void *ctx, const unsigned char *buf, size_t len)
{
    ssize_t n = write(*(int *)ctx, buf, len);
    return n > 0 ? (int)n : -1;
}

#else

int rozhttp_trust_anchor_count(void) { return 0; }
int rozhttp_tls_available(void) { return 0; }

#endif /* ROZOS_HAVE_BEARSSL */

static int conn_write(struct conn *c, const void *buf, size_t len)
{
#ifdef ROZOS_HAVE_BEARSSL
    if (c->tls) {
        if (br_sslio_write_all(&c->ioc, buf, len) < 0) return -1;
        return br_sslio_flush(&c->ioc);
    }
#endif
    {
        const char *p = (const char *)buf;
        while (len > 0) {
            ssize_t n = write(c->fd, p, len);
            if (n <= 0) return -1;
            p += n;
            len -= (size_t)n;
        }
        return 0;
    }
}

/* > 0 বাইট, 0 = শেষ, < 0 = ত্রুটি */
static long conn_read(struct conn *c, void *buf, size_t len, char *err, size_t errlen)
{
#ifdef ROZOS_HAVE_BEARSSL
    if (c->tls) {
        int n = br_sslio_read(&c->ioc, buf, len);
        if (n > 0) return n;
        {
            int e = br_ssl_engine_last_error(&c->sc.eng);
            /* close_notify ছাড়া সংযোগ বন্ধও অনেক সার্ভার করে — শেষ হিসেবে ধরা */
            if (e == BR_ERR_OK || e == BR_ERR_IO) return 0;
            snprintf(err, errlen, "TLS error %d: %s", e, tls_error_name(e) ? tls_error_name(e) : "see bearssl_ssl.h");
            return -1;
        }
    }
#endif
    {
        ssize_t n = read(c->fd, buf, len);
        if (n < 0) snprintf(err, errlen, "%s", strerror(errno));
        return n;
    }
}

static void conn_close(struct conn *c)
{
#ifdef ROZOS_HAVE_BEARSSL
    if (c->tls) br_sslio_close(&c->ioc);
#endif
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* ---------- URL ---------- */

struct url {
    int https;
    char host[256];
    int port;
    char path[ROZHTTP_URL_MAX];
};

static int parse_url(const char *s, struct url *u, char *err, size_t errlen)
{
    const char *p, *hs;
    size_t hl;
    memset(u, 0, sizeof(*u));
    if (!strncmp(s, "https://", 8)) { u->https = 1; s += 8; u->port = 443; }
    else if (!strncmp(s, "http://", 7)) { s += 7; u->port = 80; }
    else { snprintf(err, errlen, "URL must start with http:// or https://"); return -1; }
    hs = s;
    p = s + strcspn(s, ":/?#");
    hl = (size_t)(p - hs);
    if (hl == 0 || hl >= sizeof(u->host)) { snprintf(err, errlen, "bad host name in URL"); return -1; }
    memcpy(u->host, hs, hl);
    u->host[hl] = '\0';
    if (*p == ':') {
        u->port = atoi(p + 1);
        p += 1 + strspn(p + 1, "0123456789");
    }
    if (*p == '/' || *p == '?') snprintf(u->path, sizeof(u->path), "%s%s", *p == '?' ? "/" : "", p);
    else strcpy(u->path, "/");
    u->path[strcspn(u->path, "#")] = '\0';
    return 0;
}

static struct conn *open_conn(const struct url *u, const struct rozhttp_opts *o, char *err, size_t errlen)
{
    struct addrinfo hints, *ai;
    struct conn *c;
    char port[16];
    int r;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", u->port);
    if ((r = getaddrinfo(u->host, port, &hints, &ai)) != 0) {
        snprintf(err, errlen, "%s: %s", u->host, gai_strerror(r));
        return NULL;
    }
    c = (struct conn *)calloc(1, sizeof(*c));
    if (!c) { freeaddrinfo(ai); snprintf(err, errlen, "out of memory"); return NULL; }
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (o->verbose) {
        fprintf(stderr, "connecting to %s (%s) port %d%s ...\n", u->host,
                inet_ntoa(((struct sockaddr_in *)ai->ai_addr)->sin_addr), u->port, u->https ? " (TLS)" : "");
    }
    if (c->fd < 0 || connect(c->fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        snprintf(err, errlen, "connect to %s:%d: %s", u->host, u->port, strerror(errno));
        freeaddrinfo(ai);
        conn_close(c);
        return NULL;
    }
    freeaddrinfo(ai);
    {
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (u->https) {
#ifdef ROZOS_HAVE_BEARSSL
        load_anchors();
        if (n_anchors == 0) {
            snprintf(err, errlen, "no trusted certificates in %s", CA_FILE);
            conn_close(c);
            return NULL;
        }
        c->tls = 1;
        br_ssl_client_init_full(&c->sc, &c->xc, anchors, n_anchors);
        br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);
        if (!br_ssl_client_reset(&c->sc, u->host, 0)) {
            snprintf(err, errlen, "TLS setup failed (error %d)", br_ssl_engine_last_error(&c->sc.eng));
            conn_close(c);
            return NULL;
        }
        br_sslio_init(&c->ioc, &c->sc.eng, sock_read_cb, &c->fd, sock_write_cb, &c->fd);
#else
        snprintf(err, errlen, "https needs BearSSL (rebuild after ./toolchain/get-bearssl.sh)");
        conn_close(c);
        return NULL;
#endif
    }
    return c;
}

/* ---------- একটা অনুরোধ ---------- */

static void emit(int fd, const void *p, size_t n)
{
    const char *s = (const char *)p;
    while (fd >= 0 && n > 0) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void body(const struct rozhttp_opts *o, int out_fd, struct rozhttp_result *r, const void *p, size_t n)
{
    if (!n) return;
    emit(out_fd, p, n);
    if (o->on_data) o->on_data(o->on_data_ctx, p, n);
    r->bytes += (long)n;
    if (o->progress && (r->bytes >> 16) != ((r->bytes - (long)n) >> 16)) {
        if (r->content_length > 0) {
            fprintf(stderr, "\r  %ld / %ld KiB", r->bytes / 1024, r->content_length / 1024);
        } else {
            fprintf(stderr, "\r  %ld KiB", r->bytes / 1024);
        }
    }
}

/* হেডারে name-এর মান (case না দেখে); না থাকলে NULL — out-এ কপি */
static const char *header_value(const char *hdr, const char *name, char *out, size_t size)
{
    size_t nl = strlen(name);
    const char *p = strstr(hdr, "\r\n");
    while (p && p[2]) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1, *e;
            size_t len;
            while (*v == ' ' || *v == '\t') v++;
            e = strstr(v, "\r\n");
            len = e ? (size_t)(e - v) : strlen(v);
            if (len >= size) len = size - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return out;
        }
        p = strstr(p, "\r\n");
    }
    return NULL;
}

/* একটা GET; ফেরত: 0 = শেষ, 1 = redirect (next_url-এ), -1 = ত্রুটি */
static int one_request(const struct url *u, int out_fd, const struct rozhttp_opts *o,
                       struct rozhttp_result *r, char *next_url)
{
    struct conn *c;
    char *hdr = (char *)malloc(HDR_MAX + 1), *buf = (char *)malloc(BODY_BUF), val[ROZHTTP_URL_MAX];
    size_t hl = 0;
    long n, left = -1;
    int chunked = 0, ret = -1;
    char *hend = NULL;

    if (!hdr || !buf) { snprintf(r->error, sizeof(r->error), "out of memory"); goto out_free; }
    c = open_conn(u, o, r->error, sizeof(r->error));
    if (!c) goto out_free;
    r->tls = u->https;
    {
        char req[ROZHTTP_URL_MAX + 512];
        int len;
        if ((u->https && u->port == 443) || (!u->https && u->port == 80)) {
            len = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\n", u->path, u->host);
        } else {
            len = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s:%d\r\n", u->path, u->host, u->port);
        }
        len += snprintf(req + len, sizeof(req) - (size_t)len,
                        "User-Agent: rozOS-rozhttp/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n");
        if (conn_write(c, req, (size_t)len) < 0) {
#ifdef ROZOS_HAVE_BEARSSL
            if (c->tls) {
                int e = br_ssl_engine_last_error(&c->sc.eng);
                snprintf(r->error, sizeof(r->error), "TLS handshake with %s failed: error %d (%s)", u->host, e,
                         tls_error_name(e) ? tls_error_name(e) : "see bearssl_ssl.h");
            } else
#endif
            snprintf(r->error, sizeof(r->error), "sending the request failed");
            goto out;
        }
    }

    /* হেডার: খালি লাইন (\r\n\r\n) পর্যন্ত */
    while (!(hend = (hl >= 4 ? strstr(hdr, "\r\n\r\n") : NULL))) {
        if (hl >= HDR_MAX) { snprintf(r->error, sizeof(r->error), "response headers too long"); goto out; }
        n = conn_read(c, hdr + hl, HDR_MAX - hl, r->error, sizeof(r->error));
        if (n < 0) goto out;
        if (n == 0) { snprintf(r->error, sizeof(r->error), "connection closed before the headers ended"); goto out; }
        hl += (size_t)n;
        hdr[hl] = '\0';
    }
    *hend = '\0';
    if (sscanf(hdr, "HTTP/%*d.%*d %d", &r->status) != 1) {
        snprintf(r->error, sizeof(r->error), "not an HTTP response");
        goto out;
    }
    emit(o->header_fd, hdr, strlen(hdr));
    emit(o->header_fd, "\r\n\r\n", 4);

    r->content_length = -1;
    if (header_value(hdr, "Content-Length", val, sizeof(val))) {
        r->content_length = atol(val);
        left = r->content_length;
    }
    if (header_value(hdr, "Transfer-Encoding", val, sizeof(val)) && strstr(val, "chunked")) {
        chunked = 1;
        left = -1;
    }
    if (!o->no_follow && r->status >= 300 && r->status < 400 && r->status != 304 &&
        header_value(hdr, "Location", val, sizeof(val))) {
        /* পরম, "//host/..." বা "/path" */
        if (!strncmp(val, "http://", 7) || !strncmp(val, "https://", 8)) {
            snprintf(next_url, ROZHTTP_URL_MAX, "%s", val);
        } else if (val[0] == '/' && val[1] == '/') {
            snprintf(next_url, ROZHTTP_URL_MAX, "%s:%s", u->https ? "https" : "http", val);
        } else {
            snprintf(next_url, ROZHTTP_URL_MAX, "%s://%s:%d%s%s", u->https ? "https" : "http", u->host, u->port,
                     val[0] == '/' ? "" : "/", val);
        }
        ret = 1;
        goto out;
    }
    if (o->no_body) { ret = 0; goto out; }

    /* শরীর: হেডারের পরে আগেই যা এসে গেছে, তারপর বাকিটা */
    {
        char *rest = hend + 4;
        size_t have = hl - (size_t)(rest - hdr);
        memmove(buf, rest, have);
        if (!chunked) {
            for (;;) {
                if (left >= 0 && (long)have > left) have = (size_t)left;
                body(o, out_fd, r, buf, have);
                if (left >= 0 && (left -= (long)have) <= 0) break;
                n = conn_read(c, buf, 16384, r->error, sizeof(r->error));
                if (n < 0) goto out;
                if (n == 0) {
                    if (left > 0) { snprintf(r->error, sizeof(r->error), "connection closed early (%ld bytes missing)", left); goto out; }
                    break;
                }
                have = (size_t)n;
            }
        } else {
            /* chunked: "<হেক্স আকার>\r\n<ডেটা>\r\n" ... "0\r\n\r\n" */
            size_t pos = 0;
            for (;;) {
                char *eol;
                unsigned long csize;
                while (!(eol = memchr(buf + pos, '\n', have - pos))) {
                    memmove(buf, buf + pos, have - pos);
                    have -= pos;
                    pos = 0;
                    if (have >= BODY_BUF - 384) { snprintf(r->error, sizeof(r->error), "bad chunk header"); goto out; }
                    n = conn_read(c, buf + have, BODY_BUF - have, r->error, sizeof(r->error));
                    if (n <= 0) { if (n == 0) snprintf(r->error, sizeof(r->error), "connection closed inside a chunk"); goto out; }
                    have += (size_t)n;
                }
                csize = strtoul(buf + pos, NULL, 16);
                pos = (size_t)(eol - buf) + 1;
                if (csize == 0) break;
                while (csize > 0) {
                    size_t take;
                    if (pos >= have) {
                        n = conn_read(c, buf, 16384, r->error, sizeof(r->error));
                        if (n <= 0) { if (n == 0) snprintf(r->error, sizeof(r->error), "connection closed inside a chunk"); goto out; }
                        have = (size_t)n;
                        pos = 0;
                    }
                    take = have - pos < csize ? have - pos : csize;
                    body(o, out_fd, r, buf + pos, take);
                    pos += take;
                    csize -= take;
                }
                /* ডেটার পরের \r\n — পরের লুপের লাইন-খোঁজায় খালি লাইন হিসেবে পার হয় */
                while (pos < have && (buf[pos] == '\r' || buf[pos] == '\n')) pos++;
            }
        }
    }
    ret = 0;
out:
    conn_close(c);
out_free:
    free(hdr);
    free(buf);
    return ret;
}

int rozhttp_get(const char *url, int out_fd, const struct rozhttp_opts *opts, struct rozhttp_result *r)
{
    struct rozhttp_opts o;
    char cur[ROZHTTP_URL_MAX], next[ROZHTTP_URL_MAX];
    int max;

    memset(r, 0, sizeof(*r));
    if (opts) o = *opts;
    else { memset(&o, 0, sizeof(o)); o.header_fd = -1; }
    max = o.max_redirects > 0 ? o.max_redirects : 5;
    snprintf(cur, sizeof(cur), "%s", url);
    for (;;) {
        struct url u;
        int st;
        if (parse_url(cur, &u, r->error, sizeof(r->error)) < 0) return -1;
        snprintf(r->final_url, sizeof(r->final_url), "%s", cur);
        r->bytes = 0;
        st = one_request(&u, out_fd, &o, r, next);
        if (st < 0) return -1;
        if (st == 0) break;
        if (++r->redirects > max) {
            snprintf(r->error, sizeof(r->error), "too many redirects");
            return -1;
        }
        if (o.verbose) fprintf(stderr, "redirect %d -> %s\n", r->status, next);
        snprintf(cur, sizeof(cur), "%s", next);
    }
    if (o.progress && r->bytes >= 65536) fprintf(stderr, "\n");
    if ((r->status < 200 || r->status >= 300) && !(o.no_follow && r->status >= 300 && r->status < 400)) {
        snprintf(r->error, sizeof(r->error), "server answered HTTP %d", r->status);
        return -1;
    }
    return 0;
}

/* ---------- SHA-256 (FIPS 180-4) ---------- */

struct sha_state {
    unsigned int h[8];
    unsigned long long len;
    unsigned char buf[64];
    unsigned int used;
};

typedef char sha_size_check[(sizeof(struct sha_state) <= sizeof(struct rozhttp_sha256)) ? 1 : -1];

static const unsigned int K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha_state *s, const unsigned char *p)
{
    unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned)p[4 * i] << 24) | ((unsigned)p[4 * i + 1] << 16) | ((unsigned)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        unsigned int s0 = RR(w[i - 15], 7) ^ RR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = RR(w[i - 2], 17) ^ RR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (RR(e, 6) ^ RR(e, 11) ^ RR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (RR(a, 2) ^ RR(a, 13) ^ RR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void rozhttp_sha256_init(struct rozhttp_sha256 *ctx)
{
    static const unsigned int init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    struct sha_state *s = (struct sha_state *)(void *)ctx;
    memset(s, 0, sizeof(*s));
    memcpy(s->h, init, sizeof(init));
}

void rozhttp_sha256_update(struct rozhttp_sha256 *ctx, const void *data, size_t len)
{
    struct sha_state *s = (struct sha_state *)(void *)ctx;
    const unsigned char *p = (const unsigned char *)data;
    s->len += len;
    while (len > 0) {
        size_t take = 64 - s->used < len ? 64 - s->used : len;
        memcpy(s->buf + s->used, p, take);
        s->used += (unsigned)take;
        p += take;
        len -= take;
        if (s->used == 64) {
            sha_block(s, s->buf);
            s->used = 0;
        }
    }
}

void rozhttp_sha256_hex(struct rozhttp_sha256 *ctx, char out[65])
{
    struct sha_state *s = (struct sha_state *)(void *)ctx;
    unsigned long long bits = s->len * 8;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    int i;
    rozhttp_sha256_update(ctx, &pad, 1);
    while (s->used != 56) rozhttp_sha256_update(ctx, &zero, 1);
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    rozhttp_sha256_update(ctx, lenb, 8);
    for (i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", (s->h[i / 4] >> (24 - 8 * (i % 4))) & 0xFF);
    out[64] = '\0';
}
