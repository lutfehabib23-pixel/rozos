/* rozOS — exFAT, পড়া আর লেখা (ধাপ ২৪গ)
 *
 * বড় পেন ড্রাইভ আর SD কার্ড সাধারণত exFAT (৩২ GB-এর বেশি হলে Windows এটাই দেয়)।
 *
 * বিন্যাস:
 *   boot sector "EXFAT   ": FAT আর cluster heap কোথায়, cluster-এর মাপ, root-এর cluster
 *   FAT: প্রতি cluster ৪ বাইট (পরের cluster; 0xFFFFFFFF = শেষ)। NoFatChain ফ্ল্যাগ থাকলে
 *        ফাইল একটানা, FAT-এ কিছু লেখা থাকে না
 *   allocation bitmap (root-এর 0x81 entry): প্রতি cluster ১ বিট — খালি জায়গার আসল হিসাব
 *   upcase table (0x82): নামের hash-এর জন্য বড় হাতের অক্ষর
 *   ডিরেক্টরি: ৩২ বাইটের entry-র "set": 0x85 File (attribute, সময়, checksum) +
 *     0xC0 Stream (প্রথম cluster, মাপ, নামের hash) + 0xC1 Name (প্রতিটায় ১৫টা UTF-16 অক্ষর)
 *     মোছা হলে type-এর বিট ৭ শূন্য; 0x00 = শেষ। "." / ".." নেই।
 * লেখার নিয়ম (সহজ রাখতে): ফাইল বা ডিরেক্টরি বড় হলে আগে FAT chain বানিয়ে NoFatChain মুছে
 * ফেলা হয় — exFAT-এ এটাও বৈধ, Windows ঠিকমতো পড়ে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/block.h>
#include <rozos/errno.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/clock.h>

#define EOC        0xFFFFFFFFu
#define ATTR_DIR   0x10
#define SF_ALLOC   0x01
#define SF_NOFAT   0x02

struct exfat_vnode;

struct exfat_fs {
    struct block_device *dev;
    uint32_t bps, csize;                    /* বাইট: sector, cluster */
    uint64_t fat_off, heap_off;
    uint32_t clusters, root;
    uint8_t *bm;                            /* allocation bitmap (মেমোরিতে পুরো) */
    uint32_t bm_bytes, *bm_cl, bm_ncl;      /* bitmap-এর cluster-গুলো */
    uint32_t free_count, hint;
    uint16_t *up;                           /* upcase (65536 ঘর) */
    char     label[48];
    uint32_t serial;
    int      read_only;
    struct exfat_vnode *vnodes;
};

struct exfat_vnode {
    struct vnode vn;                        /* প্রথম সদস্য */
    struct exfat_fs *fs;
    uint32_t first;                         /* 0 = খালি */
    int      nofat, is_root;
    uint64_t dsize;                         /* ডিরেক্টরির data length (root: chain-এর মাপ) */
    /* parent-এ entry set: pdir (root হলে NULL), File entry-র slot, secondary-র সংখ্যা */
    struct exfat_vnode *pdir;
    uint32_t slot, nsec;
    uint16_t attr;
    struct exfat_vnode *next;
};

static const struct vnode_ops exfat_ops;
static inline struct exfat_vnode *XV(struct vnode *vn) { return (struct exfat_vnode *)vn; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static inline void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* ---------- সময় (DOS ধাঁচের ৩২ বিট) ---------- */
static int64_t days_from_civil(int y, int m, int d)
{
    int era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static uint64_t ts_to_unix(uint32_t ts)
{
    int y = 1980 + (int)(ts >> 25), m = (int)((ts >> 21) & 15), d = (int)((ts >> 16) & 31);
    if (m < 1 || m > 12 || d < 1) return 0;
    return (uint64_t)(days_from_civil(y, m, d) * 86400 + ((ts >> 11) & 31) * 3600 + ((ts >> 5) & 63) * 60 +
                      (ts & 31) * 2);
}

static uint32_t now_ts(void)
{
    struct clock_tm tm;
    int64_t t = clock_unix_time();
    if (t <= 0) t = 315532800;
    clock_to_tm(t, &tm);
    if (tm.year < 1980) tm.year = 1980;
    return ((uint32_t)(tm.year - 1980) << 25) | ((uint32_t)tm.month << 21) | ((uint32_t)tm.day << 16) |
           ((uint32_t)tm.hour << 11) | ((uint32_t)tm.minute << 5) | (uint32_t)(tm.second / 2);
}

/* ---------- UTF ---------- */
static size_t put_utf8(char *out, size_t pos, size_t size, uint16_t c)
{
    if (c < 0x80) {
        if (pos + 1 < size) out[pos++] = (char)c;
    } else if (c < 0x800) {
        if (pos + 2 < size) { out[pos++] = (char)(0xC0 | (c >> 6)); out[pos++] = (char)(0x80 | (c & 0x3F)); }
    } else if (c >= 0xD800 && c < 0xE000) {
        if (pos + 1 < size) out[pos++] = '?';
    } else if (pos + 3 < size) {
        out[pos++] = (char)(0xE0 | (c >> 12));
        out[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (c & 0x3F));
    }
    return pos;
}

static int utf8_to_utf16(const char *s, uint16_t *out, int max)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c;
        if (n >= max) return -1;
        if (p[0] < 0x80) { c = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { c = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            c = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3F); p += 3;
        } else return -1;
        out[n++] = (uint16_t)c;
    }
    return n;
}

