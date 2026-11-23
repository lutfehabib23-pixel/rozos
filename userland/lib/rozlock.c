/* rozOS — librozlock: ফোল্ডার লক আর লুকানো (ধাপ ১৯ছ) — বিবরণ rozlock.h-এ
 *
 * লক-ফাইলের গঠন:
 *   হেডার ৬৪ বাইট: "ROZLOCK1", u32 version (1), u32 PBKDF2-বার, u32 টুকরোর মাপ, u32 0,
 *                  salt[16], বাকি শূন্য
 *   তারপর টুকরো: u32 len, u8 last, ciphertext[len], tag[16]
 *     nonce = 0 0 0 0 + টুকরোর ক্রম (u64, little endian)
 *     AAD   = হেডার + ক্রম + len + last  → টুকরো বদলানো/সরানো/কাটা সব ধরা পড়ে
 *   শেষ টুকরোতে last = 1 (না পেলে ফাইল কাটা)।
 * খোলা ডেটার ভেতরে রেকর্ড:
 *   'R' u16 n নাম            — মূল ফোল্ডারের নাম
 *   'D' u16 n পথ u32 mode u64 mtime
 *   'F' u16 n পথ u32 mode u64 mtime u8 attr u64 size, তারপর size বাইট
 *   'E'                      — শেষ
 * সব সংখ্যা little endian। পথ ফোল্ডারের ভেতরের আপেক্ষিক ("a/b.txt")। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/random.h>
#include <rozos.h>
#include "rozlock.h"
#ifdef ROZOS_HAVE_BEARSSL
#include <bearssl.h>
#endif

#define RL_MAGIC   "ROZLOCK1"
#define RL_HDR     64
#define RL_CHUNK   65536
#define RL_ITER    100000
#define RL_PATH    1024

/* ================= ছোট সাহায্য ================= */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return w < 0 ? -errno : -EIO;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r < 0) return -errno;
        if (r == 0) return ROZLOCK_EFORMAT;      /* আগেই শেষ = কাটা ফাইল */
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static void join(char *out, size_t n, const char *a, const char *b)
{
    if (!a[0]) snprintf(out, n, "%s", b);
    else snprintf(out, n, "%s/%s", strcmp(a, "/") ? a : "", b);
}

/* "/x/y/name" → parent "/x/y", base "name" (শেষের '/' বাদ) */
static void split(const char *path, char *parent, size_t pn, char *base, size_t bn)
{
    char tmp[RL_PATH], *s;
    snprintf(tmp, sizeof(tmp), "%s", path);
    while (strlen(tmp) > 1 && tmp[strlen(tmp) - 1] == '/') tmp[strlen(tmp) - 1] = '\0';
    s = strrchr(tmp, '/');
    if (!s) { snprintf(parent, pn, "."); snprintf(base, bn, "%s", tmp); return; }
    snprintf(base, bn, "%s", s + 1);
    if (s == tmp) snprintf(parent, pn, "/");
    else { *s = '\0'; snprintf(parent, pn, "%s", tmp); }
}

static int has_suffix(const char *s, const char *x)
{
    size_t a = strlen(s), b = strlen(x);
    return a > b && !strcmp(s + a - b, x);
}

const char *rozlock_strerror(int err)
{
    switch (err) {
    case 0: return "OK";
    case ROZLOCK_EWRONG: return "Wrong secret";
    case ROZLOCK_EFORMAT: return "Not a locked folder, or the file is damaged";
    case ROZLOCK_ENOCRYPTO: return "rozlock was built without BearSSL (no encryption)";
    case ROZLOCK_ESYSTEM: return "System folders cannot be locked";
    case ROZLOCK_EPARTIAL: return "Locked, but some original files could not be removed";
    default: return strerror(err < 0 ? -err : err);
    }
}

void rozlock_display_name(const char *path, char *out, size_t outlen)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(out, outlen, "%s", b);
    if (has_suffix(out, ROZLOCK_EXT)) out[strlen(out) - strlen(ROZLOCK_EXT)] = '\0';
}

