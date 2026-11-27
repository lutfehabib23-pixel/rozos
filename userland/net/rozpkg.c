/* rozOS — rozpkg: ছোট প্যাকেজ ম্যানেজার (ধাপ ১০)
 *
 * প্যাকেজ আসে একটা "repo" থেকে — সাধারণত একটা GitHub রিপোর Releases:
 *   https://github.com/OWNER/REPO/releases/latest/download/
 * সেখানে থাকে index.txt আর প্রতিটি প্যাকেজের .tar (USTAR, অসংকুচিত)।
 *
 * index.txt-এর প্রতি লাইন (ফাঁকা দিয়ে আলাদা; # = মন্তব্য):
 *   name version sha256 size filename description...
 *
 * ফাইল:
 *   /etc/rozpkg.conf                       repo <URL>
 *   /var/cache/rozpkg/index.txt            শেষ `rozpkg update`-এর index
 *   /var/cache/rozpkg/<filename>           ডাউনলোড করা প্যাকেজ (sha256 মিললে আবার ব্যবহার)
 *   /var/lib/rozpkg/installed/<name>.list  প্যাকেজের বসানো ফাইলের তালিকা
 *   /var/lib/rozpkg/installed/<name>.ver   বসানো সংস্করণ
 *
 * প্যাকেজের .tar-এর পথগুলো "/" থেকে আপেক্ষিক (usr/lib/libcjson.a ...)।
 * ধাপ ১৭খ: প্যাকেজ .tar.gz-ও হতে পারে (zlib-এর gzread সংকুচিত আর সাধারণ দুটোই পড়ে),
 * আর খোলার সময় কত MiB হলো তা দেখায়। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include "rozhttp.h"

/* ধাপ ১৭খ: tar পড়ার উৎস — zlib থাকলে gz* (.tar আর .tar.gz দুটোই), না থাকলে সাধারণ fd */
#ifdef ROZPKG_ZLIB
#include <zlib.h>
typedef gzFile tarin_t;
#define TIN_OPEN(p)       gzopen((p), "rb")
#define TIN_OK(t)         ((t) != NULL)
#define TIN_READ(t, b, n) ((long)gzread((t), (b), (unsigned)(n)))
#define TIN_SKIP(t, n)    gzseek((t), (z_off_t)(n), SEEK_CUR)
#define TIN_CLOSE(t)      gzclose(t)
#else
typedef int tarin_t;
#define TIN_OPEN(p)       open((p), O_RDONLY)
#define TIN_OK(t)         ((t) >= 0)
#define TIN_READ(t, b, n) ((long)read((t), (b), (size_t)(n)))
#define TIN_SKIP(t, n)    lseek((t), (off_t)(n), SEEK_CUR)
#define TIN_CLOSE(t)      close(t)
#endif

#define CONF_FILE   "/etc/rozpkg.conf"
#define CACHE_DIR   "/var/cache/rozpkg"
#define INDEX_FILE  CACHE_DIR "/index.txt"
#define DB_DIR      "/var/lib/rozpkg/installed"
#define DEFAULT_REPO "https://github.com/OWNER/rozos-packages/releases/latest/download"

struct pkg {
    char name[64];
    char version[32];
    char sha256[65];
    long size;
    char file[128];
    char desc[256];
};

static struct pkg *idx;
static int n_idx;
static char repo[1024];
static const char *root = "/";        /* -r DIR: অন্য কোথাও বসানো (পরীক্ষার জন্য) */

/* ---------- ছোট সহায়ক ---------- */

static void mkdirs(const char *path)
{
    char tmp[1024], *p;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void join_root(char *out, size_t size, const char *rel)
{
    if (!strcmp(root, "/")) snprintf(out, size, "/%s", rel);
    else snprintf(out, size, "%s/%s", root, rel);
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    return s;
}

static int valid_name(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.' || *s == '+'))
            return 0;
    }
    return 1;
}

/* ---------- repo কনফিগ ---------- */

static void load_conf(void)
{
    FILE *f = fopen(CONF_FILE, "r");
    char line[1200];
    snprintf(repo, sizeof(repo), "%s", DEFAULT_REPO);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!strncmp(s, "repo", 4) && (s[4] == ' ' || s[4] == '\t')) {
            snprintf(repo, sizeof(repo), "%s", trim(s + 4));
        }
    }
    fclose(f);
    while (strlen(repo) > 1 && repo[strlen(repo) - 1] == '/') repo[strlen(repo) - 1] = '\0';
}

