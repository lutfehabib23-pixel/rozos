/* rozOS — FAT16/FAT32 ফাইলসিস্টেম, পড়া ও লেখা (ধাপ ১১খ)
 *
 * কেন: Limine (bootloader) শুধু FAT আর ISO9660 পড়ে, ext2 নয়। তাই boot partition (ESP)
 * FAT32 — kernel-গুলো আর limine.conf সেখানে থাকে, rozOS সেটা /boot-এ mount করে আর
 * kinstall দিয়ে নতুন kernel লেখে।
 *
 * গঠন (Microsoft-এর FAT spec অনুযায়ী):
 *   sector 0 = boot sector/BPB  → তারপর reserved sector → FAT-এর কপি (সাধারণত ২টা)
 *   → (FAT16: স্থির মাপের root ডিরেক্টরি) → data অঞ্চল, cluster 2 থেকে শুরু
 *   FAT = প্রতিটি cluster-এর "পরেরটা কোন cluster" তালিকা; 0 = খালি, EOC = শেষ
 *   ডিরেক্টরি = ৩২-বাইট এন্ট্রির তালিকা; লম্বা নাম (LFN) আলাদা এন্ট্রিতে UTF-16-এ,
 *   আসল (8.3) এন্ট্রির ঠিক আগে, উল্টো ক্রমে।
 *
 * FAT-এ inode নেই — একটা ফাইলের পরিচয় = তার ৩২-বাইট এন্ট্রির ডিস্ক-অফসেট (vnode-এর
 * inode নম্বরও তাই)। rename নেই (VFS-এও নেই)। FAT12 সাপোর্ট নেই (ফ্লপির জন্য)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/block.h>
#include <rozos/errno.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/clock.h>
#include <rozos/printf.h>
#include <rozos/fat.h>

#define ATTR_RO      0x01
#define ATTR_HIDDEN  0x02
#define ATTR_SYSTEM  0x04
#define ATTR_VOLID   0x08
#define ATTR_DIR     0x10
#define ATTR_ARCH    0x20
#define ATTR_LFN     0x0F

#define NTRES_LOWER_BASE 0x08
#define NTRES_LOWER_EXT  0x10

#define FAT_MAX_FILE 0xFFFFFFFFULL

struct fat_fs {
    struct block_device *dev;
    int      type;                       /* 16 বা 32 */
    uint32_t bps, spc, cluster_size;
    uint32_t reserved, nfats, fat_sectors;
    uint32_t root_entries, root_sectors; /* FAT16-এর স্থির root */
    uint32_t first_data_sector, total_sectors, clusters;
    uint32_t root_cluster;               /* FAT32 */
    uint32_t fsinfo_sector;
    uint32_t free_count, next_free;
    uint32_t serial;
    char     label[12], oem[9];
    struct fat_vnode *vnodes;
    /* dir_slot()-এর ছোট cache: একই chain-এ বারবার হাঁটা এড়াতে */
    uint32_t cache_first, cache_index, cache_cluster;
};

struct fat_vnode {
    struct vnode vn;                     /* প্রথম সদস্য */
    struct fat_fs *fs;
    uint32_t first;                      /* প্রথম cluster (0 = খালি ফাইল / FAT16 root) */
    int      is_root;
    /* parent ডিরেক্টরিতে এন্ট্রির জায়গা (root-এর নেই) */
    uint32_t dir_first;                  /* parent-এর প্রথম cluster (0 = FAT16 root) */
    uint32_t slot;                       /* 8.3 এন্ট্রির index */
    uint32_t nslots;                     /* LFN সহ মোট এন্ট্রি */
    uint64_t key;                        /* 8.3 এন্ট্রির ডিস্ক-অফসেট */
    struct fat_vnode *next;
};

static const struct vnode_ops fat_ops;

static inline struct fat_vnode *FV(struct vnode *vn) { return (struct fat_vnode *)vn; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ---------- সময়: FAT-এর তারিখ/সময় ↔ UNIX ---------- */

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

static uint64_t fat_to_unix(uint16_t date, uint16_t time)
{
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1) return 0;
    return (uint64_t)(days_from_civil(y, m, d) * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2);
}

static void unix_to_fat(int64_t t, uint16_t *date, uint16_t *time)
{
    struct clock_tm tm;
    if (t <= 0) t = 315532800;                     /* 1980-01-01 */
    clock_to_tm(t, &tm);
    if (tm.year < 1980) tm.year = 1980;
    *date = (uint16_t)(((tm.year - 1980) << 9) | (tm.month << 5) | tm.day);
    *time = (uint16_t)((tm.hour << 11) | (tm.minute << 5) | (tm.second / 2));
}

/* ---------- FAT টেবিল ---------- */

static uint64_t cluster_off(struct fat_fs *fs, uint32_t c)
{
    return ((uint64_t)fs->first_data_sector + (uint64_t)(c - 2) * fs->spc) * fs->bps;
}

static int is_eoc(struct fat_fs *fs, uint32_t v)
{
    return fs->type == 32 ? v >= 0x0FFFFFF8u : v >= 0xFFF8u;
}

static uint32_t eoc_value(struct fat_fs *fs)
{
    return fs->type == 32 ? 0x0FFFFFFFu : 0xFFFFu;
}

static int fat_get(struct fat_fs *fs, uint32_t c, uint32_t *out)
{
    uint8_t b[4];
    uint64_t off = (uint64_t)fs->reserved * fs->bps + (uint64_t)c * (fs->type == 32 ? 4 : 2);
    if (c >= fs->clusters + 2) return -EIO;
    if (block_read_bytes(fs->dev, off, b, fs->type == 32 ? 4 : 2) < 0) return -EIO;
    *out = fs->type == 32 ? (rd32(b) & 0x0FFFFFFFu) : rd16(b);
    return 0;
}

static int fat_set(struct fat_fs *fs, uint32_t c, uint32_t v)
{
    uint32_t i;
    uint8_t b[4];
    if (c < 2 || c >= fs->clusters + 2) return -EIO;
    for (i = 0; i < fs->nfats; i++) {
        uint64_t off = ((uint64_t)fs->reserved + (uint64_t)i * fs->fat_sectors) * fs->bps +
                       (uint64_t)c * (fs->type == 32 ? 4 : 2);
        if (fs->type == 32) {
            if (block_read_bytes(fs->dev, off, b, 4) < 0) return -EIO;
            wr32(b, (rd32(b) & 0xF0000000u) | (v & 0x0FFFFFFFu));   /* ওপরের ৪ বিট সংরক্ষিত */
            if (block_write_bytes(fs->dev, off, b, 4) < 0) return -EIO;
        } else {
            wr16(b, (uint16_t)v);
            if (block_write_bytes(fs->dev, off, b, 2) < 0) return -EIO;
        }
    }
    return 0;
}