int rozlock_protect(const char *path, int on)
{
    int fd = open(path, O_RDONLY), fl = 0, r = 0;
    if (fd < 0) return -errno;
    if (ioctl(fd, FS_IOC_GETFLAGS, &fl) < 0) r = -errno;
    else {
        fl = on ? (fl | FS_IMMUTABLE_FL) : (fl & ~FS_IMMUTABLE_FL);
        if (ioctl(fd, FS_IOC_SETFLAGS, &fl) < 0) r = -errno;
    }
    close(fd);
    return r;
}

int rozlock_is_protected(const char *path)
{
    int fd = open(path, O_RDONLY), fl = 0;
    if (fd < 0) return 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &fl) < 0) fl = 0;
    close(fd);
    return (fl & FS_IMMUTABLE_FL) != 0;
}

int rozlock_is_lockfile(const char *path)
{
    char m[8];
    int fd = open(path, O_RDONLY), ok;
    if (fd < 0) return 0;
    ok = read(fd, m, 8) == 8 && !memcmp(m, RL_MAGIC, 8);
    close(fd);
    return ok;
}

/* ================= লুকানো (.hidden) ================= */

int rozhide_load(struct rozhide *h, const char *dir)
{
    char p[RL_PATH];
    int fd, cap = 16;
    struct stat st;
    char *s;
    memset(h, 0, sizeof(*h));
    join(p, sizeof(p), dir, ".hidden");
    fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0 || st.st_size > 1024 * 1024 || !(h->data = (char *)malloc((size_t)st.st_size + 1))) {
        close(fd);
        return 0;
    }
    if (read_all(fd, h->data, (size_t)st.st_size) < 0) st.st_size = 0;
    close(fd);
    h->data[st.st_size] = '\0';
    h->names = (char **)malloc(sizeof(char *) * (size_t)cap);
    for (s = h->data; h->names && *s;) {
        char *e = strchr(s, '\n');
        if (e) *e = '\0';
        if (s[0] && s[strlen(s) - 1] == '\r') s[strlen(s) - 1] = '\0';
        if (s[0]) {
            if (h->n == cap) {
                char **nn = (char **)realloc(h->names, sizeof(char *) * (size_t)(cap *= 2));
                if (!nn) break;
                h->names = nn;
            }
            h->names[h->n++] = s;
        }
        if (!e) break;
        s = e + 1;
    }
    return 0;
}

int rozhide_has(const struct rozhide *h, const char *name)
{
    int i;
    for (i = 0; i < h->n; i++) if (!strcmp(h->names[i], name)) return 1;
    return 0;
}

void rozhide_free(struct rozhide *h)
{
    free(h->names);
    free(h->data);
    memset(h, 0, sizeof(*h));
}

int rozhide_is_hidden(const char *path)
{
    char dir[RL_PATH], base[256];
    struct rozhide h;
    int r;
    split(path, dir, sizeof(dir), base, sizeof(base));
    rozhide_load(&h, dir);
    r = rozhide_has(&h, base);
    rozhide_free(&h);
    return r;
}