/* ধাপ ১৯ছ: repo একটা ফোল্ডার হলে ("/mnt/usb/packages") ইন্টারনেট ছাড়াই সেখান থেকে —
 * rozhttp_get-এর মতোই out_fd-তে লেখে আর on_data ডাকে */
static int repo_get(const char *url, int out_fd, const struct rozhttp_opts *o, struct rozhttp_result *r)
{
    char buf[8192];
    ssize_t n;
    int fd;
    if (url[0] != '/') return rozhttp_get(url, out_fd, o, r);
    memset(r, 0, sizeof(*r));
    fd = open(url, O_RDONLY);
    if (fd < 0) {
        r->status = 404;
        snprintf(r->error, sizeof(r->error), "%s: %s", url, strerror(errno));
        return -1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (out_fd >= 0 && write(out_fd, buf, (size_t)n) != n) { n = -1; break; }
        if (o->on_data) o->on_data(o->on_data_ctx, buf, (size_t)n);
        r->bytes += n;
    }
    close(fd);
    if (n < 0) { snprintf(r->error, sizeof(r->error), "read error: %s", strerror(errno)); return -1; }
    r->status = 200;
    return 0;
}

/* "OWNER/REPO" বা "OWNER/REPO@TAG" → GitHub Releases URL; পূর্ণ URL হলে যেমন আছে;
 * ধাপ ১৯ছ: "/" দিয়ে শুরু = rozOS-এর একটা ফোল্ডার (পেন ড্রাইভ ইত্যাদি) */
static int cmd_repo(int argc, char **argv)
{
    char url[1024];
    FILE *f;
    if (argc < 1) {
        printf("%s\n", repo);
        return 0;
    }
    if (!strncmp(argv[0], "http://", 7) || !strncmp(argv[0], "https://", 8) || argv[0][0] == '/') {
        snprintf(url, sizeof(url), "%s", argv[0]);
    } else {
        char tmp[512], *at;
        snprintf(tmp, sizeof(tmp), "%s", argv[0]);
        at = strchr(tmp, '@');
        if (!strchr(tmp, '/')) {
            fprintf(stderr, "rozpkg: repo must be a URL or OWNER/REPO[@TAG]\n");
            return 2;
        }
        if (at) {
            *at = '\0';
            snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s", tmp, at + 1);
        } else {
            snprintf(url, sizeof(url), "https://github.com/%s/releases/latest/download", tmp);
        }
    }
    while (strlen(url) > 1 && url[strlen(url) - 1] == '/') url[strlen(url) - 1] = '\0';
    f = fopen(CONF_FILE, "w");
    if (!f) {
        fprintf(stderr, "rozpkg: %s: %s\n", CONF_FILE, strerror(errno));
        return 1;
    }
    fprintf(f, "# rozpkg — প্যাকেজের উৎস (rozpkg repo দিয়ে বদলানো হয়েছে)\nrepo %s\n", url);
    fclose(f);
    printf("repo set to %s\nnow run: rozpkg update\n", url);
    return 0;
}

/* ---------- index ---------- */

static int parse_index_line(char *line, struct pkg *p)
{
    char *save = NULL, *t[6];
    int i;
    line = trim(line);
    if (!*line || *line == '#') return 0;
    for (i = 0; i < 5; i++) {
        t[i] = strtok_r(i ? NULL : line, " \t", &save);
        if (!t[i]) return 0;
    }
    t[5] = save ? trim(save) : (char *)"";
    if (!valid_name(t[0]) || strlen(t[2]) != 64 || strchr(t[4], '/')) return 0;
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", t[0]);
    snprintf(p->version, sizeof(p->version), "%s", t[1]);
    snprintf(p->sha256, sizeof(p->sha256), "%s", t[2]);
    p->size = atol(t[3]);
    snprintf(p->file, sizeof(p->file), "%s", t[4]);
    snprintf(p->desc, sizeof(p->desc), "%s", t[5]);
    return 1;
}