static uint16_t upc(struct exfat_fs *fs, uint16_t c)
{
    if (fs->up) return fs->up[c];
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
}

/* ---------- cluster, FAT, bitmap ---------- */
static uint64_t cl_off(struct exfat_fs *fs, uint32_t c) { return fs->heap_off + (uint64_t)(c - 2) * fs->csize; }

static uint32_t fat_get(struct exfat_fs *fs, uint32_t c)
{
    uint8_t b[4];
    if (block_read_bytes(fs->dev, fs->fat_off + (uint64_t)c * 4, b, 4) < 0) return EOC;
    return rd32(b);
}

static int fat_set(struct exfat_fs *fs, uint32_t c, uint32_t v)
{
    uint8_t b[4];
    wr32(b, v);
    return block_write_bytes(fs->dev, fs->fat_off + (uint64_t)c * 4, b, 4);
}

static int valid_cl(struct exfat_fs *fs, uint32_t c) { return c >= 2 && c < fs->clusters + 2; }

static uint32_t next_cl(struct exfat_fs *fs, uint32_t c, int nofat)
{
    uint32_t n = nofat ? c + 1 : fat_get(fs, c);
    return valid_cl(fs, n) ? n : EOC;
}

/* n-তম cluster (০ থেকে); নেই = EOC */
static uint32_t nth_cl(struct exfat_fs *fs, uint32_t first, int nofat, uint64_t n)
{
    uint32_t c = first;
    if (!valid_cl(fs, first)) return EOC;
    if (nofat) return n < fs->clusters && valid_cl(fs, (uint32_t)(first + n)) ? (uint32_t)(first + n) : EOC;
    while (n-- && c != EOC) c = next_cl(fs, c, 0);
    return c;
}

static int bm_test(struct exfat_fs *fs, uint32_t c) { c -= 2; return (fs->bm[c >> 3] >> (c & 7)) & 1; }

static int bm_flush_byte(struct exfat_fs *fs, uint32_t byte)
{
    uint32_t ci = byte / fs->csize;
    if (ci >= fs->bm_ncl) return -EIO;
    return block_write_bytes(fs->dev, cl_off(fs, fs->bm_cl[ci]) + byte % fs->csize, fs->bm + byte, 1);
}

static int bm_set(struct exfat_fs *fs, uint32_t c, int used)
{
    uint32_t i = c - 2;
    if (used) fs->bm[i >> 3] |= (uint8_t)(1u << (i & 7));
    else fs->bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
    return bm_flush_byte(fs, i >> 3);
}

static int alloc_cl(struct exfat_fs *fs, uint32_t near, uint32_t *out)
{
    uint32_t i, c;
    if (!fs->free_count) return -ENOSPC;
    c = valid_cl(fs, near) ? near : (valid_cl(fs, fs->hint) ? fs->hint : 2);
    for (i = 0; i < fs->clusters; i++, c++) {
        if (!valid_cl(fs, c)) c = 2;
        if (!bm_test(fs, c)) {
            int err = bm_set(fs, c, 1);
            if (err < 0) return err;
            fs->free_count--;
            fs->hint = c + 1;
            *out = c;
            return 0;
        }
    }
    return -ENOSPC;
}

static void free_chain(struct exfat_fs *fs, uint32_t first, int nofat, uint64_t count)
{
    uint32_t c = first;
    while (valid_cl(fs, c) && count--) {
        uint32_t n = next_cl(fs, c, nofat);
        if (bm_test(fs, c)) { bm_set(fs, c, 0); fs->free_count++; }
        if (!nofat) fat_set(fs, c, 0);
        c = n;
    }
}

static int zero_cl(struct exfat_fs *fs, uint32_t c)
{
    uint8_t *z = (uint8_t *)kzalloc(fs->csize);
    int err;
    if (!z) return -ENOMEM;
    err = block_write_bytes(fs->dev, cl_off(fs, c), z, fs->csize);
    kfree(z);
    return err;
}

/* NoFatChain ফাইলের ncl-টা cluster-এর FAT chain লেখা (তারপর FAT দিয়েই চলে) */
static int make_chain(struct exfat_fs *fs, uint32_t first, uint64_t ncl)
{
    uint64_t i;
    for (i = 0; i < ncl; i++) {
        int err = fat_set(fs, (uint32_t)(first + i), i + 1 < ncl ? (uint32_t)(first + i + 1) : EOC);
        if (err < 0) return err;
    }
    return 0;
}