int rozhide_set(const char *path, int hidden)
{
    char dir[RL_PATH], base[256], p[RL_PATH], np[RL_PATH + 8];
    struct rozhide h;
    int i, fd, r = 0, left = 0;
    split(path, dir, sizeof(dir), base, sizeof(base));
    if (!base[0] || !strcmp(base, ".hidden")) return -EINVAL;
    rozhide_load(&h, dir);
    if (rozhide_has(&h, base) == !!hidden) { rozhide_free(&h); return 0; }
    join(p, sizeof(p), dir, ".hidden");
    snprintf(np, sizeof(np), "%s.new", p);
    fd = open(np, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { rozhide_free(&h); return -errno; }
    for (i = 0; i < h.n && r == 0; i++) {
        if (!hidden && !strcmp(h.names[i], base)) continue;
        if ((r = write_all(fd, h.names[i], strlen(h.names[i]))) == 0) r = write_all(fd, "\n", 1);
        left++;
    }
    if (hidden && r == 0) {
        if ((r = write_all(fd, base, strlen(base))) == 0) r = write_all(fd, "\n", 1);
        left++;
    }
    close(fd);
    rozhide_free(&h);
    if (r < 0) { unlink(np); return r; }
    if (!left) { unlink(np); unlink(p); return 0; }       /* খালি তালিকা = .hidden মুছে ফেলা */
    if (rename(np, p) != 0) { r = -errno; unlink(np); }
    return r;
}

/* ================= encryption ================= */
#ifdef ROZOS_HAVE_BEARSSL

static void pbkdf2(const char *pw, const uint8_t *salt, uint32_t iter, uint8_t out[32])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    uint8_t u[32], one[4] = { 0, 0, 0, 1 };
    uint32_t i;
    int j;
    br_hmac_key_init(&kc, &br_sha256_vtable, pw, strlen(pw));
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, salt, 16);
    br_hmac_update(&hc, one, 4);
    br_hmac_out(&hc, u);
    memcpy(out, u, 32);
    for (i = 1; i < iter; i++) {
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, u, 32);
        br_hmac_out(&hc, u);
        for (j = 0; j < 32; j++) out[j] ^= u[j];
    }
    memset(u, 0, sizeof(u));
}

struct rl {
    int fd;
    uint8_t key[32];
    uint8_t hdr[RL_HDR];
    uint64_t idx;
    uint8_t *buf;
    size_t n, pos, chunk;
    int last;                   /* পাঠক: শেষ টুকরো পড়া হয়ে গেছে */
    uint64_t done, total;       /* অগ্রগতি */
    rozlock_progress_fn cb;
    void *ctx;
    const char *cur;
};

static void crypt_chunk(struct rl *s, uint8_t *data, size_t len, int last, uint8_t tag[16], int enc)
{
    uint8_t iv[12], aad[RL_HDR + 13];
    memset(iv, 0, 4);
    put64(iv + 4, s->idx);
    memcpy(aad, s->hdr, RL_HDR);
    put64(aad + RL_HDR, s->idx);
    put32(aad + RL_HDR + 8, (uint32_t)len);
    aad[RL_HDR + 12] = (uint8_t)last;
    br_poly1305_ctmul_run(s->key, iv, data, len, aad, sizeof(aad), tag, br_chacha20_ct_run, enc);
}

/* ---------- লেখক ---------- */
static int w_flush(struct rl *s, int last)
{
    uint8_t h[5], tag[16];
    int r;
    crypt_chunk(s, s->buf, s->n, last, tag, 1);
    put32(h, (uint32_t)s->n);
    h[4] = (uint8_t)last;
    if ((r = write_all(s->fd, h, 5)) < 0 || (r = write_all(s->fd, s->buf, s->n)) < 0 ||
        (r = write_all(s->fd, tag, 16)) < 0)
        return r;
    s->idx++;
    s->n = 0;
    return 0;
}

static int w_put(struct rl *s, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        size_t k = s->chunk - s->n;
        int r;
        if (k > n) k = n;
        memcpy(s->buf + s->n, b, k);
        s->n += k;
        b += k;
        n -= k;
        if (s->n == s->chunk && (r = w_flush(s, 0)) < 0) return r;
    }
    return 0;
}

static int w_rec(struct rl *s, char type, const char *path)
{
    uint8_t h[3];
    size_t n = strlen(path);
    int r;
    if (n > 65535) return -ENAMETOOLONG;
    h[0] = (uint8_t)type;
    h[1] = (uint8_t)n;
    h[2] = (uint8_t)(n >> 8);
    if ((r = w_put(s, h, 3)) < 0) return r;
    return w_put(s, path, n);
}