static int load_index(int quiet)
{
    FILE *f = fopen(INDEX_FILE, "r");
    char line[1024];
    int cap = 0;
    if (!f) {
        if (!quiet) fprintf(stderr, "rozpkg: no package index yet — run `rozpkg update` first\n");
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        struct pkg p;
        if (!parse_index_line(line, &p)) continue;
        if (n_idx == cap) {
            cap = cap ? cap * 2 : 32;
            idx = (struct pkg *)realloc(idx, (size_t)cap * sizeof(*idx));
            if (!idx) { fclose(f); return -1; }
        }
        idx[n_idx++] = p;
    }
    fclose(f);
    return 0;
}

static struct pkg *find_pkg(const char *name)
{
    int i;
    for (i = 0; i < n_idx; i++) {
        if (!strcmp(idx[i].name, name)) return &idx[i];
    }
    return NULL;
}

struct membuf {
    char *p;
    size_t len, cap;
};

static void mem_append(void *ctx, const void *data, size_t len)
{
    struct membuf *m = (struct membuf *)ctx;
    if (m->len + len + 1 > m->cap) {
        size_t nc = (m->len + len + 1) * 2;
        char *np = (char *)realloc(m->p, nc);
        if (!np) return;
        m->p = np;
        m->cap = nc;
    }
    memcpy(m->p + m->len, data, len);
    m->len += len;
    m->p[m->len] = '\0';
}

static int cmd_update(void)
{
    char url[1200];
    struct rozhttp_opts o;
    struct rozhttp_result r;
    struct membuf m = { NULL, 0, 0 };
    int fd, count = 0;
    char *line, *save = NULL;

    if (strstr(repo, "github.com/OWNER/")) {
        fprintf(stderr, "rozpkg: no repo configured yet — set yours first, e.g.\n"
                        "    rozpkg repo YOUR-GITHUB-NAME/rozos-packages\n"
                        "(see `help rozpkg` for how to publish packages)\n");
        return 1;
    }
    snprintf(url, sizeof(url), "%s/index.txt", repo);
    printf("fetching %s\n", url);
    memset(&o, 0, sizeof(o));
    o.header_fd = -1;
    o.on_data = mem_append;
    o.on_data_ctx = &m;
    if (repo_get(url, -1, &o, &r) < 0) {
        fprintf(stderr, "rozpkg: update failed: %s\n", r.error);
        if (r.status == 404) fprintf(stderr, "rozpkg: is the repo right? (rozpkg repo — see `help rozpkg`)\n");
        free(m.p);
        return 1;
    }
    if (!m.p) {
        fprintf(stderr, "rozpkg: the index is empty\n");
        return 1;
    }
    mkdirs(CACHE_DIR);
    fd = open(INDEX_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, m.p, m.len) != (ssize_t)m.len) {
        fprintf(stderr, "rozpkg: cannot write %s: %s\n", INDEX_FILE, strerror(errno));
        if (fd >= 0) close(fd);
        free(m.p);
        return 1;
    }
    close(fd);
    for (line = strtok_r(m.p, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        struct pkg p;
        count += parse_index_line(line, &p);
    }
    free(m.p);
    printf("%d package%s available%s\n", count, count == 1 ? "" : "s", r.tls ? " (verified over TLS)" : "");
    return 0;
}

/* ---------- ইনস্টল-ডাটাবেস ---------- */

static int installed_version(const char *name, char *ver, size_t size)
{
    char path[256];
    FILE *f;
    snprintf(path, sizeof(path), DB_DIR "/%s.ver", name);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(ver, (int)size, f)) ver[0] = '\0';
    fclose(f);
    snprintf(ver, size, "%s", trim(ver));
    return 1;
}

static int cmd_list(int only_installed)
{
    int i, shown = 0;
    if (only_installed) {
        DIR *d = opendir(DB_DIR);
        struct dirent *e;
        if (!d) { printf("no packages installed\n"); return 0; }
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            char name[128], ver[64];
            if (l < 5 || strcmp(e->d_name + l - 4, ".ver")) continue;
            snprintf(name, sizeof(name), "%.*s", (int)(l - 4), e->d_name);
            installed_version(name, ver, sizeof(ver));
            printf("  %-16s %s\n", name, ver);
            shown++;
        }
        closedir(d);
        if (!shown) printf("no packages installed\n");
        return 0;
    }
    if (load_index(0) < 0) return 1;
    for (i = 0; i < n_idx; i++) {
        char ver[64];
        int inst = installed_version(idx[i].name, ver, sizeof(ver));
        printf("  %-16s %-10s %s %s\n", idx[i].name, idx[i].version,
               !inst ? "   " : !strcmp(ver, idx[i].version) ? "[i]" : "[u]", idx[i].desc);
    }
    printf("%d packages   [i] installed   [u] update available\n", n_idx);
    return 0;
}