/* ফাইলের cluster সংখ্যা need পর্যন্ত বাড়ানো (have = এখন কয়টা) */
static int grow(struct exfat_fs *fs, uint32_t *first, int *nofat, uint64_t have, uint64_t need, int zero)
{
    uint32_t last = EOC, c;
    int err;
    if (have >= need) return 0;
    if (have) {
        if (*nofat) {
            err = make_chain(fs, *first, have);
            if (err < 0) return err;
            *nofat = 0;
        }
        last = nth_cl(fs, *first, 0, have - 1);
        if (last == EOC) return -EIO;
    }
    while (have < need) {
        err = alloc_cl(fs, last == EOC ? 0 : last + 1, &c);
        if (err < 0) return err;
        if (zero && (err = zero_cl(fs, c)) < 0) return err;
        fat_set(fs, c, EOC);
        if (last == EOC) { *first = c; *nofat = 0; }
        else fat_set(fs, last, c);
        last = c;
        have++;
    }
    return 0;
}

/* ---------- ডিরেক্টরির slot (৩২ বাইট) ---------- */
static int slot_off(struct exfat_vnode *d, uint32_t idx, uint64_t *off)
{
    struct exfat_fs *fs = d->fs;
    uint64_t b = (uint64_t)idx * 32;
    uint32_t c;
    if (b >= d->dsize) return -ENOENT;
    c = nth_cl(fs, d->first, d->nofat, b / fs->csize);
    if (c == EOC) return -EIO;
    *off = cl_off(fs, c) + b % fs->csize;
    return 0;
}

static int slot_rw(struct exfat_vnode *d, uint32_t idx, uint8_t *e, int write)
{
    uint64_t off;
    int err = slot_off(d, idx, &off);
    if (err < 0) return err;
    return write ? block_write_bytes(d->fs->dev, off, e, 32) : block_read_bytes(d->fs->dev, off, e, 32);
}

struct xent {
    uint32_t slot, nsec;
    uint16_t attr;
    uint32_t mtime, first;
    int      nofat;
    uint64_t size;
    char     name[VFS_NAME_MAX + 1];
};

typedef int (*xvisit)(const struct xent *x, void *ctx);   /* ১ = থামো */