/* ---------- পাঠক ---------- */
static int r_fill(struct rl *s)
{
    uint8_t h[5], tag[16], want[16];
    uint32_t len;
    int r;
    if (s->last) return ROZLOCK_EFORMAT;          /* শেষের পরে আরও চাওয়া */
    if ((r = read_all(s->fd, h, 5)) < 0) return r;
    len = get32(h);
    if (len > s->chunk || h[4] > 1) return s->idx == 0 ? ROZLOCK_EFORMAT : ROZLOCK_EFORMAT;
    if ((r = read_all(s->fd, s->buf, len)) < 0 || (r = read_all(s->fd, tag, 16)) < 0) return r;
    crypt_chunk(s, s->buf, len, h[4], want, 0);
    {
        int i, d = 0;
        for (i = 0; i < 16; i++) d |= want[i] ^ tag[i];
        if (d) {
            memset(s->buf, 0, len);
            return s->idx == 0 ? ROZLOCK_EWRONG : ROZLOCK_EFORMAT;
        }
    }
    s->idx++;
    s->n = len;
    s->pos = 0;
    s->last = h[4];
    s->done += 5 + len + 16;
    if (s->cb) s->cb(s->ctx, s->done, s->total, s->cur);
    return 0;
}

static int r_get(struct rl *s, void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n) {
        size_t k;
        int r;
        if (s->pos == s->n && (r = r_fill(s)) < 0) return r;
        k = s->n - s->pos;
        if (k > n) k = n;
        memcpy(b, s->buf + s->pos, k);
        s->pos += k;
        b += k;
        n -= k;
    }
    return 0;
}

static void rl_free(struct rl *s)
{
    if (s->buf) { memset(s->buf, 0, s->chunk); free(s->buf); }
    memset(s->key, 0, sizeof(s->key));
    s->buf = NULL;
}

/* ================= লক ================= */

/* প্রথম পাস: মোট মাপ */
static int count_tree(const char *p, uint64_t *bytes, int depth)
{
    DIR *d;
    struct dirent *e;
    int r = 0;
    if (depth > 64) return -ELOOP;
    if (!(d = opendir(p))) return -errno;
    while (r == 0 && (e = readdir(d))) {
        char c[RL_PATH];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        join(c, sizeof(c), p, e->d_name);
        if (stat(c, &st) != 0) { r = -errno; break; }
        if (S_ISDIR(st.st_mode)) r = count_tree(c, bytes, depth + 1);
        else *bytes += (uint64_t)st.st_size + 64;
    }
    closedir(d);
    return r;
}