static int cmd_search(const char *word)
{
    int i, found = 0;
    if (load_index(0) < 0) return 1;
    for (i = 0; i < n_idx; i++) {
        if (strstr(idx[i].name, word) || strstr(idx[i].desc, word)) {
            printf("  %-16s %-10s %s\n", idx[i].name, idx[i].version, idx[i].desc);
            found++;
        }
    }
    if (!found) printf("nothing matches '%s'\n", word);
    return found ? 0 : 1;
}

static int cmd_info(const char *name)
{
    char ver[64], path[256], line[1024];
    struct pkg *p;
    FILE *f;
    load_index(1);
    p = find_pkg(name);
    if (!p && !installed_version(name, ver, sizeof(ver))) {
        fprintf(stderr, "rozpkg: no package '%s'\n", name);
        return 1;
    }
    printf("name:        %s\n", name);
    if (p) {
        printf("version:     %s\n", p->version);
        printf("description: %s\n", p->desc);
        printf("file:        %s (%ld bytes)\n", p->file, p->size);
        printf("sha256:      %s\n", p->sha256);
    }
    if (installed_version(name, ver, sizeof(ver))) {
        printf("installed:   %s\nfiles:\n", ver);
        snprintf(path, sizeof(path), DB_DIR "/%s.list", name);
        if ((f = fopen(path, "r"))) {
            while (fgets(line, sizeof(line), f)) printf("  /%s", line);
            fclose(f);
        }
    } else {
        printf("installed:   no\n");
    }
    return 0;
}

/* ---------- USTAR খোলা ---------- */

static long octal(const char *s, int n)
{
    long v = 0;
    while (n-- > 0 && (*s == ' ' || *s == '0' || (*s >= '1' && *s <= '7'))) {
        if (*s != ' ') v = v * 8 + (*s - '0');
        s++;
    }
    return v;
}

static int safe_path(const char *p)
{
    const char *s = p;
    if (*p == '/' || !*p) return 0;
    while (*s) {
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == '\0') && (s == p || s[-1] == '/')) return 0;
        s++;
    }
    return 1;
}