/* FAT32-এর FSInfo sector-এ খালি cluster-এর হিসাব (অন্য OS যেন জানে) */
static void update_fsinfo(struct fat_fs *fs)
{
    uint8_t b[8];
    if (fs->type != 32 || fs->fsinfo_sector == 0 || fs->fsinfo_sector == 0xFFFF) return;
    wr32(b, fs->free_count);
    wr32(b + 4, fs->next_free);
    block_write_bytes(fs->dev, (uint64_t)fs->fsinfo_sector * fs->bps + 488, b, 8);
}

static int zero_cluster(struct fat_fs *fs, uint32_t c)
{
    uint8_t *z = (uint8_t *)kzalloc(fs->cluster_size);
    int err;
    if (!z) return -ENOMEM;
    err = block_write_bytes(fs->dev, cluster_off(fs, c), z, fs->cluster_size);
    kfree(z);
    return err < 0 ? -EIO : 0;
}

/* নতুন cluster (EOC চিহ্নিত), prev থাকলে তার পরে জোড়া; zero = শূন্য দিয়ে ভরা */
static int alloc_cluster(struct fat_fs *fs, uint32_t prev, int zero, uint32_t *out)
{
    uint32_t i, c, v;
    if (fs->free_count == 0) return -ENOSPC;
    c = fs->next_free >= 2 && fs->next_free < fs->clusters + 2 ? fs->next_free : 2;
    for (i = 0; i < fs->clusters; i++, c++) {
        if (c >= fs->clusters + 2) c = 2;
        if (fat_get(fs, c, &v) < 0) return -EIO;
        if (v == 0) break;
    }
    if (i == fs->clusters) {
        fs->free_count = 0;
        return -ENOSPC;
    }
    if (fat_set(fs, c, eoc_value(fs)) < 0) return -EIO;
    if (zero && zero_cluster(fs, c) < 0) {
        fat_set(fs, c, 0);
        return -EIO;
    }
    if (prev && fat_set(fs, prev, c) < 0) return -EIO;
    fs->free_count--;
    fs->next_free = c + 1;
    update_fsinfo(fs);
    *out = c;
    return 0;
}

static void free_chain(struct fat_fs *fs, uint32_t c)
{
    uint32_t next, guard = 0;
    while (c >= 2 && c < fs->clusters + 2 && guard++ <= fs->clusters) {
        if (fat_get(fs, c, &next) < 0) break;
        fat_set(fs, c, 0);
        fs->free_count++;
        if (c < fs->next_free) fs->next_free = c;
        if (is_eoc(fs, next) || next < 2) break;
        c = next;
    }
    fs->cache_first = 0;
    update_fsinfo(fs);
}

/* chain-এর n-তম cluster; শেষ পেরোলে 0 */
static uint32_t chain_nth(struct fat_fs *fs, uint32_t c, uint32_t n)
{
    uint32_t v;
    while (n--) {
        if (fat_get(fs, c, &v) < 0 || is_eoc(fs, v) || v < 2) return 0;
        c = v;
    }
    return c;
}

/* ---------- ডিরেক্টরির ৩২-বাইট এন্ট্রি ---------- */

/* ডিরেক্টরির idx-তম এন্ট্রির ডিস্ক-অফসেট। ফেরত 0 = আছে, 1 = ডিরেক্টরির শেষ, <0 ত্রুটি */
static int dir_slot(struct fat_fs *fs, uint32_t first, uint32_t idx, uint64_t *off)
{
    uint32_t per = fs->cluster_size / 32, want = idx / per, c, k;
    if (first == 0) {                                  /* FAT16-এর স্থির root */
        if (idx >= fs->root_entries) return 1;
        *off = ((uint64_t)fs->reserved + (uint64_t)fs->nfats * fs->fat_sectors) * fs->bps + (uint64_t)idx * 32;
        return 0;
    }
    if (fs->cache_first == first && fs->cache_index <= want) {
        c = fs->cache_cluster;
        k = fs->cache_index;
    } else {
        c = first;
        k = 0;
    }
    while (k < want) {
        uint32_t v;
        if (fat_get(fs, c, &v) < 0) return -EIO;
        if (is_eoc(fs, v) || v < 2) return 1;
        c = v;
        k++;
    }
    fs->cache_first = first;
    fs->cache_index = want;
    fs->cache_cluster = c;
    *off = cluster_off(fs, c) + (uint64_t)(idx % per) * 32;
    return 0;
}

static uint8_t lfn_checksum(const uint8_t *short_name)
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