static int pack_tree(struct rl *s, const char *abs, const char *rel, int depth)
{
    DIR *d;
    struct dirent *e;
    int r = 0;
    if (depth > 64) return -ELOOP;
    if (!(d = opendir(abs))) return -errno;
    while (r == 0 && (e = readdir(d))) {
        char a[RL_PATH], rp[RL_PATH];
        uint8_t m[21];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        join(a, sizeof(a), abs, e->d_name);
        join(rp, sizeof(rp), rel, e->d_name);
        if (stat(a, &st) != 0) { r = -errno; break; }
        put32(m, (uint32_t)st.st_mode);
        put64(m + 4, (uint64_t)st.st_mtime);
        if (S_ISDIR(st.st_mode)) {
            if ((r = w_rec(s, 'D', rp)) == 0 && (r = w_put(s, m, 12)) == 0)
                r = pack_tree(s, a, rp, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            int fd = open(a, O_RDONLY);
            uint8_t buf[8192];
            ssize_t n;
            uint64_t left = (uint64_t)st.st_size;
            if (fd < 0) { r = -errno; break; }
            m[12] = (uint8_t)rozlock_is_protected(a);
            put64(m + 13, left);
            s->cur = e->d_name;
            if ((r = w_rec(s, 'F', rp)) == 0) r = w_put(s, m, 21);
            while (r == 0 && left && (n = read(fd, buf, left < sizeof(buf) ? (size_t)left : sizeof(buf))) > 0) {
                r = w_put(s, buf, (size_t)n);
                left -= (uint64_t)n;
                s->done += (uint64_t)n;
                if (s->cb) s->cb(s->ctx, s->done, s->total, s->cur);
            }
            if (r == 0 && left) r = -EIO;            /* পড়ার সময় ফাইল ছোট হয়ে গেছে */
            memset(buf, 0, sizeof(buf));
            close(fd);
            s->done += 64;
        }
        /* অন্য ধরন (ডিভাইস, pipe) বাদ */
    }
    closedir(d);
    return r;
}

/* মূল ফাইল শূন্য দিয়ে ঢেকে মোছা (নিচ থেকে ওপরে) */
static int wipe_tree(const char *p, int depth)
{
    struct stat st;
    int r = 0;
    if (depth > 64) return -ELOOP;
    if (stat(p, &st) != 0) return -errno;
    if (S_ISDIR(st.st_mode)) {
        char names[32][256];
        int n, i, more = 1;
        while (more) {
            DIR *d = opendir(p);
            struct dirent *e;
            n = 0;
            if (!d) return -errno;
            while ((e = readdir(d)) && n < 32) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                snprintf(names[n++], 256, "%s", e->d_name);
            }
            closedir(d);
            more = n > 0;
            for (i = 0; i < n; i++) {
                char c[RL_PATH];
                join(c, sizeof(c), p, names[i]);
                if (wipe_tree(c, depth + 1) < 0) { r = -EIO; more = 0; }
            }
        }
        if (r < 0) return r;
        return rmdir(p) == 0 ? 0 : -errno;
    }
    if (rozlock_is_protected(p)) rozlock_protect(p, 0);
    if (S_ISREG(st.st_mode) && st.st_size > 0) {
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            static uint8_t zero[8192];
            uint64_t left = (uint64_t)st.st_size;
            while (left) {
                size_t k = left < sizeof(zero) ? (size_t)left : sizeof(zero);
                if (write_all(fd, zero, k) < 0) break;
                left -= k;
            }
            fsync(fd);
            close(fd);
        }
    }
    return unlink(p) == 0 ? 0 : -errno;
}

static int is_system_dir(const char *abs)
{
    const char *home = getenv("HOME");
    int slashes = 0;
    const char *c;
    for (c = abs; *c; c++) slashes += *c == '/';
    if (!strcmp(abs, "/") || slashes < 2) return 1;          /* "/", "/bin", "/home" ... */
    if (home && !strcmp(abs, home)) return 1;
    if (!strcmp(abs, "/home/Desktop")) return 1;
    if (!strncmp(abs, "/dev/", 5) || !strncmp(abs, "/boot/", 6)) return 1;
    {   /* mount করা ডিস্ক নিজে বা ভেতরে mount আছে এমন ফোল্ডার নয় */
        static char buf[4096];
        long n = rozos_mounts(buf, sizeof(buf) - 1);
        char *line, *save = NULL;
        size_t al = strlen(abs);
        if (n > 0) {
            buf[n] = '\0';
            for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                char dev[64], mp[512], fs[32];
                if (sscanf(line, "%63s %511s %31s", dev, mp, fs) != 3) continue;
                if (!strncmp(mp, abs, al) && (mp[al] == '\0' || mp[al] == '/')) return 1;
            }
        }
    }
    return 0;
}

static void make_abs(const char *in, char *out, size_t n)
{
    char cwd[RL_PATH];
    if (in[0] == '/') snprintf(out, n, "%s", in);
    else if (getcwd(cwd, sizeof(cwd))) join(out, n, cwd, in);
    else snprintf(out, n, "%s", in);
    while (strlen(out) > 1 && out[strlen(out) - 1] == '/') out[strlen(out) - 1] = '\0';
}