/* list_fd-এ প্রতিটি বসানো ফাইল/ডিরেক্টরির আপেক্ষিক পথ */
static int extract_tar(const char *tarfile, int list_fd, int *nfiles)
{
    tarin_t fd = TIN_OPEN(tarfile);
    int out;
    long done = 0, shown = -1;            /* ধাপ ১৭খ: খোলার অগ্রগতি (MiB) */
    unsigned char hdr[512];
    char name[256], dest[1200], *buf;
    long size, left;

    if (!TIN_OK(fd)) return -1;
    buf = (char *)malloc(16384);
    if (!buf) { TIN_CLOSE(fd); return -1; }
    *nfiles = 0;
    for (;;) {
        if (TIN_READ(fd, hdr, 512) != 512) break;
        if (!hdr[0]) break;                                   /* শেষের ফাঁকা ব্লক */
        if (memcmp(hdr + 257, "ustar", 5)) {
            fprintf(stderr, "rozpkg: %s: not a USTAR archive\n", tarfile);
            goto fail;
        }
        if (hdr[345]) snprintf(name, sizeof(name), "%.155s/%.100s", (char *)hdr + 345, (char *)hdr);
        else snprintf(name, sizeof(name), "%.100s", (char *)hdr);
        size = octal((char *)hdr + 124, 12);
        while (!strncmp(name, "./", 2)) memmove(name, name + 2, strlen(name + 2) + 1);
        if (name[0] && name[strlen(name) - 1] == '/') name[strlen(name) - 1] = '\0';
        if (!name[0]) { TIN_SKIP(fd, (size + 511) / 512 * 512); continue; }
        if (!safe_path(name)) {
            fprintf(stderr, "rozpkg: refusing unsafe path '%s'\n", name);
            goto fail;
        }
        join_root(dest, sizeof(dest), name);
        if (hdr[156] == '5') {
            mkdirs(dest);
            dprintf(list_fd, "%s/\n", name);
            continue;
        }
        if (hdr[156] != '0' && hdr[156] != '\0') {
            fprintf(stderr, "rozpkg: skipping '%s' (unsupported tar entry type '%c')\n", name, hdr[156]);
            TIN_SKIP(fd, (size + 511) / 512 * 512);
            continue;
        }
        {
            char dir[1200], *slash;
            snprintf(dir, sizeof(dir), "%s", dest);
            if ((slash = strrchr(dir, '/')) && slash != dir) { *slash = '\0'; mkdirs(dir); }
        }
        out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, (int)(octal((char *)hdr + 100, 8) & 0777) | 0600);
        if (out < 0) {
            fprintf(stderr, "rozpkg: %s: %s\n", dest, strerror(errno));
            goto fail;
        }
        left = size;
        while (left > 0) {
            long want = left > 16384 ? 16384 : left;
            long n = TIN_READ(fd, buf, want);
            if (n <= 0 || write(out, buf, (size_t)n) != n) {
                close(out);
                fprintf(stderr, "rozpkg: %s: short read/write\n", dest);
                goto fail;
            }
            left -= n;
            done += n;
            if (done >> 20 != shown) {     /* প্রতি MiB-এ এক লাইন হালনাগাদ */
                shown = done >> 20;
                printf("\r  unpacking %ld MiB", shown);
                fflush(stdout);
            }
        }
        close(out);
        if (size % 512) TIN_SKIP(fd, 512 - size % 512);
        dprintf(list_fd, "%s\n", name);
        (*nfiles)++;
    }
    if (shown >= 0) printf("\r  unpacked %ld MiB   \n", shown);
    free(buf);
    TIN_CLOSE(fd);
    return 0;
fail:
    if (shown >= 0) printf("\n");
    free(buf);
    TIN_CLOSE(fd);
    return -1;
}

/* ---------- ডাউনলোড + যাচাই ---------- */

static int file_sha256(const char *path, char hex[65], long *size)
{
    struct rozhttp_sha256 s;
    char buf[8192];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    rozhttp_sha256_init(&s);
    *size = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        rozhttp_sha256_update(&s, buf, (size_t)n);
        *size += n;
    }
    close(fd);
    rozhttp_sha256_hex(&s, hex);
    return 0;
}

static void sha_cb(void *ctx, const void *data, size_t len)
{
    rozhttp_sha256_update((struct rozhttp_sha256 *)ctx, data, len);
}