static int walk_dir(struct exfat_vnode *d, xvisit fn, void *ctx)
{
    uint8_t e[32], s[32];
    uint32_t idx = 0, total = (uint32_t)(d->dsize / 32);
    while (idx < total) {
        int err = slot_rw(d, idx, e, 0);
        if (err < 0) return err;
        if (e[0] == 0) break;
        if (e[0] == 0x85) {
            struct xent x;
            uint32_t k, nsec = e[1];
            size_t pos = 0;
            int nl = 0, got = 0, ok = 1;
            memset(&x, 0, sizeof(x));
            x.slot = idx;
            x.nsec = nsec;
            x.attr = rd16(e + 4);
            x.mtime = rd32(e + 12);
            for (k = 1; k <= nsec && ok; k++) {
                if (slot_rw(d, idx + k, s, 0) < 0) { ok = 0; break; }
                if (s[0] == 0xC0) {
                    nl = s[3];
                    x.nofat = (s[1] & SF_NOFAT) != 0;
                    x.first = rd32(s + 20);
                    x.size = rd64(s + 24);
                    got = 1;
                } else if (s[0] == 0xC1) {
                    int i;
                    for (i = 0; i < 15 && nl > 0; i++, nl--) pos = put_utf8(x.name, pos, sizeof(x.name), rd16(s + 2 + 2 * i));
                } else if (!(s[0] & 0x80)) ok = 0;
            }
            x.name[pos] = '\0';
            if (ok && got && x.name[0]) {
                int r = fn(&x, ctx);
                if (r) return r;
            }
            idx += 1 + nsec;
            continue;
        }
        idx++;
    }
    return 0;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ---------- vnode ---------- */
static struct exfat_vnode *get_vnode(struct exfat_vnode *dir, const struct xent *x)
{
    struct exfat_fs *fs = dir->fs;
    struct exfat_vnode *v;
    for (v = fs->vnodes; v; v = v->next) {
        if (!v->is_root && v->pdir == dir && v->slot == x->slot && v->vn.nlink) {
            vnode_ref(&v->vn);
            return v;
        }
    }
    v = (struct exfat_vnode *)kzalloc(sizeof(*v));
    if (!v) return NULL;
    v->fs = fs;
    v->first = x->first;
    v->nofat = x->nofat;
    v->pdir = dir;
    v->slot = x->slot;
    v->nsec = x->nsec;
    v->attr = x->attr;
    v->vn.type = (x->attr & ATTR_DIR) ? VNODE_DIR : VNODE_FILE;
    v->vn.mode = 0755;
    v->vn.size = v->vn.type == VNODE_DIR ? 0 : x->size;
    v->dsize = v->vn.type == VNODE_DIR ? x->size : 0;
    v->vn.inode = ((uint64_t)(uintptr_t)dir << 20) ^ x->slot ^ ((uint64_t)x->first << 32);
    v->vn.mtime = ts_to_unix(x->mtime);
    v->vn.nlink = 1;
    v->vn.refcount = 1;
    v->vn.ops = &exfat_ops;
    v->vn.fs_data = fs;
    if (v->vn.type == VNODE_DIR) v->vn.parent = &dir->vn;
    v->vn.mount = dir->vn.mount;
    v->next = fs->vnodes;
    fs->vnodes = v;
    return v;
}

/* entry set-এর checksum (প্রথম entry-র বাইট ২-৩ বাদ) */
static uint16_t set_checksum(const uint8_t *set, uint32_t n)
{
    uint16_t c = 0;
    uint32_t i;
    for (i = 0; i < n * 32; i++) {
        if (i == 2 || i == 3) continue;
        c = (uint16_t)(((c & 1) ? 0x8000 : 0) + (c >> 1) + set[i]);
    }
    return c;
}

/* vnode-এর বর্তমান তথ্য (প্রথম cluster, মাপ, সময়) তার entry set-এ লেখা */
static int update_entry(struct exfat_vnode *v)
{
    uint8_t *set;
    uint32_t n = v->nsec + 1, i;
    int err = 0;
    if (v->is_root || !v->pdir) return 0;
    set = (uint8_t *)kmalloc(n * 32);
    if (!set) return -ENOMEM;
    for (i = 0; i < n && err >= 0; i++) err = slot_rw(v->pdir, v->slot + i, set + i * 32, 0);
    if (err >= 0) {
        uint64_t len = v->vn.type == VNODE_DIR ? v->dsize : v->vn.size;
        uint32_t ts = now_ts();
        if (set[0] != 0x85 || set[32] != 0xC0) { kfree(set); return -EIO; }
        wr32(set + 12, ts);
        wr32(set + 16, ts);
        set[1 * 32 + 1] = (uint8_t)((v->first ? SF_ALLOC : 0) | (v->nofat ? SF_NOFAT : 0));
        wr32(set + 32 + 20, v->first);
        wr64(set + 32 + 8, len);
        wr64(set + 32 + 24, len);
        wr16(set + 2, set_checksum(set, n));
        for (i = 0; i < n && err >= 0; i++) err = slot_rw(v->pdir, v->slot + i, set + i * 32, 1);
    }
    kfree(set);
    v->vn.mtime = (uint64_t)clock_unix_time();
    return err < 0 ? err : 0;
}

/* ---------- পড়া/লেখা ---------- */
static int64_t exfat_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct exfat_vnode *v = XV(vn);
    struct exfat_fs *fs = v->fs;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    uint32_t c;
    uint64_t ci;
    if (vn->type == VNODE_DIR) return -EISDIR;
    if (off >= vn->size) return 0;
    if (len > vn->size - off) len = (size_t)(vn->size - off);
    ci = off / fs->csize;
    c = nth_cl(fs, v->first, v->nofat, ci);
    while (done < len) {
        uint64_t in = (off + done) % fs->csize;
        size_t n = (size_t)(fs->csize - in);
        if (c == EOC) return done ? (int64_t)done : -EIO;
        if (n > len - done) n = len - done;
        if (block_read_bytes(fs->dev, cl_off(fs, c) + in, out + done, n) < 0) return -EIO;
        done += n;
        if (done < len) c = next_cl(fs, c, v->nofat);
    }
    return (int64_t)done;
}

static uint64_t ncl_of(struct exfat_fs *fs, uint64_t bytes) { return (bytes + fs->csize - 1) / fs->csize; }

static int64_t exfat_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct exfat_vnode *v = XV(vn);
    struct exfat_fs *fs = v->fs;
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;
    uint64_t end = off + len;
    uint32_t c;
    int err;
    if (vn->type == VNODE_DIR) return -EISDIR;
    if (fs->read_only || block_is_read_only(fs->dev)) return -EROFS;
    if (!len) return 0;
    /* ফাঁক থাকলে (off > size) সেই অংশ শূন্য: নতুন cluster শূন্য করে নেওয়া হয় */
    err = grow(fs, &v->first, &v->nofat, ncl_of(fs, vn->size), ncl_of(fs, end), off > vn->size);
    if (err < 0) return err;
    if (off > vn->size && vn->size % fs->csize) {        /* শেষ পুরোনো cluster-এর বাকিটা শূন্য */
        uint32_t lc = nth_cl(fs, v->first, v->nofat, vn->size / fs->csize);
        size_t zl = (size_t)(fs->csize - vn->size % fs->csize);
        uint8_t *z = (uint8_t *)kzalloc(zl);
        if (z && lc != EOC) block_write_bytes(fs->dev, cl_off(fs, lc) + vn->size % fs->csize, z, zl);
        kfree(z);
    }
    c = nth_cl(fs, v->first, v->nofat, off / fs->csize);
    while (done < len) {
        uint64_t o = (off + done) % fs->csize;
        size_t n = (size_t)(fs->csize - o);
        if (c == EOC) return -EIO;
        if (n > len - done) n = len - done;
        if (block_write_bytes(fs->dev, cl_off(fs, c) + o, in + done, n) < 0) return -EIO;
        done += n;
        if (done < len) c = next_cl(fs, c, v->nofat);
    }
    if (end > vn->size) vn->size = end;
    err = update_entry(v);
    return err < 0 ? err : (int64_t)done;
}