/* হেডার বানানো/পড়া আর key তৈরি */
static int rl_begin(struct rl *s, const char *secret, const uint8_t *hdr_in)
{
    rozlock_progress_fn cb = s->cb;
    void *ctx = s->ctx;
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->ctx = ctx;
    if (hdr_in) {
        if (memcmp(hdr_in, RL_MAGIC, 8) || get32(hdr_in + 8) != 1) return ROZLOCK_EFORMAT;
        memcpy(s->hdr, hdr_in, RL_HDR);
        s->chunk = get32(hdr_in + 16);
        if (s->chunk < 1024 || s->chunk > 1024 * 1024 || get32(hdr_in + 12) < 1000 || get32(hdr_in + 12) > 10000000)
            return ROZLOCK_EFORMAT;
    } else {
        memcpy(s->hdr, RL_MAGIC, 8);
        put32(s->hdr + 8, 1);
        put32(s->hdr + 12, RL_ITER);
        put32(s->hdr + 16, RL_CHUNK);
        if (getrandom(s->hdr + 24, 16, 0) != 16) return -EIO;
        s->chunk = RL_CHUNK;
    }
    s->buf = (uint8_t *)malloc(s->chunk);
    if (!s->buf) return -ENOMEM;
    pbkdf2(secret, s->hdr + 24, get32(s->hdr + 12), s->key);
    return 0;
}

int rozlock_lock(const char *dir, const char *secret, rozlock_progress_fn cb, void *ctx,
                 char *out, size_t outlen)
{
    char abs[RL_PATH], parent[RL_PATH], base[256], final[RL_PATH + 16], tmp[RL_PATH + 32];
    struct rl s;
    struct stat st;
    struct statvfs vf;
    uint64_t bytes = 0;
    int r, fd, was_hidden;

    if (!secret || !secret[0]) return -EINVAL;
    make_abs(dir, abs, sizeof(abs));
    if (stat(abs, &st) != 0) return -errno;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    if (is_system_dir(abs)) return ROZLOCK_ESYSTEM;
    split(abs, parent, sizeof(parent), base, sizeof(base));
    snprintf(final, sizeof(final), "%s/%s" ROZLOCK_EXT, strcmp(parent, "/") ? parent : "", base);
    snprintf(tmp, sizeof(tmp), "%s/.%s" ROZLOCK_EXT ".part", strcmp(parent, "/") ? parent : "", base);
    if (access(final, F_OK) == 0) return -EEXIST;
    if ((r = count_tree(abs, &bytes, 0)) < 0) return r;
    if (statvfs(parent, &vf) == 0 && (uint64_t)vf.f_bavail * vf.f_bsize < bytes + bytes / 50 + 65536)
        return -ENOSPC;

    memset(&s, 0, sizeof(s));
    s.cb = cb;
    s.ctx = ctx;
    if ((r = rl_begin(&s, secret, NULL)) < 0) { rl_free(&s); return r; }
    s.total = bytes;
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { r = -errno; rl_free(&s); return r; }
    s.fd = fd;
    r = write_all(fd, s.hdr, RL_HDR);
    if (r == 0) r = w_rec(&s, 'R', base);
    if (r == 0) r = pack_tree(&s, abs, "", 0);
    if (r == 0) r = w_put(&s, "E", 1);
    if (r == 0) r = w_flush(&s, 1);
    if (r == 0 && fsync(fd) != 0) r = -errno;
    close(fd);
    rl_free(&s);
    if (r < 0) { unlink(tmp); return r; }

    /* লেখা ঠিক হয়েছে কিনা — পুরো ফাইল আবার খুলে দেখা (মূল ফাইল মোছার আগে) */
    {   /* সব টুকরোর tag মিলিয়ে দেখা */
        struct rl v;
        uint8_t hdr[RL_HDR];
        memset(&v, 0, sizeof(v));
        fd = open(tmp, O_RDONLY);
        if (fd < 0 || read_all(fd, hdr, RL_HDR) < 0 || (r = rl_begin(&v, secret, hdr)) < 0) {
            if (r == 0) r = -EIO;
        } else {
            v.fd = fd;
            while (r == 0 && !v.last) r = r_fill(&v);
        }
        if (fd >= 0) close(fd);
        rl_free(&v);
    }
    if (r < 0) { unlink(tmp); return r; }
    if (rename(tmp, final) != 0) { r = -errno; unlink(tmp); return r; }
    rozlock_protect(final, 1);                    /* FAT-এ হয় না — তবু লক থাকে */
    was_hidden = rozhide_is_hidden(abs);
    if (out) snprintf(out, outlen, "%s", final);
    r = wipe_tree(abs, 0) < 0 ? ROZLOCK_EPARTIAL : 0;
    if (was_hidden) { rozhide_set(abs, 0); rozhide_set(final, 1); }
    return r;
}