/* ধাপ ২৫ক: ফাইল কপি (বড় বাফারে — ডিস্কে কম কমান্ড) */
static int copy_file(const char *from, const char *to)
{
    char *buf = (char *)malloc(256 * 1024);
    int in, out, ok = 1;
    ssize_t n;
    if (!buf) return -1;
    in = open(from, O_RDONLY);
    out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    if (in < 0 || out < 0) ok = 0;
    while (ok && (n = read(in, buf, 256 * 1024)) > 0) {
        if (write(out, buf, (size_t)n) != n) ok = 0;
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    free(buf);
    if (!ok) unlink(to);
    return ok ? 0 : -1;
}

/* ধাপ ২৫ক: RAM-এ (/tmp) নামানোর মতো জায়গা আছে কি — প্যাকেজের ৩ গুণ + ৬৪ MiB খালি থাকতে হবে।
 * তাহলে নেটওয়ার্ক পেন ড্রাইভের ধীর লেখার জন্য অপেক্ষা করে না। */
static int fits_in_ram(long size)
{
    struct sysinfo si;
    unsigned long free_b;
    if (size <= 0 || sysinfo(&si) != 0) return 0;
    free_b = si.freeram * (si.mem_unit ? si.mem_unit : 1);
    return free_b > (unsigned long)size * 3 + 64ul * 1024 * 1024;
}

/* ফেরত: 0 = path-এ (cache) তৈরি, 1 = path-এ /tmp-এর ফাইল (install_one পরে cache-এ কপি করে), -1 = ভুল */
static int fetch_pkg(const struct pkg *p, char *path, size_t size)
{
    char url[1400], hex[65];
    struct rozhttp_sha256 s;
    struct rozhttp_opts o;
    struct rozhttp_result r;
    long have;
    int fd, in_tmp = 0;

    mkdirs(CACHE_DIR);
    snprintf(path, size, CACHE_DIR "/%s", p->file);
    if (file_sha256(path, hex, &have) == 0 && !strcmp(hex, p->sha256)) {
        printf("  using cached %s\n", p->file);
        return 0;
    }
    snprintf(url, sizeof(url), "%s/%s", repo, p->file);
    if (fits_in_ram(p->size)) {                       /* ধাপ ২৫ক */
        snprintf(path, size, "/tmp/rozpkg-%s", p->file);
        in_tmp = 1;
    }
    printf("  downloading %s (%ld KiB)%s\n", p->file, (p->size + 1023) / 1024, in_tmp ? " into RAM" : "");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "rozpkg: %s: %s\n", path, strerror(errno));
        return -1;
    }
    rozhttp_sha256_init(&s);
    memset(&o, 0, sizeof(o));
    o.header_fd = -1;
    o.progress = 1;
    o.on_data = sha_cb;
    o.on_data_ctx = &s;
    if (repo_get(url, fd, &o, &r) < 0) {
        close(fd);
        unlink(path);
        fprintf(stderr, "rozpkg: download failed: %s\n", r.error);
        return -1;
    }
    close(fd);
    rozhttp_sha256_hex(&s, hex);
    if (strcmp(hex, p->sha256) || (p->size > 0 && r.bytes != p->size)) {
        unlink(path);
        fprintf(stderr, "rozpkg: %s: checksum mismatch — refusing to install\n  expected %s\n  got      %s\n",
                p->file, p->sha256, hex);
        return -1;
    }
    printf("  sha256 ok\n");
    return in_tmp;
}

/* ---------- remove ---------- */

static int remove_pkg(const char *name, int verbose)
{
    char path[256], line[1024], dest[1200], ver[64];
    char **dirs = NULL;
    int ndirs = 0, nfiles = 0, i;
    FILE *f;

    if (!installed_version(name, ver, sizeof(ver))) {
        fprintf(stderr, "rozpkg: '%s' is not installed\n", name);
        return -1;
    }
    snprintf(path, sizeof(path), DB_DIR "/%s.list", name);
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            char *s = trim(line);
            size_t l = strlen(s);
            if (!l || !safe_path(s)) continue;
            if (s[l - 1] == '/') {
                char **nd = (char **)realloc(dirs, (size_t)(ndirs + 1) * sizeof(char *));
                if (nd) { dirs = nd; s[l - 1] = '\0'; dirs[ndirs++] = strdup(s); }
                continue;
            }
            join_root(dest, sizeof(dest), s);
            if (unlink(dest) == 0) nfiles++;
        }
        fclose(f);
    }
    /* ডিরেক্টরি — গভীরগুলো আগে; খালি না হলে (অন্য প্যাকেজের ফাইল) থেকে যায় */
    for (i = ndirs - 1; i >= 0; i--) {
        join_root(dest, sizeof(dest), dirs[i]);
        rmdir(dest);
        free(dirs[i]);
    }
    free(dirs);
    unlink(path);
    snprintf(path, sizeof(path), DB_DIR "/%s.ver", name);
    unlink(path);
    if (verbose) printf("removed %s %s (%d files)\n", name, ver, nfiles);
    return 0;
}

/* ---------- install ---------- */