/* UTF-16 → UTF-8 (BMP; surrogate হলে '?') */
static size_t put_utf8(char *out, size_t pos, size_t size, uint16_t c)
{
    if (c < 0x80) {
        if (pos + 1 < size) out[pos++] = (char)c;
    } else if (c < 0x800) {
        if (pos + 2 < size) { out[pos++] = (char)(0xC0 | (c >> 6)); out[pos++] = (char)(0x80 | (c & 0x3F)); }
    } else if (c >= 0xD800 && c < 0xE000) {
        if (pos + 1 < size) out[pos++] = '?';
    } else {
        if (pos + 3 < size) {
            out[pos++] = (char)(0xE0 | (c >> 12));
            out[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return pos;
}

/* UTF-8 → UTF-16 (BMP); ফেরত: কয়টা অক্ষর, ভুল হলে -1 */
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

static void short_to_name(const uint8_t *e, char *out, size_t size)
{
    size_t pos = 0;
    int i, end;
    for (end = 8; end > 0 && e[end - 1] == ' '; end--) {
    }
    for (i = 0; i < end && pos + 1 < size; i++) {
        uint8_t c = (i == 0 && e[0] == 0x05) ? 0xE5 : e[i];
        if ((e[12] & NTRES_LOWER_BASE) && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        out[pos++] = (char)c;
    }
    for (end = 11; end > 8 && e[end - 1] == ' '; end--) {
    }
    if (end > 8 && pos + 1 < size) {
        out[pos++] = '.';
        for (i = 8; i < end && pos + 1 < size; i++) {
            uint8_t c = e[i];
            if ((e[12] & NTRES_LOWER_EXT) && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
            out[pos++] = (char)c;
        }
    }
    out[pos] = '\0';
}

/* ডিরেক্টরি হাঁটা: প্রতিটি আসল এন্ট্রির (".", ".." আর volume label বাদে) জন্য visit।
 * visit 1 ফেরত দিলে থামে, সেটাই walk-এর ফেরত। */
struct dir_entry_info {
    const char *name;
    const uint8_t *e;                    /* ৩২-বাইট 8.3 এন্ট্রি */
    uint64_t off;                        /* তার ডিস্ক-অফসেট */
    uint32_t slot, nslots;
};

typedef int (*dir_visit_fn)(const struct dir_entry_info *info, void *ctx);

static int walk_dir(struct fat_fs *fs, uint32_t first, dir_visit_fn visit, void *ctx)
{
    uint8_t e[32];
    uint16_t lfn[260];
    char name[VFS_NAME_MAX + 1];
    uint32_t idx, lfn_start = 0;
    int lfn_count = 0, lfn_seen = 0;
    uint8_t lfn_sum = 0;

    for (idx = 0;; idx++) {
        uint64_t off;
        int r = dir_slot(fs, first, idx, &off);
        if (r < 0) return r;
        if (r > 0) return 0;
        if (block_read_bytes(fs->dev, off, e, 32) < 0) return -EIO;
        if (e[0] == 0x00) return 0;                        /* ডিরেক্টরির শেষ */
        if (e[0] == 0xE5) { lfn_count = 0; continue; }     /* মোছা */
        if ((e[11] & 0x3F) == ATTR_LFN) {
            int seq = e[0] & 0x1F, k;
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            if (e[0] & 0x40) {
                lfn_count = seq;
                lfn_seen = 0;
                lfn_sum = e[13];
                lfn_start = idx;
                memset(lfn, 0xFF, sizeof(lfn));
            }
            if (lfn_count == 0 || seq < 1 || seq > 20 || e[13] != lfn_sum) { lfn_count = 0; continue; }
            for (k = 0; k < 13; k++) lfn[(seq - 1) * 13 + k] = rd16(e + pos[k]);
            lfn_seen++;
            continue;
        }
        if (e[11] & ATTR_VOLID) { lfn_count = 0; continue; }
        {
            struct dir_entry_info info;
            int have_lfn = lfn_count > 0 && lfn_seen == lfn_count && lfn_sum == lfn_checksum(e) &&
                           lfn_start + (uint32_t)lfn_count == idx;
            if (have_lfn) {
                size_t p = 0;
                int k;
                for (k = 0; k < lfn_count * 13 && lfn[k] != 0x0000 && lfn[k] != 0xFFFF; k++) {
                    p = put_utf8(name, p, sizeof(name), lfn[k]);
                }
                name[p] = '\0';
            } else {
                short_to_name(e, name, sizeof(name));
            }
            info.slot = idx;
            info.nslots = have_lfn ? (uint32_t)lfn_count + 1 : 1;
            lfn_count = 0;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            info.name = name;
            info.e = e;
            info.off = off;
            r = visit(&info, ctx);
            if (r) return r;
        }
    }
}

static int name_eq(const char *a, const char *b)          /* ASCII case বাদ দিয়ে */
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ---------- vnode ---------- */

static uint32_t entry_cluster(struct fat_fs *fs, const uint8_t *e)
{
    uint32_t c = rd16(e + 26);
    if (fs->type == 32) c |= (uint32_t)rd16(e + 20) << 16;
    return c;
}

static struct fat_vnode *get_vnode(struct fat_fs *fs, const struct dir_entry_info *info,
                                   uint32_t dir_first, struct vnode *parent)
{
    struct fat_vnode *fv;
    for (fv = fs->vnodes; fv; fv = fv->next) {
        if (!fv->is_root && fv->key == info->off && fv->vn.nlink) {
            vnode_ref(&fv->vn);
            return fv;
        }
    }
    fv = (struct fat_vnode *)kzalloc(sizeof(*fv));
    if (!fv) return NULL;
    fv->fs = fs;
    fv->first = entry_cluster(fs, info->e);
    fv->dir_first = dir_first;
    fv->slot = info->slot;
    fv->nslots = info->nslots;
    fv->key = info->off;
    fv->vn.type = (info->e[11] & ATTR_DIR) ? VNODE_DIR : VNODE_FILE;
    fv->vn.mode = fv->vn.type == VNODE_DIR ? 0755 : 0755;  /* FAT-এ অনুমতি নেই; চালানো যায় এমন ধরা */
    fv->vn.size = fv->vn.type == VNODE_DIR ? 0 : rd32(info->e + 28);
    fv->vn.inode = info->off;
    fv->vn.mtime = fat_to_unix(rd16(info->e + 24), rd16(info->e + 22));
    fv->vn.nlink = 1;
    fv->vn.refcount = 1;
    fv->vn.ops = &fat_ops;
    fv->vn.fs_data = fs;
    if (fv->vn.type == VNODE_DIR) fv->vn.parent = parent;
    if (parent) fv->vn.mount = parent->mount;
    fv->next = fs->vnodes;
    fs->vnodes = fv;
    return fv;
}

static void drop_vnode(struct fat_vnode *fv)
{
    struct fat_vnode **pp;
    for (pp = &fv->fs->vnodes; *pp; pp = &(*pp)->next) {
        if (*pp == fv) {
            *pp = fv->next;
            break;
        }
    }
    kfree(fv);
}

/* vnode-এর তথ্য (প্রথম cluster, আকার, সময়) তার ডিরেক্টরি এন্ট্রিতে লেখা */
static int update_entry(struct fat_vnode *fv)
{
    struct fat_fs *fs = fv->fs;
    uint8_t e[32];
    uint16_t d, t;
    if (fv->is_root) return 0;
    if (block_read_bytes(fs->dev, fv->key, e, 32) < 0) return -EIO;
    wr16(e + 26, (uint16_t)fv->first);
    if (fs->type == 32) wr16(e + 20, (uint16_t)(fv->first >> 16));
    wr32(e + 28, fv->vn.type == VNODE_DIR ? 0 : (uint32_t)fv->vn.size);
    unix_to_fat((int64_t)fv->vn.mtime, &d, &t);
    wr16(e + 24, d);
    wr16(e + 22, t);
    wr16(e + 18, d);                                     /* শেষ ব্যবহারের তারিখ */
    e[11] |= (fv->vn.type == VNODE_FILE) ? ATTR_ARCH : 0;
    return block_write_bytes(fs->dev, fv->key, e, 32) < 0 ? -EIO : 0;
}

/* ---------- ফাইল পড়া/লেখা ---------- */

static int64_t fat_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct fat_vnode *fv = FV(vn);
    struct fat_fs *fs = fv->fs;
    uint8_t *out = (uint8_t *)buf;
    uint32_t c;
    size_t done = 0;

    if (vn->type != VNODE_FILE) return -EISDIR;
    if (off >= vn->size) return 0;
    if (len > vn->size - off) len = (size_t)(vn->size - off);
    c = chain_nth(fs, fv->first, (uint32_t)(off / fs->cluster_size));
    while (done < len && c >= 2) {
        uint32_t in = (uint32_t)(off % fs->cluster_size), n = fs->cluster_size - in, v;
        if (n > len - done) n = (uint32_t)(len - done);
        if (block_read_bytes(fs->dev, cluster_off(fs, c) + in, out + done, n) < 0) return -EIO;
        done += n;
        off += n;
        if (done < len) {
            if (fat_get(fs, c, &v) < 0 || is_eoc(fs, v) || v < 2) break;
            c = v;
        }
    }
    return (int64_t)done;
}

/* ফাইলের chain যেন অন্তত need বাইট ধরে (নতুন cluster যোগ) */
static int ensure_clusters(struct fat_vnode *fv, uint64_t need)
{
    struct fat_fs *fs = fv->fs;
    uint32_t want = (uint32_t)((need + fs->cluster_size - 1) / fs->cluster_size), have = 0, c = fv->first, last = 0, v;
    int err;
    if (want == 0) return 0;
    if (c == 0) {
        err = alloc_cluster(fs, 0, 0, &c);
        if (err < 0) return err;
        fv->first = c;
    }
    for (;;) {
        have++;
        last = c;
        if (fat_get(fs, c, &v) < 0) return -EIO;
        if (is_eoc(fs, v) || v < 2) break;
        c = v;
    }
    while (have < want) {
        err = alloc_cluster(fs, last, 0, &c);
        if (err < 0) return err;
        last = c;
        have++;
    }
    return 0;
}

static int write_bytes(struct fat_vnode *fv, uint64_t off, const uint8_t *src, size_t len)
{
    struct fat_fs *fs = fv->fs;
    uint32_t c = chain_nth(fs, fv->first, (uint32_t)(off / fs->cluster_size)), v;
    size_t done = 0;
    uint8_t *zero = NULL;
    while (done < len && c >= 2) {
        uint32_t in = (uint32_t)(off % fs->cluster_size), n = fs->cluster_size - in;
        if (n > len - done) n = (uint32_t)(len - done);
        if (!src) {
            if (!zero && !(zero = (uint8_t *)kzalloc(fs->cluster_size))) return -ENOMEM;
            if (block_write_bytes(fs->dev, cluster_off(fs, c) + in, zero, n) < 0) { kfree(zero); return -EIO; }
        } else if (block_write_bytes(fs->dev, cluster_off(fs, c) + in, src + done, n) < 0) {
            return -EIO;
        }
        done += n;
        off += n;
        if (done < len) {
            if (fat_get(fs, c, &v) < 0 || is_eoc(fs, v) || v < 2) break;
            c = v;
        }
    }
    kfree(zero);
    return done == len ? 0 : -EIO;
}

static int64_t fat_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct fat_vnode *fv = FV(vn);
    int err;
    if (vn->type != VNODE_FILE) return -EISDIR;
    if (block_is_read_only(fv->fs->dev)) return -EROFS;         /* ধাপ ১৫খ: সুরক্ষিত ডিস্ক */
    if (len == 0) return 0;
    if (off + len > FAT_MAX_FILE) return -EFBIG;
    err = ensure_clusters(fv, off + len);
    if (err < 0) return err;
    if (off > vn->size) {
        err = write_bytes(fv, vn->size, NULL, (size_t)(off - vn->size));   /* ফাঁকা অংশ শূন্য */
        if (err < 0) return err;
    }
    err = write_bytes(fv, off, (const uint8_t *)buf, len);
    if (err < 0) return err;
    if (off + len > vn->size) vn->size = off + len;
    vn->mtime = (uint64_t)clock_unix_time();
    err = update_entry(fv);
    return err < 0 ? err : (int64_t)len;
}

static int fat_truncate(struct vnode *vn, uint64_t size)
{
    struct fat_vnode *fv = FV(vn);
    struct fat_fs *fs = fv->fs;
    int err;
    if (vn->type != VNODE_FILE) return -EISDIR;
    if (block_is_read_only(fs->dev)) return -EROFS;             /* ধাপ ১৫খ */
    if (size > FAT_MAX_FILE) return -EFBIG;
    if (size < vn->size) {
        uint32_t keep = (uint32_t)((size + fs->cluster_size - 1) / fs->cluster_size);
        if (keep == 0) {
            free_chain(fs, fv->first);
            fv->first = 0;
        } else {
            uint32_t last = chain_nth(fs, fv->first, keep - 1), v;
            if (last && fat_get(fs, last, &v) == 0 && !is_eoc(fs, v) && v >= 2) {
                fat_set(fs, last, eoc_value(fs));
                free_chain(fs, v);
            }
        }
        vn->size = size;
    } else if (size > vn->size) {
        err = ensure_clusters(fv, size);
        if (err < 0) return err;
        err = write_bytes(fv, vn->size, NULL, (size_t)(size - vn->size));
        if (err < 0) return err;
        vn->size = size;
    }
    vn->mtime = (uint64_t)clock_unix_time();
    return update_entry(fv);
}

/* ---------- ডিরেক্টরি ---------- */

static uint32_t dir_first_of(struct fat_vnode *dir)
{
    if (dir->is_root) return dir->fs->type == 32 ? dir->fs->root_cluster : 0;
    return dir->first;
}

struct find_ctx {
    const char *name;
    struct dir_entry_info found;
    uint8_t e[32];
    int ok;
};

static int find_visit(const struct dir_entry_info *info, void *ctx)
{
    struct find_ctx *f = (struct find_ctx *)ctx;
    if (!name_eq(info->name, f->name)) return 0;
    f->found = *info;
    memcpy(f->e, info->e, 32);
    f->found.e = f->e;
    f->found.name = NULL;
    f->ok = 1;
    return 1;
}

static int fat_lookup(struct vnode *dirvn, const char *name, struct vnode **out)
{
    struct fat_vnode *dir = FV(dirvn), *child;
    struct find_ctx f;
    int r;
    if (dirvn->type != VNODE_DIR) return -ENOTDIR;
    f.name = name;
    f.ok = 0;
    r = walk_dir(dir->fs, dir_first_of(dir), find_visit, &f);
    if (r < 0) return r;
    if (!f.ok) return -ENOENT;
    child = get_vnode(dir->fs, &f.found, dir_first_of(dir), dirvn);
    if (!child) return -ENOMEM;
    *out = &child->vn;
    return 0;
}

struct readdir_ctx {
    uint64_t want, seen;
    struct vfs_dirent *out;
    struct fat_fs *fs;
};

static int readdir_visit(const struct dir_entry_info *info, void *ctx)
{
    struct readdir_ctx *r = (struct readdir_ctx *)ctx;
    size_t n;
    if (r->seen++ != r->want) return 0;
    n = strlen(info->name);
    if (n > VFS_NAME_MAX) n = VFS_NAME_MAX;
    memcpy(r->out->name, info->name, n);
    r->out->name[n] = '\0';
    r->out->type = (info->e[11] & ATTR_DIR) ? VNODE_DIR : VNODE_FILE;
    r->out->inode = info->off;
    r->out->size = r->out->type == VNODE_DIR ? 0 : rd32(info->e + 28);
    return 1;
}

static int fat_readdir(struct vnode *dirvn, uint64_t index, struct vfs_dirent *out)
{
    struct fat_vnode *dir = FV(dirvn);
    struct readdir_ctx r;
    r.want = index;
    r.seen = 0;
    r.out = out;
    r.fs = dir->fs;
    return walk_dir(dir->fs, dir_first_of(dir), readdir_visit, &r);
}

/* ---- নতুন এন্ট্রির নাম: সম্ভব হলে শুধু 8.3, নইলে LFN + "BASIS~N.EXT" ---- */

static int short_char_ok(char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c && strchr("!#$%&'()-@^_`{}~", c) != NULL;
}

/* name সরাসরি 8.3 হতে পারে কিনা; পারলে sn আর ntres ভরে 1 */
static int try_plain_83(const char *name, uint8_t sn[11], uint8_t *ntres)
{
    const char *dot = strchr(name, '.');
    size_t bl = dot ? (size_t)(dot - name) : strlen(name), el = dot ? strlen(dot + 1) : 0, i;
    int lower_b = 0, upper_b = 0, lower_e = 0, upper_e = 0;
    if (bl == 0 || bl > 8 || el > 3 || (dot && strchr(dot + 1, '.')) || (dot && el == 0)) return 0;
    memset(sn, ' ', 11);
    for (i = 0; i < bl; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') { lower_b = 1; c = (char)(c - 32); } else if (c >= 'A' && c <= 'Z') upper_b = 1;
        if (!short_char_ok(c)) return 0;
        sn[i] = (uint8_t)c;
    }
    for (i = 0; i < el; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') { lower_e = 1; c = (char)(c - 32); } else if (c >= 'A' && c <= 'Z') upper_e = 1;
        if (!short_char_ok(c)) return 0;
        sn[8 + i] = (uint8_t)c;
    }
    if ((lower_b && upper_b) || (lower_e && upper_e)) return 0;   /* মেশানো case → LFN লাগবে */
    *ntres = (uint8_t)((lower_b ? NTRES_LOWER_BASE : 0) | (lower_e ? NTRES_LOWER_EXT : 0));
    if (sn[0] == 0xE5) sn[0] = 0x05;
    return 1;
}

struct short_ctx {
    const uint8_t *sn;
    int exists;
};

static int short_visit(const struct dir_entry_info *info, void *ctx)
{
    struct short_ctx *s = (struct short_ctx *)ctx;
    if (!memcmp(info->e, s->sn, 11)) {
        s->exists = 1;
        return 1;
    }
    return 0;
}

static const char *last_dot(const char *s)
{
    const char *d = NULL;
    for (; *s; s++) if (*s == '.') d = s;
    return d;
}

static int make_alias(struct fat_fs *fs, uint32_t dir_first, const char *name, uint8_t sn[11])
{
    const char *dot = last_dot(name);
    uint8_t basis[8];
    size_t bl = 0, i;
    uint32_t n;
    if (dot == name) dot = NULL;                           /* ".profile" ধরনের */
    memset(sn, ' ', 11);
    for (i = 0; name[i] && (!dot || name + i < dot) && bl < 8; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!short_char_ok(c)) c = '_';
        basis[bl++] = (uint8_t)c;
    }
    if (bl == 0) basis[bl++] = '_';
    if (dot) {
        size_t el = 0;
        for (i = 1; dot[i] && el < 3; i++) {
            char c = dot[i];
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (!short_char_ok(c)) c = '_';
            sn[8 + el++] = (uint8_t)c;
        }
    }
    for (n = 1; n < 1000000; n++) {
        char num[8];
        size_t nl = 0, keep, k;
        uint32_t t = n;
        struct short_ctx s;
        char rev[8];
        size_t rl = 0;
        while (t) { rev[rl++] = (char)('0' + t % 10); t /= 10; }
        num[nl++] = '~';
        while (rl) num[nl++] = rev[--rl];
        keep = bl < 8 - nl ? bl : 8 - nl;
        for (k = 0; k < 8; k++) sn[k] = ' ';
        memcpy(sn, basis, keep);
        memcpy(sn + keep, num, nl);
        s.sn = sn;
        s.exists = 0;
        if (walk_dir(fs, dir_first, short_visit, &s) < 0) return -EIO;
        if (!s.exists) return 0;
    }
    return -EEXIST;
}

static int lfn_name_ok(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > 255 || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (; *name; name++) {
        if ((unsigned char)*name < 0x20 || strchr("\\/:*?\"<>|", *name)) return 0;
    }
    return 1;
}

/* dir-এ n টা পরপর খালি এন্ট্রি; দরকার হলে ডিরেক্টরি বাড়ানো */
static int find_free_slots(struct fat_vnode *dir, uint32_t n, uint32_t *start)
{
    struct fat_fs *fs = dir->fs;
    uint32_t first = dir_first_of(dir), idx, run = 0, run_start = 0, per = fs->cluster_size / 32;
    uint8_t b;
    for (idx = 0;; idx++) {
        uint64_t off;
        int r = dir_slot(fs, first, idx, &off);
        if (r < 0) return r;
        if (r > 0) {
            /* শেষ: নতুন cluster (শূন্য — সব এন্ট্রি খালি) */
            uint32_t last, c;
            int err;
            if (first == 0) return -ENOSPC;                  /* FAT16 root স্থির */
            last = chain_nth(fs, first, idx / per - 1);
            if (!last) return -EIO;
            err = alloc_cluster(fs, last, 1, &c);
            if (err < 0) return err;
            fs->cache_first = 0;
            if (run == 0) run_start = idx;
            run += per;
            if (run >= n) { *start = run_start; return 0; }
            idx += per - 1;
            continue;
        }
        if (block_read_bytes(fs->dev, off, &b, 1) < 0) return -EIO;
        if (b == 0x00 || b == 0xE5) {
            if (run == 0) run_start = idx;
            if (++run >= n) { *start = run_start; return 0; }
        } else {
            run = 0;
        }
    }
}

static int fat_create(struct vnode *dirvn, const char *name, enum vnode_type type, struct vnode **out)
{
    struct fat_vnode *dir = FV(dirvn), *child;
    struct fat_fs *fs = dir->fs;
    struct find_ctx f;
    uint8_t sn[11], ntres = 0, e[32];
    uint16_t u16[260], d, t;
    int ulen = 0, need_lfn, err, k;
    uint32_t nlfn = 0, start = 0, dfirst = dir_first_of(dir), newc = 0, i;
    struct dir_entry_info info;
    if (block_is_read_only(FV(dirvn)->fs->dev)) return -EROFS;         /* ধাপ ১৫খ: সুরক্ষিত ডিস্ক */

    if (dirvn->type != VNODE_DIR) return -ENOTDIR;
    if (type != VNODE_FILE && type != VNODE_DIR) return -EINVAL;
    if (!lfn_name_ok(name)) return -EINVAL;
    f.name = name;
    f.ok = 0;
    if (walk_dir(fs, dfirst, find_visit, &f) < 0) return -EIO;
    if (f.ok) return -EEXIST;

    need_lfn = !try_plain_83(name, sn, &ntres);
    if (need_lfn) {
        ulen = utf8_to_utf16(name, u16, 255);
        if (ulen <= 0) return -EINVAL;
        nlfn = (uint32_t)(ulen + 12) / 13;
        ntres = 0;
        err = make_alias(fs, dfirst, name, sn);
        if (err < 0) return err;
    }
    err = find_free_slots(dir, nlfn + 1, &start);
    if (err < 0) return err;

    if (type == VNODE_DIR) {
        uint8_t dots[64];
        err = alloc_cluster(fs, 0, 1, &newc);
        if (err < 0) return err;
        memset(dots, 0, sizeof(dots));
        memset(dots, ' ', 11);
        dots[0] = '.';
        dots[11] = ATTR_DIR;
        wr16(dots + 26, (uint16_t)newc);
        if (fs->type == 32) wr16(dots + 20, (uint16_t)(newc >> 16));
        memset(dots + 32, ' ', 11);
        dots[32] = '.';
        dots[33] = '.';
        dots[43] = ATTR_DIR;
        {
            uint32_t pc = dir->is_root ? 0 : dir->first;   /* root-এর দিকে ".." = 0 (spec) */
            wr16(dots + 32 + 26, (uint16_t)pc);
            if (fs->type == 32) wr16(dots + 32 + 20, (uint16_t)(pc >> 16));
        }
        unix_to_fat(clock_unix_time(), &d, &t);
        wr16(dots + 24, d); wr16(dots + 22, t); wr16(dots + 16, d); wr16(dots + 14, t);
        wr16(dots + 32 + 24, d); wr16(dots + 32 + 22, t); wr16(dots + 32 + 16, d); wr16(dots + 32 + 14, t);
        if (block_write_bytes(fs->dev, cluster_off(fs, newc), dots, 64) < 0) {
            free_chain(fs, newc);
            return -EIO;
        }
    }

    /* LFN এন্ট্রি: সবচেয়ে শেষের অংশ প্রথমে (0x40 চিহ্ন) */
    {
        uint8_t sum = lfn_checksum(sn);
        static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (i = 0; i < nlfn; i++) {
            uint32_t seq = nlfn - i;                        /* এই slot-এর ক্রম */
            uint64_t off;
            memset(e, 0, 32);
            e[0] = (uint8_t)(seq | (i == 0 ? 0x40 : 0));
            e[11] = ATTR_LFN;
            e[13] = sum;
            for (k = 0; k < 13; k++) {
                int ci = (int)(seq - 1) * 13 + k;
                uint16_t ch = ci < ulen ? u16[ci] : (ci == ulen ? 0x0000 : 0xFFFF);
                wr16(e + pos[k], ch);
            }
            if (dir_slot(fs, dfirst, start + i, &off) != 0 || block_write_bytes(fs->dev, off, e, 32) < 0) {
                if (newc) free_chain(fs, newc);
                return -EIO;
            }
        }
    }
    /* আসল 8.3 এন্ট্রি */
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    e[11] = type == VNODE_DIR ? ATTR_DIR : ATTR_ARCH;
    e[12] = ntres;
    unix_to_fat(clock_unix_time(), &d, &t);
    wr16(e + 14, t); wr16(e + 16, d); wr16(e + 18, d); wr16(e + 22, t); wr16(e + 24, d);
    wr16(e + 26, (uint16_t)newc);
    if (fs->type == 32) wr16(e + 20, (uint16_t)(newc >> 16));
    if (dir_slot(fs, dfirst, start + nlfn, &info.off) != 0 || block_write_bytes(fs->dev, info.off, e, 32) < 0) {
        if (newc) free_chain(fs, newc);
        return -EIO;
    }
    info.e = e;
    info.slot = start + nlfn;
    info.nslots = nlfn + 1;
    info.name = name;
    child = get_vnode(fs, &info, dfirst, dirvn);
    if (!child) return -ENOMEM;
    dirvn->mtime = (uint64_t)clock_unix_time();
    *out = &child->vn;
    return 0;
}

static int fat_unlink(struct vnode *dirvn, const char *name)
{
    struct fat_vnode *dir = FV(dirvn), *fv;
    struct fat_fs *fs = dir->fs;
    struct find_ctx f;
    uint32_t i, dfirst = dir_first_of(dir), first;
    uint8_t del = 0xE5;
    if (block_is_read_only(FV(dirvn)->fs->dev)) return -EROFS;         /* ধাপ ১৫খ: সুরক্ষিত ডিস্ক */

    f.name = name;
    f.ok = 0;
    if (walk_dir(fs, dfirst, find_visit, &f) < 0) return -EIO;
    if (!f.ok) return -ENOENT;
    for (i = 0; i < f.found.nslots; i++) {
        uint64_t off;
        if (dir_slot(fs, dfirst, f.found.slot - (f.found.nslots - 1) + i, &off) != 0 ||
            block_write_bytes(fs->dev, off, &del, 1) < 0) {
            return -EIO;
        }
    }
    first = entry_cluster(fs, f.e);
    for (fv = fs->vnodes; fv; fv = fv->next) {
        if (!fv->is_root && fv->key == f.found.off && fv->vn.nlink) break;
    }
    if (fv) {
        fv->vn.nlink = 0;
        if (fv->vn.refcount == 0) {
            free_chain(fs, fv->first);
            drop_vnode(fv);
        }
        /* খোলা থাকলে: শেষ ref চলে গেলে fat_release cluster ফেরত দেয় */
    } else if (first >= 2) {
        free_chain(fs, first);
    }
    dirvn->mtime = (uint64_t)clock_unix_time();
    return 0;
}

/* ধাপ ১৫ক: নাম বদল / সরানো — নতুন নামে এন্ট্রি বানিয়ে (LFN সহ) তাতে পুরোনো এন্ট্রির
 * cluster, আকার, সময় বসানো; তারপর পুরোনো এন্ট্রি মোছা (ডেটা নড়ে না) */
static int fat_rename(struct vnode *odvn, const char *oname, struct vnode *ndvn, const char *nname)
{
    struct fat_vnode *od = FV(odvn), *nd = FV(ndvn), *child, *fv;
    struct fat_fs *fs = od->fs;
    struct find_ctx f, g;
    struct vnode *cvn;
    uint32_t odfirst = dir_first_of(od), ndfirst = dir_first_of(nd), i, oldfirst;
    uint8_t ne[32], del = 0xE5;
    int is_dir, err;
    if (block_is_read_only(FV(odvn)->fs->dev)) return -EROFS;         /* ধাপ ১৫খ: সুরক্ষিত ডিস্ক */

    if (nd->fs != fs) return -EXDEV;
    f.name = oname;
    f.ok = 0;
    if (walk_dir(fs, odfirst, find_visit, &f) < 0) return -EIO;
    if (!f.ok) return -ENOENT;
    is_dir = (f.e[11] & ATTR_DIR) != 0;
    oldfirst = entry_cluster(fs, f.e);
    g.name = nname;
    g.ok = 0;
    if (walk_dir(fs, ndfirst, find_visit, &g) < 0) return -EIO;
    if (g.ok) {
        if (g.found.off == f.found.off) {
            if (!strcmp(oname, nname)) return 0;
            /* শুধু বড়/ছোট হাতের অক্ষর বদল (FAT-এ একই নাম): নিচে নতুন করে লেখা হবে */
        } else {
            if ((g.e[11] & ATTR_DIR) || is_dir) return -EEXIST;
            err = fat_unlink(ndvn, nname);
            if (err < 0) return err;
        }
    }
    /* পুরোনো এন্ট্রি আগে মোছা (একই নাম কেস-বদলের জন্য জায়গা খালি হয়) */
    for (i = 0; i < f.found.nslots; i++) {
        uint64_t off;
        if (dir_slot(fs, odfirst, f.found.slot - (f.found.nslots - 1) + i, &off) != 0 ||
            block_write_bytes(fs->dev, off, &del, 1) < 0) {
            return -EIO;
        }
    }
    err = fat_create(ndvn, nname, is_dir ? VNODE_DIR : VNODE_FILE, &cvn);
    if (err < 0) {
        /* ফেরত: পুরোনো এন্ট্রি আবার চালু (প্রথম বাইট) */
        for (i = 0; i < f.found.nslots; i++) {
            uint64_t off;
            uint8_t b0;
            if (dir_slot(fs, odfirst, f.found.slot - (f.found.nslots - 1) + i, &off) != 0) continue;
            if (i + 1 == f.found.nslots) b0 = f.e[0];
            else b0 = (uint8_t)(((f.found.nslots - 1 - i) & 0x1F) | (i == 0 ? 0x40 : 0));
            block_write_bytes(fs->dev, off, &b0, 1);
        }
        return err;
    }
    child = FV(cvn);
    if (is_dir && child->first >= 2) free_chain(fs, child->first);   /* নতুন খালি ক্লাস্টার লাগবে না */
    /* নতুন 8.3 এন্ট্রিতে পুরোনো তথ্য (নামের ১১ বাইট বাদে) */
    if (block_read_bytes(fs->dev, child->key, ne, 32) < 0) return -EIO;
    memcpy(ne + 11, f.e + 11, 21);
    if (block_write_bytes(fs->dev, child->key, ne, 32) < 0) return -EIO;
    /* ডিরেক্টরি অন্য parent-এ গেলে তার ".." */
    if (is_dir && oldfirst >= 2 && od != nd) {
        uint8_t dd[32];
        uint32_t pc = nd->is_root ? 0 : nd->first;
        if (block_read_bytes(fs->dev, cluster_off(fs, oldfirst) + 32, dd, 32) == 0 && dd[0] == '.' && dd[1] == '.') {
            wr16(dd + 26, (uint16_t)pc);
            if (fs->type == 32) wr16(dd + 20, (uint16_t)(pc >> 16));
            block_write_bytes(fs->dev, cluster_off(fs, oldfirst) + 32, dd, 32);
        }
    }
    /* cache: পুরোনো vnode খোলা থাকলে নতুন জায়গায় সরানো; নতুনটা বাদ */
    for (fv = fs->vnodes; fv; fv = fv->next) {
        if (fv != child && !fv->is_root && fv->key == f.found.off && fv->vn.nlink) break;
    }
    if (fv) {
        fv->key = child->key;
        fv->slot = child->slot;
        fv->nslots = child->nslots;
        fv->dir_first = child->dir_first;
        fv->vn.inode = child->key;
        if (fv->vn.type == VNODE_DIR) fv->vn.parent = ndvn;
        child->vn.nlink = 1;
        drop_vnode(child);                       /* ref ছিল শুধু আমাদের — ক্লাস্টার ছোঁয়া নয় */
    } else {
        child->first = oldfirst;
        child->vn.size = is_dir ? 0 : rd32(f.e + 28);
        child->vn.mtime = fat_to_unix(rd16(f.e + 24), rd16(f.e + 22));
        vnode_unref(cvn);
    }
    odvn->mtime = ndvn->mtime = (uint64_t)clock_unix_time();
    return 0;
}

static void fat_release(struct vnode *vn)
{
    struct fat_vnode *fv = FV(vn);
    if (fv->vn.nlink == 0 && !fv->is_root) {
        free_chain(fv->fs, fv->first);
        drop_vnode(fv);
    }
}

static const struct vnode_ops fat_ops = {
    fat_read, fat_write, fat_truncate, fat_lookup, fat_create,
    fat_unlink, fat_readdir, fat_release, NULL,
    fat_rename                                           /* ধাপ ১৫ক */, NULL /* setflags (ধাপ ১৯ছ) */
};

/* ---------- mount / statfs / info ---------- */

static void copy_trim(char *dst, const uint8_t *src, size_t n)
{
    size_t i;
    memcpy(dst, src, n);
    dst[n] = '\0';
    for (i = n; i > 0 && dst[i - 1] == ' '; i--) dst[i - 1] = '\0';
}

static int fat_mount(const void *arg, struct vnode **root)
{
    struct block_device *dev = block_find((const char *)arg);
    struct fat_fs *fs;
    struct fat_vnode *r;
    uint8_t bs[512];
    uint32_t data_sectors, c, total16, fat16, i;
    uint8_t *buf;

    if (!dev) return -ENODEV;
    if (block_read_bytes(dev, 0, bs, sizeof(bs)) < 0) return -EIO;
    if (bs[510] != 0x55 || bs[511] != 0xAA) return -EINVAL;
    fs = (struct fat_fs *)kzalloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    fs->dev = dev;
    fs->bps = rd16(bs + 11);
    fs->spc = bs[13];
    fs->reserved = rd16(bs + 14);
    fs->nfats = bs[16];
    fs->root_entries = rd16(bs + 17);
    total16 = rd16(bs + 19);
    fat16 = rd16(bs + 22);
    fs->total_sectors = total16 ? total16 : rd32(bs + 32);
    fs->fat_sectors = fat16 ? fat16 : rd32(bs + 36);
    if ((fs->bps != 512 && fs->bps != 1024 && fs->bps != 2048 && fs->bps != 4096) ||
        fs->spc == 0 || (fs->spc & (fs->spc - 1)) || fs->reserved == 0 || fs->nfats == 0 ||
        fs->fat_sectors == 0 || fs->total_sectors == 0 ||
        (uint64_t)fs->total_sectors * fs->bps > dev->sector_count * dev->sector_size) {
        kfree(fs);
        return -EINVAL;
    }
    fs->cluster_size = fs->bps * fs->spc;
    fs->root_sectors = (fs->root_entries * 32 + fs->bps - 1) / fs->bps;
    fs->first_data_sector = fs->reserved + fs->nfats * fs->fat_sectors + fs->root_sectors;
    if (fs->first_data_sector >= fs->total_sectors) {
        kfree(fs);
        return -EINVAL;
    }
    data_sectors = fs->total_sectors - fs->first_data_sector;
    fs->clusters = data_sectors / fs->spc;
    if (fs->clusters < 4085) {                       /* FAT12 */
        kfree(fs);
        return -EINVAL;
    }
    fs->type = fs->clusters < 65525 ? 16 : 32;
    copy_trim(fs->oem, bs + 3, 8);
    if (fs->type == 32) {
        fs->root_cluster = rd32(bs + 44);
        fs->fsinfo_sector = rd16(bs + 48);
        if (bs[66] == 0x29) { fs->serial = rd32(bs + 67); copy_trim(fs->label, bs + 71, 11); }
        if (fs->root_cluster < 2 || fs->root_cluster >= fs->clusters + 2) {
            kfree(fs);
            return -EINVAL;
        }
    } else {
        if (bs[38] == 0x29) { fs->serial = rd32(bs + 39); copy_trim(fs->label, bs + 43, 11); }
    }
    if (!strcmp(fs->label, "NO NAME")) fs->label[0] = '\0';

    /* খালি cluster গোনা — FAT একবার পুরো পড়ে (FSInfo-র সংখ্যা ভুল হতে পারে) */
    buf = (uint8_t *)kmalloc(4096);
    if (!buf) { kfree(fs); return -ENOMEM; }
    fs->free_count = 0;
    fs->next_free = 0;
    {
        uint32_t esize = fs->type == 32 ? 4 : 2, per = 4096 / esize, last = fs->clusters + 2;
        for (c = 0; c < last; c += per) {
            uint32_t n = last - c < per ? last - c : per;
            if (block_read_bytes(dev, (uint64_t)fs->reserved * fs->bps + (uint64_t)c * esize, buf, n * esize) < 0) {
                kfree(buf);
                kfree(fs);
                return -EIO;
            }
            for (i = 0; i < n; i++) {
                uint32_t v = esize == 4 ? (rd32(buf + i * 4) & 0x0FFFFFFFu) : rd16(buf + i * 2);
                if (c + i >= 2 && v == 0) {
                    fs->free_count++;
                    if (!fs->next_free) fs->next_free = c + i;
                }
            }
        }
    }
    kfree(buf);

    r = (struct fat_vnode *)kzalloc(sizeof(*r));
    if (!r) { kfree(fs); return -ENOMEM; }
    r->fs = fs;
    r->is_root = 1;
    r->first = fs->type == 32 ? fs->root_cluster : 0;
    r->vn.type = VNODE_DIR;
    r->vn.mode = 0755;
    r->vn.inode = 1;
    r->vn.nlink = 1;
    r->vn.refcount = 1;
    r->vn.ops = &fat_ops;
    r->vn.fs_data = fs;
    *root = &r->vn;
    return 0;
}

static int fat_statfs(struct vnode *root, struct vfs_statfs *out)
{
    struct fat_fs *fs = FV(root)->fs;
    size_t n = strlen(fs->dev->name);
    out->block_size = fs->cluster_size;
    out->total_blocks = fs->clusters;
    out->free_blocks = fs->free_count;
    out->total_inodes = 0;
    out->free_inodes = 0;
    out->read_only = block_is_read_only(fs->dev);      /* ধাপ ১৫খ */
    if (n >= sizeof(out->source)) n = sizeof(out->source) - 1;
    memcpy(out->source, fs->dev->name, n);
    out->source[n] = '\0';
    return 0;
}

static void fat_info(struct vnode *root)
{
    struct fat_fs *fs = FV(root)->fs;
    kprintf("      type            FAT%d\n", fs->type);
    kprintf("      label           \"%s\"   serial %04X-%04X   OEM \"%s\"\n", fs->label,
            (unsigned)(fs->serial >> 16), (unsigned)(fs->serial & 0xFFFF), fs->oem);
    kprintf("      sector size     %u bytes, %u sectors (%u MiB)\n", (unsigned)fs->bps,
            (unsigned)fs->total_sectors, (unsigned)((uint64_t)fs->total_sectors * fs->bps / (1024 * 1024)));
    kprintf("      cluster size    %u bytes (%u sectors)\n", (unsigned)fs->cluster_size, (unsigned)fs->spc);
    kprintf("      clusters        %u total, %u free (%u KiB free)\n", (unsigned)fs->clusters,
            (unsigned)fs->free_count, (unsigned)((uint64_t)fs->free_count * fs->cluster_size / 1024));
    kprintf("      layout          %u reserved sectors, %u FATs x %u sectors, data from sector %u\n",
            (unsigned)fs->reserved, (unsigned)fs->nfats, (unsigned)fs->fat_sectors, (unsigned)fs->first_data_sector);
    if (fs->type == 32) {
        kprintf("      root directory  cluster %u, FSInfo at sector %u\n", (unsigned)fs->root_cluster,
                (unsigned)fs->fsinfo_sector);
    } else {
        kprintf("      root directory  fixed, %u entries (%u sectors)\n", (unsigned)fs->root_entries,
                (unsigned)fs->root_sectors);
    }
}

/* ধাপ ১৫ক: umount-এর আগে — খোলা ফাইল থাকলে ব্যস্ত */
static int fat_umount(struct vnode *root)
{
    struct fat_vnode *fv;
    for (fv = FV(root)->fs->vnodes; fv; fv = fv->next) {
        if (&fv->vn != root && fv->vn.refcount > 0) return -EBUSY;
    }
    return 0;
}

const struct filesystem fat_filesystem = { "fat", fat_mount, fat_statfs, fat_info, fat_umount };

/* ---------- kmon-এর জন্য: কোনো ডিভাইসে কী ফাইলসিস্টেম (mount না করেই) ---------- */

const char *fs_probe_name(struct block_device *dev)
{
    uint8_t b[512];
    uint16_t magic;
    if (dev->sector_count * dev->sector_size >= 2048 &&
        block_read_bytes(dev, 1024 + 56, &magic, 2) == 0 && magic == 0xEF53) {
        return "ext2";
    }
    if (block_read_bytes(dev, 0, b, sizeof(b)) < 0) return "?";
    if (b[510] == 0x55 && b[511] == 0xAA) {
        if (!memcmp(b + 82, "FAT32", 5)) return "fat32";
        if (!memcmp(b + 54, "FAT16", 5)) return "fat16";
        if (!memcmp(b + 54, "FAT12", 5)) return "fat12";
        if (!memcmp(b + 3, "NTFS", 4)) return "ntfs";
        if (!memcmp(b + 3, "EXFAT", 5)) return "exfat";
    }
    return "-";
}