/* ================= আনলক ================= */

static int safe_rel(const char *p)
{
    const char *c = p;
    if (!p[0] || p[0] == '/') return 0;
    while (*c) {
        const char *e = strchr(c, '/');
        size_t n = e ? (size_t)(e - c) : strlen(c);
        if (n == 0 || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) return 0;
        if (!e) break;
        c = e + 1;
    }
    return 1;
}

static int rd_name(struct rl *s, char *out, size_t n)
{
    uint8_t h[2];
    size_t len;
    int r;
    if ((r = r_get(s, h, 2)) < 0) return r;
    len = (size_t)h[0] | ((size_t)h[1] << 8);
    if (len >= n) return ROZLOCK_EFORMAT;
    if ((r = r_get(s, out, len)) < 0) return r;
    out[len] = '\0';
    return 0;
}

/* dst == NULL হলে শুধু পড়ে যাচাই */
static int extract(struct rl *s, const char *dst)
{
    char rel[RL_PATH], a[RL_PATH * 2];
    uint8_t t, m[21], buf[8192];
    int r;
    for (;;) {
        if ((r = r_get(s, &t, 1)) < 0) return r;
        if (t == 'E') return 0;
        if (t != 'D' && t != 'F') return ROZLOCK_EFORMAT;
        if ((r = rd_name(s, rel, sizeof(rel))) < 0) return r;
        if (!safe_rel(rel)) return ROZLOCK_EFORMAT;
        if (dst) snprintf(a, sizeof(a), "%s/%s", dst, rel);
        if (t == 'D') {
            if ((r = r_get(s, m, 12)) < 0) return r;
            if (dst && mkdir(a, (mode_t)(get32(m) & 0777)) != 0 && errno != EEXIST) return -errno;
        } else {
            uint64_t left;
            int fd = -1;
            if ((r = r_get(s, m, 21)) < 0) return r;
            left = get64(m + 13);
            if (dst && (fd = open(a, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)(get32(m) & 0777))) < 0) return -errno;
            { const char *b = strrchr(rel, '/'); s->cur = b ? b + 1 : rel; }
            while (left) {
                size_t k = left < sizeof(buf) ? (size_t)left : sizeof(buf);
                if ((r = r_get(s, buf, k)) < 0 || (fd >= 0 && (r = write_all(fd, buf, k)) < 0)) break;
                left -= k;
            }
            memset(buf, 0, sizeof(buf));
            if (fd >= 0) close(fd);
            if (r < 0) return r;
            if (dst && m[12]) rozlock_protect(a, 1);    /* ভেতরের লক-ফাইল আবার মোছা-রোধী */
        }
    }
}

static int open_lock(const char *path, char *real, size_t rn)
{
    char abs[RL_PATH];
    int fd;
    make_abs(path, abs, sizeof(abs));
    if (!has_suffix(abs, ROZLOCK_EXT) && access(abs, F_OK) != 0)
        snprintf(real, rn, "%s" ROZLOCK_EXT, abs);
    else
        snprintf(real, rn, "%s", abs);
    fd = open(real, O_RDONLY);
    return fd < 0 ? -errno : fd;
}