static int install_one(const char *name, int force)
{
    char ver[64], tar[256], path[256];
    struct pkg *p = find_pkg(name);
    int lfd, nfiles, from_tmp;

    if (!p) {
        fprintf(stderr, "rozpkg: no package '%s' (try `rozpkg search`, or `rozpkg update`)\n", name);
        return -1;
    }
    if (installed_version(name, ver, sizeof(ver))) {
        if (!strcmp(ver, p->version) && !force) {
            printf("%s %s is already installed\n", name, ver);
            return 0;
        }
        printf("replacing %s %s with %s\n", name, ver, p->version);
    }
    printf("installing %s %s\n", p->name, p->version);
    if ((from_tmp = fetch_pkg(p, tar, sizeof(tar))) < 0) return -1;
    if (installed_version(name, ver, sizeof(ver))) remove_pkg(name, 0);
    mkdirs(DB_DIR);
    snprintf(path, sizeof(path), DB_DIR "/%s.list", name);
    lfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd < 0) {
        fprintf(stderr, "rozpkg: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (extract_tar(tar, lfd, &nfiles) < 0) {
        close(lfd);
        fprintf(stderr, "rozpkg: unpacking %s failed\n", p->file);
        if (from_tmp) unlink(tar);
        return -1;
    }
    close(lfd);
    /* ধাপ ২৫ক: RAM-এ নামানো হলে একটা কপি cache-এ (পরে reinstall-এ আবার নামাতে হয় না) */
    if (from_tmp) {
        char cached[256];
        snprintf(cached, sizeof(cached), CACHE_DIR "/%s", p->file);
        printf("  keeping a copy in " CACHE_DIR "\n");
        if (copy_file(tar, cached) < 0) fprintf(stderr, "rozpkg: could not keep a copy (%s)\n", strerror(errno));
        unlink(tar);
    }
    snprintf(path, sizeof(path), DB_DIR "/%s.ver", name);
    lfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd >= 0) {
        dprintf(lfd, "%s\n", p->version);
        close(lfd);
    }
    printf("installed %s %s (%d files) — see `rozpkg info %s`\n", p->name, p->version, nfiles, p->name);
    return 0;
}

static int cmd_install(int argc, char **argv, int force)
{
    int i, bad = 0;
    if (argc < 1) {
        fprintf(stderr, "usage: rozpkg install NAME...\n");
        return 2;
    }
    if (load_index(0) < 0) return 1;
    for (i = 0; i < argc; i++) bad |= install_one(argv[i], force) < 0;
    return bad;
}

static int cmd_upgrade(void)
{
    int i, n = 0, bad = 0;
    char ver[64];
    if (load_index(0) < 0) return 1;
    for (i = 0; i < n_idx; i++) {
        if (installed_version(idx[i].name, ver, sizeof(ver)) && strcmp(ver, idx[i].version)) {
            bad |= install_one(idx[i].name, 0) < 0;
            n++;
        }
    }
    if (!n) printf("everything is up to date\n");
    return bad;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: rozpkg [-r ROOT] COMMAND [ARGS]\n"
            "  update              download the package index from the repo\n"
            "  list                all packages ([i] installed, [u] update available)\n"
            "  search WORD         find packages by name or description\n"
            "  info NAME           details and installed files\n"
            "  install NAME...     download, verify (sha256) and install\n"
            "  reinstall NAME...   install again even if the same version\n"
            "  remove NAME...      delete a package's files\n"
            "  upgrade             install newer versions of installed packages\n"
            "  installed           list installed packages\n"
            "  repo [URL|OWNER/REPO[@TAG]|/FOLDER]   show or set the package source\n"
            "see: help rozpkg\n");
}

int main(int argc, char **argv)
{
    const char *cmd;
    int i, bad = 0;

    if (argc > 2 && !strcmp(argv[1], "-r")) {
        root = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    load_conf();
    cmd = argv[1];
    argc -= 2;
    argv += 2;
    if (!strcmp(cmd, "update")) return cmd_update();
    if (!strcmp(cmd, "list")) return cmd_list(0);
    if (!strcmp(cmd, "installed")) return cmd_list(1);
    if (!strcmp(cmd, "search") && argc == 1) return cmd_search(argv[0]);
    if (!strcmp(cmd, "info") && argc == 1) return cmd_info(argv[0]);
    if (!strcmp(cmd, "install")) return cmd_install(argc, argv, 0);
    if (!strcmp(cmd, "reinstall")) return cmd_install(argc, argv, 1);
    if (!strcmp(cmd, "upgrade")) return cmd_upgrade();
    if (!strcmp(cmd, "repo")) return cmd_repo(argc, argv);
    if (!strcmp(cmd, "remove") && argc >= 1) {
        for (i = 0; i < argc; i++) bad |= remove_pkg(argv[i], 1) < 0;
        return bad;
    }
    usage();
    return 2;
}
