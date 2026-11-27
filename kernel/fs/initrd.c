/* rozOS — USTAR initrd পাঠক (rozos/initrd.h দেখুন)
 *
 * USTAR বিন্যাস: প্রতিটি এন্ট্রি = 512 বাইটের হেডার + ডেটা (512-এর গুণিতকে প্যাড)।
 * আর্কাইভ শেষ হয় দুটো শূন্য ব্লকে। সংখ্যাগুলো ASCII অক্টালে লেখা। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/initrd.h>
#include <rozos/bootinfo.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/compiler.h>

#define TAR_BLOCK 512

struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];          /* "ustar\0" অথবা GNU "ustar " */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} ROZ_PACKED;

static uint64_t parse_octal(const char *s, size_t n)
{
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\0')) {
        i++;
    }
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
        v = (v << 3) | (uint64_t)(s[i] - '0');
    }
    return v;
}

static int checksum_ok(const struct ustar_header *h)
{
    const uint8_t *p = (const uint8_t *)h;
    uint64_t sum = 0, stored = parse_octal(h->checksum, sizeof(h->checksum));
    size_t i;
    for (i = 0; i < TAR_BLOCK; i++) {
        /* checksum ফিল্ড নিজে স্পেস ধরে হিসাব হয় */
        sum += (i >= 148 && i < 156) ? (uint8_t)' ' : p[i];
    }
    return sum == stored;
}

static int is_zero_block(const uint8_t *p)
{
    size_t i;
    for (i = 0; i < TAR_BLOCK; i++) {
        if (p[i]) {
            return 0;
        }
    }
    return 1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static const struct boot_module *find_initrd(void)
{
    const struct bootinfo *bi = bootinfo();
    uint64_t i;
    for (i = 0; i < bi->module_count; i++) {
        if (!strcmp(bi->modules[i].cmdline, "initrd") || ends_with(bi->modules[i].path, "initrd.tar")) {
            return &bi->modules[i];
        }
    }
    return NULL;
}

/* "./etc/motd" বা prefix + name → "/dest/etc/motd"; শেষের '/' বাদ */
static int build_path(const char *dest, const struct ustar_header *h, char *out, size_t size)
{
    char rel[256 + 2];
    size_t n = 0, i, dl;
    const char *name;

    if (h->prefix[0]) {
        for (i = 0; i < sizeof(h->prefix) && h->prefix[i]; i++) rel[n++] = h->prefix[i];
        rel[n++] = '/';
    }
    for (i = 0; i < sizeof(h->name) && h->name[i]; i++) rel[n++] = h->name[i];
    rel[n] = '\0';

    name = rel;
    while (name[0] == '.' && name[1] == '/') name += 2;   /* "./" বাদ */
    while (name[0] == '/') name++;
    n = strlen(name);
    while (n > 0 && name[n - 1] == '/') n--;
    if (n == 0 || !strcmp(name, ".")) {
        return 0;                                          /* আর্কাইভের root নিজে */
    }

    dl = strlen(dest);
    while (dl > 1 && dest[dl - 1] == '/') dl--;
    if (dl + 1 + n + 1 > size) {
        return -ENAMETOOLONG;
    }
    memcpy(out, dest, dl);
    i = dl;
    if (!(dl == 1 && dest[0] == '/')) {
        out[i++] = '/';
    }
    memcpy(out + i, name, n);
    out[i + n] = '\0';
    return 1;
}

static const char *base_name(const char *path)
{
    const char *s = strchr(path, '/'), *last = path;
    while (s) {
        last = s + 1;
        s = strchr(last, '/');
    }
    return last;
}

/* ধাপ ২৪খ: keep(path) সত্য আর ফাইলটা আগে থেকেই থাকলে লেখা হয় না (স্থায়ী root-এ
 * নতুন সংস্করণের ফাইল বসানোর সময় ব্যবহারকারীর ফাইল আর সেটিং রাখা) */
static int (*keep_fn)(const char *path);
static void (*progress_fn)(uint64_t done, uint64_t total);   /* ধাপ ২৪গ */

void initrd_set_progress(void (*fn)(uint64_t done, uint64_t total))
{
    progress_fn = fn;
}

int initrd_load_keep(const char *dest, struct initrd_stats *st, int (*keep)(const char *path))
{
    int r;
    keep_fn = keep;
    r = initrd_load(dest, st);
    keep_fn = NULL;
    return r;
}

int initrd_load(const char *dest, struct initrd_stats *st)
{
    const struct boot_module *m = find_initrd();
    const uint8_t *p, *end;
    char path[VFS_PATH_MAX];

    memset(st, 0, sizeof(*st));
    if (!m) {
        return -ENOENT;
    }
    strncpy(st->module_path, m->path, sizeof(st->module_path) - 1);
    st->archive_size = m->size;
    p = (const uint8_t *)(uintptr_t)m->virt;
    end = p + m->size;

    while (p + TAR_BLOCK <= end) {
        const struct ustar_header *h = (const struct ustar_header *)p;
        uint64_t size;
        int r;

        if (is_zero_block(p)) {
            break;                                         /* আর্কাইভ শেষ */
        }
        if (memcmp(h->magic, "ustar", 5) != 0 || !checksum_ok(h)) {
            st->errors++;
            break;                                         /* নষ্ট আর্কাইভ — আর এগোনো নিরাপদ নয় */
        }
        size = parse_octal(h->size, sizeof(h->size));
        if (p + TAR_BLOCK + size > end) {
            st->errors++;
            break;
        }

        if (progress_fn) progress_fn((uint64_t)(p - (const uint8_t *)(uintptr_t)m->virt), m->size);
        r = build_path(dest, h, path, sizeof(path));
        if (r < 0) {
            st->errors++;
        } else if (r > 0) {
            if (h->typeflag == '5') {
                if (vfs_mkdir_p(path) == 0) {
                    st->dirs++;
                } else {
                    st->errors++;
                }
            } else if (h->typeflag == '0' || h->typeflag == '\0') {
                struct vfs_stat ks;
                if (!strcmp(base_name(path), ".keep")) {
                    /* শুধু খালি ডিরেক্টরি রাখার চিহ্ন — ফাইলটা লাগবে না */
                } else if (keep_fn && keep_fn(path) && vfs_stat(path, &ks) == 0) {
                    /* ধাপ ২৪খ: ব্যবহারকারীর ফাইল — আগেরটাই থাকে */
                } else {
                    char parent[VFS_PATH_MAX], name[VFS_NAME_MAX + 1];
                    int fd;
                    if (vfs_split_path(path, parent, sizeof(parent), name, sizeof(name)) == 0) {
                        vfs_mkdir_p(parent);
                    }
                    fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
                    if (fd >= 0 && (size == 0 || vfs_write(fd, p + TAR_BLOCK, (size_t)size) == (int64_t)size)) {
                        st->files++;
                        st->bytes += size;
                    } else {
                        st->errors++;
                    }
                    if (fd >= 0) {
                        vfs_close(fd);
                    }
                }
            } else {
                st->skipped++;                             /* symlink, hardlink, ডিভাইস ইত্যাদি */
            }
        }
        p += TAR_BLOCK + ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
    }
    return 0;
}