int rozlock_check(const char *lockfile, const char *secret)
{
    char real[RL_PATH + 16];
    uint8_t hdr[RL_HDR];
    struct rl s;
    int fd, r;
    memset(&s, 0, sizeof(s));
    if ((fd = open_lock(lockfile, real, sizeof(real))) < 0) return fd;
    if ((r = read_all(fd, hdr, RL_HDR)) == 0 && (r = rl_begin(&s, secret ? secret : "", hdr)) == 0) {
        s.fd = fd;
        r = r_fill(&s);
    }
    close(fd);
    rl_free(&s);
    return r;
}

int rozlock_unlock(const char *lockfile, const char *secret, rozlock_progress_fn cb, void *ctx,
                   char *out, size_t outlen)
{
    char real[RL_PATH + 16], parent[RL_PATH], base[256], name[256], target[RL_PATH + 300], tmp[RL_PATH + 300];
    uint8_t hdr[RL_HDR], t;
    struct rl s;
    struct stat st;
    int fd, r, n, was_hidden;

    if (!secret) return -EINVAL;
    if ((fd = open_lock(lockfile, real, sizeof(real))) < 0) return fd;
    memset(&s, 0, sizeof(s));
    s.cb = cb;
    s.ctx = ctx;
    fstat(fd, &st);
    if ((r = read_all(fd, hdr, RL_HDR)) < 0 || (r = rl_begin(&s, secret, hdr)) < 0) {
        close(fd);
        rl_free(&s);
        return r;
    }
    s.fd = fd;
    s.total = (uint64_t)st.st_size;
    s.done = RL_HDR;
    /* প্রথম রেকর্ড = মূল নাম (ভুল secret এখানেই ধরা পড়ে) */
    if ((r = r_get(&s, &t, 1)) < 0 || (t != 'R' ? (r = ROZLOCK_EFORMAT) : (r = rd_name(&s, name, sizeof(name)))) < 0 ||
        !name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        if (r == 0) r = ROZLOCK_EFORMAT;
        close(fd);
        rl_free(&s);
        return r;
    }
    split(real, parent, sizeof(parent), base, sizeof(base));
    for (n = 1; n < 1000; n++) {
        if (n == 1) snprintf(target, sizeof(target), "%s/%s", strcmp(parent, "/") ? parent : "", name);
        else snprintf(target, sizeof(target), "%s/%s %d", strcmp(parent, "/") ? parent : "", name, n);
        if (access(target, F_OK) != 0) break;
    }
    snprintf(tmp, sizeof(tmp), "%s/.%s.unlock.part", strcmp(parent, "/") ? parent : "", name);
    if (access(tmp, F_OK) == 0) wipe_tree(tmp, 0);        /* আগের ভাঙা চেষ্টা */
    if (mkdir(tmp, 0755) != 0) { r = -errno; close(fd); rl_free(&s); return r; }
    r = extract(&s, tmp);
    close(fd);
    rl_free(&s);
    if (r < 0) { wipe_tree(tmp, 0); return r; }
    if (rename(tmp, target) != 0) { r = -errno; wipe_tree(tmp, 0); return r; }
    was_hidden = rozhide_is_hidden(real);
    rozlock_protect(real, 0);
    if (unlink(real) != 0) r = -errno;
    if (was_hidden) { rozhide_set(real, 0); rozhide_set(target, 1); }
    if (out) snprintf(out, outlen, "%s", target);
    return r;
}

#else  /* BearSSL নেই */

int rozlock_lock(const char *dir, const char *secret, rozlock_progress_fn cb, void *ctx, char *out, size_t outlen)
{ (void)dir; (void)secret; (void)cb; (void)ctx; (void)out; (void)outlen; return ROZLOCK_ENOCRYPTO; }
int rozlock_unlock(const char *f, const char *secret, rozlock_progress_fn cb, void *ctx, char *out, size_t outlen)
{ (void)f; (void)secret; (void)cb; (void)ctx; (void)out; (void)outlen; return ROZLOCK_ENOCRYPTO; }
int rozlock_check(const char *f, const char *secret) { (void)f; (void)secret; return ROZLOCK_ENOCRYPTO; }

#endif