static int exfat_truncate(struct vnode *vn, uint64_t size)
{
    struct exfat_vnode *v = XV(vn);
    struct exfat_fs *fs = v->fs;
    uint64_t have, want;
    int err;
    if (vn->type == VNODE_DIR) return -EISDIR;
    if (fs->read_only || block_is_read_only(fs->dev)) return -EROFS;
    have = ncl_of(fs, vn->size);
    want = ncl_of(fs, size);
    if (want > have) {
        err = grow(fs, &v->first, &v->nofat, have, want, 1);
        if (err < 0) return err;
    } else if (want < have) {
        if (want == 0) {
            free_chain(fs, v->first, v->nofat, have);
            v->first = 0;
            v->nofat = 0;
        } else {
            uint32_t last = nth_cl(fs, v->first, v->nofat, want - 1);
            if (last == EOC) return -EIO;
            free_chain(fs, v->nofat ? last + 1 : next_cl(fs, last, 0), v->nofat, have - want);
            if (!v->nofat) fat_set(fs, last, EOC);
        }
    }
    vn->size = size;
    return update_entry(v);
}

/* ---------- ডিরেক্টরি ---------- */
struct find_ctx { const char *name; struct xent found; int ok; };

static int find_fn(const struct xent *x, void *ctx)
{
    struct find_ctx *f = (struct find_ctx *)ctx;
    if (!name_eq(x->name, f->name)) return 0;
    f->found = *x;
    f->ok = 1;
    return 1;
}

static int find(struct exfat_vnode *d, const char *name, struct xent *out)
{
    struct find_ctx f;
    int r;
    f.name = name;
    f.ok = 0;
    r = walk_dir(d, find_fn, &f);
    if (r < 0) return r;
    if (!f.ok) return -ENOENT;
    *out = f.found;
    return 0;
}

static int exfat_lookup(struct vnode *dirvn, const char *name, struct vnode **out)
{
    struct xent x;
    struct exfat_vnode *c;
    int r;
    if (dirvn->type != VNODE_DIR) return -ENOTDIR;
    r = find(XV(dirvn), name, &x);
    if (r < 0) return r;
    c = get_vnode(XV(dirvn), &x);
    if (!c) return -ENOMEM;
    *out = &c->vn;
    return 0;
}

struct rd_ctx { uint64_t want, seen; struct vfs_dirent *out; };

static int rd_fn(const struct xent *x, void *ctx)
{
    struct rd_ctx *r = (struct rd_ctx *)ctx;
    if (r->seen++ != r->want) return 0;
    memcpy(r->out->name, x->name, sizeof(r->out->name));
    r->out->type = (x->attr & ATTR_DIR) ? VNODE_DIR : VNODE_FILE;
    r->out->inode = ((uint64_t)x->first << 32) ^ x->slot;
    r->out->size = (x->attr & ATTR_DIR) ? 0 : x->size;
    return 1;
}

static int exfat_readdir(struct vnode *dirvn, uint64_t index, struct vfs_dirent *out)
{
    struct rd_ctx r;
    r.want = index;
    r.seen = 0;
    r.out = out;
    return walk_dir(XV(dirvn), rd_fn, &r);
}

/* n-টা পরপর খালি slot; না থাকলে ডিরেক্টরি এক এক cluster করে বাড়ানো */
static int free_slots(struct exfat_vnode *d, uint32_t n, uint32_t *start)
{
    struct exfat_fs *fs = d->fs;
    uint8_t e[32];
    uint32_t idx, run = 0, total = (uint32_t)(d->dsize / 32);
    int err;
    *start = total;
    for (idx = 0; idx < total; idx++) {
        err = slot_rw(d, idx, e, 0);
        if (err < 0) return err;
        if (e[0] == 0) {                                 /* এখান থেকে শেষ পর্যন্ত সব খালি */
            if (!run) *start = idx;
            run = 1;
            break;
        }
        if (!(e[0] & 0x80)) {
            if (!run) *start = idx;
            if (++run >= n) return 0;
        } else {
            run = 0;
            *start = total;
        }
    }
    if (!run) *start = total;
    while ((uint64_t)(*start + n) * 32 > d->dsize) {
        uint64_t have = d->dsize / fs->csize;
        err = grow(fs, &d->first, &d->nofat, have, have + 1, 1);
        if (err < 0) return err;
        d->dsize += fs->csize;
        if (!d->is_root) {
            err = update_entry(d);
            if (err < 0) return err;
        }
    }
    return 0;
}

/* নতুন entry set লেখা */
static int write_set(struct exfat_vnode *d, const uint16_t *u, int nl, uint16_t attr, uint32_t first, int nofat,
                     uint64_t size, uint32_t mtime, uint32_t *slot_out, uint32_t *nsec_out)
{
    struct exfat_fs *fs = d->fs;
    uint32_t nsec = 1 + (uint32_t)((nl + 14) / 15), n = nsec + 1, i, start;
    uint8_t *set;
    uint16_t hash = 0;
    int err, k;
    err = free_slots(d, n, &start);
    if (err < 0) return err;
    set = (uint8_t *)kzalloc(n * 32);
    if (!set) return -ENOMEM;
    set[0] = 0x85;
    set[1] = (uint8_t)nsec;
    wr16(set + 4, attr);
    wr32(set + 8, mtime);
    wr32(set + 12, mtime);
    wr32(set + 16, mtime);
    set[32] = 0xC0;
    set[33] = (uint8_t)((first ? SF_ALLOC : 0) | (nofat ? SF_NOFAT : 0));
    set[35] = (uint8_t)nl;
    for (k = 0; k < nl; k++) {
        uint16_t c = upc(fs, u[k]);
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c & 0xFF));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c >> 8));
    }
    wr16(set + 36, hash);
    wr64(set + 40, size);
    wr32(set + 52, first);
    wr64(set + 56, size);
    for (k = 0; k < nl; k++) {
        uint8_t *ne = set + (2 + k / 15) * 32;
        ne[0] = 0xC1;
        wr16(ne + 2 + 2 * (k % 15), u[k]);
    }
    for (i = 2; i < n; i++) set[i * 32] = 0xC1;
    wr16(set + 2, set_checksum(set, n));
    for (i = 0; i < n && err >= 0; i++) err = slot_rw(d, start + i, set + i * 32, 1);
    kfree(set);
    *slot_out = start;
    *nsec_out = nsec;
    return err < 0 ? err : 0;
}

static int mark_deleted(struct exfat_vnode *d, uint32_t slot, uint32_t nsec)
{
    uint8_t e[32];
    uint32_t i;
    for (i = 0; i <= nsec; i++) {
        int err = slot_rw(d, slot + i, e, 0);
        if (err < 0) return err;
        e[0] &= 0x7F;
        err = slot_rw(d, slot + i, e, 1);
        if (err < 0) return err;
    }
    return 0;
}

static int name_ok(const char *name, uint16_t *u, int *nl)
{
    const char *p;
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    for (p = name; *p; p++) if (strchr("\"*/:<>?\\|", *p) || (unsigned char)*p < 32) return -EINVAL;
    *nl = utf8_to_utf16(name, u, 255);
    return *nl <= 0 ? -ENAMETOOLONG : 0;
}

static int exfat_create(struct vnode *dirvn, const char *name, enum vnode_type type, struct vnode **out)
{
    struct exfat_vnode *d = XV(dirvn), *c;
    struct exfat_fs *fs = d->fs;
    uint16_t u[256];
    struct xent x;
    uint32_t first = 0, slot, nsec;
    int nl, err, nofat = 0;
    if (dirvn->type != VNODE_DIR) return -ENOTDIR;
    if (fs->read_only || block_is_read_only(fs->dev)) return -EROFS;
    err = name_ok(name, u, &nl);
    if (err < 0) return err;
    if (find(d, name, &x) == 0) return -EEXIST;
    if (type == VNODE_DIR) {
        err = alloc_cl(fs, 0, &first);
        if (err < 0) return err;
        err = zero_cl(fs, first);
        if (err < 0) return err;
        nofat = 1;
    }
    err = write_set(d, u, nl, type == VNODE_DIR ? ATTR_DIR : 0x20, first, nofat,
                    type == VNODE_DIR ? fs->csize : 0, now_ts(), &slot, &nsec);
    if (err < 0) {
        if (first) free_chain(fs, first, 1, 1);
        return err;
    }
    memset(&x, 0, sizeof(x));
    x.slot = slot;
    x.nsec = nsec;
    x.attr = type == VNODE_DIR ? ATTR_DIR : 0x20;
    x.mtime = now_ts();
    x.first = first;
    x.nofat = nofat;
    x.size = type == VNODE_DIR ? fs->csize : 0;
    c = get_vnode(d, &x);
    if (!c) return -ENOMEM;
    if (out) *out = &c->vn;
    else vnode_unref(&c->vn);
    return 0;
}

static int dir_empty(struct exfat_vnode *d)
{
    struct rd_ctx r;
    struct vfs_dirent de;
    r.want = 0;
    r.seen = 0;
    r.out = &de;
    return walk_dir(d, rd_fn, &r) == 0;
}

static int exfat_unlink(struct vnode *dirvn, const char *name)
{
    struct exfat_vnode *d = XV(dirvn), *v;
    struct exfat_fs *fs = d->fs;
    struct xent x;
    int err;
    if (fs->read_only || block_is_read_only(fs->dev)) return -EROFS;
    err = find(d, name, &x);
    if (err < 0) return err;
    if (x.attr & ATTR_DIR) {
        struct exfat_vnode tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.fs = fs;
        tmp.first = x.first;
        tmp.nofat = x.nofat;
        tmp.dsize = x.size;
        if (!dir_empty(&tmp)) return -ENOTEMPTY;
    }
    err = mark_deleted(d, x.slot, x.nsec);
    if (err < 0) return err;
    for (v = fs->vnodes; v; v = v->next) {
        if (v->pdir == d && v->slot == x.slot && v->vn.nlink) {
            v->vn.nlink = 0;                            /* খোলা থাকলে release-এ cluster ফেরত */
            v->pdir = NULL;
            if (v->vn.refcount == 0) { free_chain(fs, v->first, v->nofat, ncl_of(fs, x.size)); v->first = 0; }
            return 0;
        }
    }
    free_chain(fs, x.first, x.nofat, ncl_of(fs, x.size));
    return 0;
}

static int exfat_rename(struct vnode *odvn, const char *oname, struct vnode *ndvn, const char *nname)
{
    struct exfat_vnode *od = XV(odvn), *nd = XV(ndvn), *v;
    struct exfat_fs *fs = od->fs;
    struct xent x, y;
    uint16_t u[256];
    uint32_t slot, nsec;
    int nl, err;
    if (fs->read_only || block_is_read_only(fs->dev)) return -EROFS;
    err = name_ok(nname, u, &nl);
    if (err < 0) return err;
    err = find(od, oname, &x);
    if (err < 0) return err;
    if (find(nd, nname, &y) == 0) {
        if (y.slot == x.slot && od == nd) return 0;
        if (y.attr & ATTR_DIR) return -EEXIST;
        err = exfat_unlink(ndvn, nname);
        if (err < 0) return err;
        if (od == nd) { err = find(od, oname, &x); if (err < 0) return err; }
    }
    err = write_set(nd, u, nl, x.attr, x.first, x.nofat, x.size, x.mtime, &slot, &nsec);
    if (err < 0) return err;
    err = mark_deleted(od, x.slot, x.nsec);
    for (v = fs->vnodes; v; v = v->next) {
        if (v->pdir == od && v->slot == x.slot && v->vn.nlink) {
            v->pdir = nd;
            v->slot = slot;
            v->nsec = nsec;
            if (v->vn.type == VNODE_DIR) v->vn.parent = &nd->vn;
            break;
        }
    }
    return err;
}

static void exfat_release(struct vnode *vn)
{
    struct exfat_vnode *v = XV(vn);
    if (v->vn.nlink == 0 && !v->is_root && v->first) {
        free_chain(v->fs, v->first, v->nofat, ncl_of(v->fs, v->vn.type == VNODE_DIR ? v->dsize : v->vn.size));
        v->first = 0;
    }
}

static const struct vnode_ops exfat_ops = {
    exfat_read, exfat_write, exfat_truncate, exfat_lookup, exfat_create,
    exfat_unlink, exfat_readdir, exfat_release, NULL, exfat_rename, NULL
};

/* ---------- mount ---------- */
static int exfat_mount(const void *arg, struct vnode **root)
{
    struct block_device *dev = block_find((const char *)arg);
    struct exfat_fs *fs;
    struct exfat_vnode *r;
    uint8_t bs[512], e[32];
    uint32_t idx, c, ncl = 0, i;
    int err = 0;

    if (!dev) return -ENODEV;
    if (block_read_bytes(dev, 0, bs, sizeof(bs)) < 0) return -EIO;
    if (memcmp(bs + 3, "EXFAT   ", 8) || bs[108] < 9 || bs[108] > 12 || bs[109] > 25 - bs[108]) return -EINVAL;
    fs = (struct exfat_fs *)kzalloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    fs->dev = dev;
    fs->bps = 1u << bs[108];
    fs->csize = fs->bps << bs[109];
    fs->fat_off = (uint64_t)rd32(bs + 80) * fs->bps;
    fs->heap_off = (uint64_t)rd32(bs + 88) * fs->bps;
    fs->clusters = rd32(bs + 92);
    fs->root = rd32(bs + 96);
    fs->serial = rd32(bs + 100);
    fs->read_only = block_is_read_only(dev);

    r = (struct exfat_vnode *)kzalloc(sizeof(*r));
    if (!r) { kfree(fs); return -ENOMEM; }
    r->fs = fs;
    r->is_root = 1;
    r->first = fs->root;
    r->nofat = 0;
    for (c = fs->root; c != EOC && ncl <= fs->clusters; c = next_cl(fs, c, 0)) ncl++;
    r->dsize = (uint64_t)ncl * fs->csize;
    /* root-এর বিশেষ entry: bitmap, upcase, label */
    for (idx = 0; idx < r->dsize / 32; idx++) {
        if (slot_rw(r, idx, e, 0) < 0 || e[0] == 0) break;
        if (e[0] == 0x81 && !fs->bm) {
            uint32_t bc = rd32(e + 20);
            uint64_t bsz = rd64(e + 24);
            if (bsz < (fs->clusters + 7) / 8 || bsz > 64ull * 1024 * 1024) { err = -EINVAL; break; }
            fs->bm_bytes = (uint32_t)bsz;
            fs->bm = (uint8_t *)kmalloc(fs->bm_bytes);
            fs->bm_ncl = (uint32_t)ncl_of(fs, bsz);
            fs->bm_cl = (uint32_t *)kmalloc(sizeof(uint32_t) * fs->bm_ncl);
            if (!fs->bm || !fs->bm_cl) { err = -ENOMEM; break; }
            for (i = 0; i < fs->bm_ncl; i++) {
                fs->bm_cl[i] = bc;
                if (!valid_cl(fs, bc) ||
                    block_read_bytes(dev, cl_off(fs, bc), fs->bm + (uint64_t)i * fs->csize,
                                     i + 1 < fs->bm_ncl ? fs->csize : fs->bm_bytes - i * fs->csize) < 0) { err = -EIO; break; }
                bc = next_cl(fs, bc, 0);
                if (bc == EOC && i + 1 < fs->bm_ncl) bc = fs->bm_cl[i] + 1;   /* chain না থাকলে একটানা */
            }
        } else if (e[0] == 0x82 && !fs->up) {
            uint32_t uc = rd32(e + 20);
            uint64_t usz = rd64(e + 24);
            uint8_t *raw = usz && usz <= 262144 ? (uint8_t *)kmalloc((size_t)usz) : NULL;
            uint64_t o = 0, k;
            uint32_t ch = 0;
            if (raw) {
                for (o = 0; o < usz; o += fs->csize) {
                    size_t n = usz - o < fs->csize ? (size_t)(usz - o) : fs->csize;
                    uint32_t prev = uc;
                    if (!valid_cl(fs, uc) || block_read_bytes(dev, cl_off(fs, uc), raw + o, n) < 0) break;
                    uc = next_cl(fs, uc, 0);
                    if (uc == EOC) uc = prev + 1;            /* chain না থাকলে একটানা */
                }
                fs->up = (uint16_t *)kmalloc(65536 * 2);
                if (fs->up) {
                    for (k = 0; k < 65536; k++) fs->up[k] = (uint16_t)k;
                    for (k = 0; k + 1 < usz && ch < 65536; k += 2) {
                        uint16_t v = rd16(raw + k);
                        if (v == 0xFFFF && k + 3 < usz) { ch += rd16(raw + k + 2); k += 2; }   /* একই রকম অনেকগুলো */
                        else fs->up[ch++] = v;
                    }
                }
                kfree(raw);
            }
        } else if (e[0] == 0x83) {
            int n = e[1] > 11 ? 11 : e[1], k;
            size_t pos = 0;
            for (k = 0; k < n; k++) pos = put_utf8(fs->label, pos, sizeof(fs->label), rd16(e + 2 + 2 * k));
            fs->label[pos] = '\0';
        }
    }
    if (!err && !fs->bm) err = -EINVAL;
    if (err < 0) {
        kfree(fs->bm); kfree(fs->bm_cl); kfree(fs->up); kfree(r); kfree(fs);
        return err;
    }
    for (c = 0; c < fs->clusters; c++) if (!((fs->bm[c >> 3] >> (c & 7)) & 1)) fs->free_count++;
    fs->hint = 2;
    r->vn.type = VNODE_DIR;
    r->vn.mode = 0755;
    r->vn.inode = 1;
    r->vn.nlink = 1;
    r->vn.refcount = 1;
    r->vn.ops = &exfat_ops;
    r->vn.fs_data = fs;
    fs->vnodes = r;
    *root = &r->vn;
    return 0;
}

static int exfat_statfs(struct vnode *root, struct vfs_statfs *out)
{
    struct exfat_fs *fs = XV(root)->fs;
    size_t n = strlen(fs->dev->name);
    out->block_size = fs->csize;
    out->total_blocks = fs->clusters;
    out->free_blocks = fs->free_count;
    out->total_inodes = 0;
    out->free_inodes = 0;
    out->read_only = fs->read_only || block_is_read_only(fs->dev);
    if (n >= sizeof(out->source)) n = sizeof(out->source) - 1;
    memcpy(out->source, fs->dev->name, n);
    out->source[n] = '\0';
    return 0;
}

static void exfat_info(struct vnode *root)
{
    struct exfat_fs *fs = XV(root)->fs;
    kprintf("      type            exFAT\n");
    kprintf("      label           \"%s\"   serial %04X-%04X\n", fs->label, (unsigned)(fs->serial >> 16),
            (unsigned)(fs->serial & 0xFFFF));
    kprintf("      cluster size    %u bytes, %u clusters, %u free (%llu MiB free)\n", (unsigned)fs->csize,
            (unsigned)fs->clusters, (unsigned)fs->free_count,
            (unsigned long long)((uint64_t)fs->free_count * fs->csize / (1024 * 1024)));
    kprintf("      upcase table    %s\n", fs->up ? "loaded" : "missing (ASCII only)");
}

static int exfat_umount(struct vnode *root)
{
    struct exfat_vnode *v;
    for (v = XV(root)->fs->vnodes; v; v = v->next) {
        if (&v->vn != root && v->vn.refcount > 0) return -EBUSY;
    }
    return 0;
}

const char *exfat_label(struct vnode *root) { return XV(root)->fs->label; }

const struct filesystem exfat_filesystem = { "exfat", exfat_mount, exfat_statfs, exfat_info, exfat_umount, exfat_label };
