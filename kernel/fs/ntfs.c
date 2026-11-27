/* rozOS — NTFS, শুধু পড়া (ধাপ ২৪গ)
 *
 * Windows-এর ডিস্ক আর বাইরের হার্ডডিস্ক (যেমন WD My Passport) দেখা আর ফাইল কপি করার জন্য।
 * লেখা কখনো নয় — NTFS-এ ভুল লেখা পুরো ডিস্ক নষ্ট করতে পারে, তাই সব লেখার অপারেশন -EROFS।
 *
 * বিন্যাস (সংক্ষেপে):
 *   boot sector: bytes/sector, sectors/cluster, $MFT-এর LCN, MFT record আর index record-এর মাপ
 *   MFT record ("FILE"): update sequence (fixup) → attribute-এর সারি (type, length, resident কিনা)
 *     0x10 $STANDARD_INFORMATION, 0x20 $ATTRIBUTE_LIST, 0x30 $FILE_NAME, 0x80 $DATA,
 *     0x90 $INDEX_ROOT ("$I30"), 0xA0 $INDEX_ALLOCATION ("$I30")
 *   non-resident attribute: runlist (দৈর্ঘ্য + আপেক্ষিক LCN; offset-এর বাইট ০ = sparse)
 *   ডিরেক্টরি: index entry-গুলোর key = $FILE_NAME (UTF-16 নাম, parent, মাপ, সময়)
 * যা নেই: compressed আর encrypted ফাইল পড়া (-EIO / -EACCES), লেখা, journal। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/block.h>
#include <rozos/errno.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/printf.h>

#define AT_ATTRIBUTE_LIST   0x20
#define AT_FILE_NAME        0x30
#define AT_DATA             0x80
#define AT_INDEX_ROOT       0x90
#define AT_INDEX_ALLOCATION 0xA0
#define AT_END              0xFFFFFFFFu
#define MAX_EXTENTS         4096

struct ntfs_extent {
    uint64_t vcn, lcn, len;                 /* lcn = ~0 মানে sparse (শূন্য) */
};

struct ntfs_runs {
    struct ntfs_extent *ext;
    uint32_t n;
    uint64_t size;                          /* data size (বাইট) */
    uint64_t init_size;                     /* এর পরের অংশ শূন্য */
    int      resident;
    uint8_t *res_data;                      /* resident হলে মান (কপি) */
    uint16_t flags;
};

struct ntfs_vnode;

struct ntfs_fs {
    struct block_device *dev;
    uint32_t bps, cluster_size, rec_size, idx_size;
    uint64_t total_clusters, mft_lcn;
    struct ntfs_runs mft;                   /* $MFT-এর নিজের $DATA */
    uint64_t free_clusters;
    int      free_known;
    char     label[64];
    struct ntfs_vnode *vnodes;
};

struct ntfs_vnode {
    struct vnode vn;                        /* প্রথম সদস্য */
    struct ntfs_fs *fs;
    uint64_t mftno;
    struct ntfs_runs data;                  /* $DATA (প্রথম পড়ার সময় ভরা) */
    int      data_loaded;
    struct ntfs_vnode *next;
};

static const struct vnode_ops ntfs_ops;
static inline struct ntfs_vnode *NV(struct vnode *vn) { return (struct ntfs_vnode *)vn; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* NTFS সময় (১৬০১ থেকে ১০০ ns) → UNIX সেকেন্ড */
static uint64_t ntfs_time(uint64_t t)
{
    uint64_t s = t / 10000000ull;
    return s > 11644473600ull ? s - 11644473600ull : 0;
}

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

static void name_to_utf8(const uint8_t *u16, int n, char *out, size_t size)
{
    size_t pos = 0;
    int i;
    for (i = 0; i < n; i++) pos = put_utf8(out, pos, size, rd16(u16 + 2 * i));
    out[pos] = '\0';
}

static int name_eq(const char *a, const char *b)          /* ASCII case বাদ দিয়ে */
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* "FILE"/"INDX" record-এর fixup: প্রতি sector-এর শেষ ২ বাইট আসল মানে ফেরানো */
static int fixup(uint8_t *rec, uint32_t size, uint32_t bps)
{
    uint16_t ofs = rd16(rec + 4), cnt = rd16(rec + 6), usn;
    uint32_t i;
    if (ofs + 2u * cnt > size || cnt == 0) return -EIO;
    usn = rd16(rec + ofs);
    for (i = 1; i < cnt && i * bps <= size; i++) {
        uint8_t *end = rec + i * bps - 2;
        if (rd16(end) != usn) return -EIO;
        end[0] = rec[ofs + 2 * i];
        end[1] = rec[ofs + 2 * i + 1];
    }
    return 0;
}

/* runs-এ vcn → বাইট offset ডিস্কে (sparse হলে ~0) */
static uint64_t runs_map(const struct ntfs_runs *r, uint64_t vcn, uint64_t *left)
{
    uint32_t i;
    for (i = 0; i < r->n; i++) {
        const struct ntfs_extent *e = &r->ext[i];
        if (vcn >= e->vcn && vcn < e->vcn + e->len) {
            *left = e->vcn + e->len - vcn;
            return e->lcn == ~0ull ? ~0ull : e->lcn + (vcn - e->vcn);
        }
    }
    *left = 0;
    return ~0ull - 1;                                    /* ম্যাপ নেই */
}

/* runlist ডিকোড করে r-এ যোগ */
static int decode_runs(struct ntfs_runs *r, const uint8_t *p, const uint8_t *end, uint64_t vcn)
{
    int64_t lcn = 0;
    while (p < end && *p) {
        int ln = *p & 0x0F, on = *p >> 4, i;
        uint64_t len = 0;
        int64_t off = 0;
        p++;
        if (ln == 0 || ln > 8 || on > 8 || p + ln + on > end) return -EIO;
        for (i = 0; i < ln; i++) len |= (uint64_t)p[i] << (8 * i);
        p += ln;
        for (i = 0; i < on; i++) off |= (int64_t)((uint64_t)p[i] << (8 * i));
        if (on && (p[on - 1] & 0x80)) off |= -((int64_t)1 << (8 * on));     /* sign extend */
        p += on;
        if (r->n >= MAX_EXTENTS) return -EFBIG;
        if (r->n % 64 == 0) {
            struct ntfs_extent *ne = (struct ntfs_extent *)kmalloc(sizeof(*ne) * (r->n + 64));
            if (!ne) return -ENOMEM;
            if (r->ext) { memcpy(ne, r->ext, sizeof(*ne) * r->n); kfree(r->ext); }
            r->ext = ne;
        }
        r->ext[r->n].vcn = vcn;
        r->ext[r->n].len = len;
        if (on) { lcn += off; r->ext[r->n].lcn = (uint64_t)lcn; }
        else r->ext[r->n].lcn = ~0ull;
        r->n++;
        vcn += len;
    }
    return 0;
}

static void runs_free(struct ntfs_runs *r)
{
    kfree(r->ext);
    kfree(r->res_data);
    memset(r, 0, sizeof(*r));
}

/* MFT record পড়া (fixup সহ); buf = rec_size */
static int read_record(struct ntfs_fs *fs, uint64_t no, uint8_t *buf)
{
    uint64_t byte = no * fs->rec_size, done = 0;
    if (!fs->mft.n) {                                    /* বুটের সময়: $MFT-এর শুরু থেকে সরাসরি */
        if (block_read_bytes(fs->dev, fs->mft_lcn * fs->cluster_size + byte, buf, fs->rec_size) < 0) return -EIO;
    } else {
        while (done < fs->rec_size) {
            uint64_t vcn = (byte + done) / fs->cluster_size, in = (byte + done) % fs->cluster_size, left;
            uint64_t lcn = runs_map(&fs->mft, vcn, &left), n = left * fs->cluster_size - in;
            if (lcn >= ~0ull - 1) return -EIO;
            if (n > fs->rec_size - done) n = fs->rec_size - done;
            if (block_read_bytes(fs->dev, lcn * fs->cluster_size + in, buf + done, (size_t)n) < 0) return -EIO;
            done += n;
        }
    }
    if (memcmp(buf, "FILE", 4)) return -EIO;
    return fixup(buf, fs->rec_size, fs->bps);
}

/* attribute-এর নাম name কিনা (NULL = নামহীন) */
static int attr_name_is(const uint8_t *a, const char *name)
{
    int n = a[9], i;
    if (!name) return n == 0;
    if ((int)strlen(name) != n) return 0;
    for (i = 0; i < n; i++) if (rd16(a + rd16(a + 10) + 2 * i) != (uint8_t)name[i]) return 0;
    return 1;
}

/* একটা record-এর ভেতরে type/name মিলে এমন attribute-গুলো r-এ যোগ */
static int collect_in_record(struct ntfs_fs *fs, const uint8_t *rec, uint32_t type, const char *name,
                             struct ntfs_runs *r, int *found)
{
    uint32_t off = rd16(rec + 20);
    (void)fs;
    while (off + 16 <= fs->rec_size) {
        const uint8_t *a = rec + off;
        uint32_t t = rd32(a), len = rd32(a + 4);
        if (t == AT_END || len < 16 || off + len > fs->rec_size) break;
        if (t == type && attr_name_is(a, name)) {
            *found = 1;
            if (!a[8]) {                                 /* resident */
                uint32_t vl = rd32(a + 16);
                uint16_t vo = rd16(a + 20);
                if (vo + vl > len) return -EIO;
                kfree(r->res_data);
                r->res_data = (uint8_t *)kmalloc(vl ? vl : 1);
                if (!r->res_data) return -ENOMEM;
                memcpy(r->res_data, a + vo, vl);
                r->resident = 1;
                r->size = r->init_size = vl;
                r->flags = rd16(a + 12);
            } else {
                uint64_t svcn = rd64(a + 16);
                int err;
                if (svcn == 0) {
                    r->size = rd64(a + 48);
                    r->init_size = rd64(a + 56);
                    r->flags = rd16(a + 12);
                }
                err = decode_runs(r, a + rd16(a + 32), a + len, svcn);
                if (err < 0) return err;
            }
        }
        off += len;
    }
    return 0;
}

/* পুরো attribute (attribute list থাকলে অন্য record থেকেও) */
static int load_attr(struct ntfs_fs *fs, uint64_t mftno, uint32_t type, const char *name, struct ntfs_runs *r)
{
    uint8_t *rec = (uint8_t *)kmalloc(fs->rec_size), *rec2 = NULL;
    struct ntfs_runs list;
    int found = 0, err;
    memset(r, 0, sizeof(*r));
    memset(&list, 0, sizeof(list));
    if (!rec) return -ENOMEM;
    err = read_record(fs, mftno, rec);
    if (err < 0) goto out;
    err = collect_in_record(fs, rec, AT_ATTRIBUTE_LIST, NULL, &list, &found);
    if (err < 0) goto out;
    if (!found) {
        err = collect_in_record(fs, rec, type, name, r, &found);
    } else {
        /* attribute list: প্রতিটা entry যে record-এ আছে সেখান থেকে */
        uint8_t *lb;
        uint64_t ll = list.size, i;
        found = 0;
        lb = list.resident ? list.res_data : (uint8_t *)kmalloc(ll ? ll : 1);
        if (!lb) { err = -ENOMEM; goto out; }
        if (!list.resident) {
            for (i = 0; i < ll; i += fs->cluster_size) {
                uint64_t left, lcn = runs_map(&list, i / fs->cluster_size, &left);
                size_t n = ll - i < fs->cluster_size ? (size_t)(ll - i) : fs->cluster_size;
                if (lcn >= ~0ull - 1) memset(lb + i, 0, n);
                else if (block_read_bytes(fs->dev, lcn * fs->cluster_size, lb + i, n) < 0) { err = -EIO; break; }
            }
        }
        rec2 = (uint8_t *)kmalloc(fs->rec_size);
        if (!rec2) err = -ENOMEM;
        for (i = 0; err >= 0 && i + 26 <= ll;) {
            const uint8_t *e = lb + i;
            uint16_t elen = rd16(e + 4);
            uint64_t ref = rd64(e + 16) & 0xFFFFFFFFFFFFull;
            if (elen < 26) break;
            if (rd32(e) == type) {
                int nm = e[6], match = 1, k;
                if (!name) match = nm == 0;
                else if ((int)strlen(name) != nm) match = 0;
                else for (k = 0; k < nm; k++) if (rd16(e + e[7] + 2 * k) != (uint8_t)name[k]) match = 0;
                if (match) {
                    const uint8_t *src = rec;
                    if (ref != mftno) {
                        err = read_record(fs, ref, rec2);
                        src = rec2;
                    }
                    if (err >= 0) {
                        /* একই record একাধিকবার এলে আবার যোগ নয়: start vcn দেখে বাদ */
                        int dup = 0;
                        uint32_t q;
                        uint64_t want = rd64(e + 8);
                        for (q = 0; q < r->n; q++) if (r->ext[q].vcn == want && want) dup = 1;
                        if (!dup && !(r->resident && want == 0)) err = collect_in_record(fs, src, type, name, r, &found);
                    }
                }
            }
            i += elen;
        }
        if (!list.resident) kfree(lb);
    }
    if (err >= 0 && !found) err = -ENOENT;
out:
    runs_free(&list);
    kfree(rec);
    kfree(rec2);
    if (err < 0) runs_free(r);
    return err < 0 ? err : 0;
}

/* attribute থেকে পড়া (non-resident: runs দিয়ে; sparse/initialized-এর পরে শূন্য) */
static int64_t runs_read(struct ntfs_fs *fs, const struct ntfs_runs *r, uint64_t off, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    if (off >= r->size) return 0;
    if (len > r->size - off) len = (size_t)(r->size - off);
    if (r->resident) {
        memcpy(out, r->res_data + off, len);
        return (int64_t)len;
    }
    while (done < len) {
        uint64_t pos = off + done, vcn = pos / fs->cluster_size, in = pos % fs->cluster_size, left, lcn;
        size_t n;
        lcn = runs_map(r, vcn, &left);
        n = (size_t)(left * fs->cluster_size - in);
        if (left == 0) n = fs->cluster_size - (size_t)in;
        if (n > len - done) n = len - done;
        if (pos >= r->init_size || lcn >= ~0ull - 1) {
            memset(out + done, 0, n);
        } else {
            if (pos + n > r->init_size) n = (size_t)(r->init_size - pos);
            if (block_read_bytes(fs->dev, lcn * fs->cluster_size + in, out + done, n) < 0) return -EIO;
        }
        done += n;
    }
    return (int64_t)done;
}

/* ---------- vnode ---------- */

static struct ntfs_vnode *get_vnode(struct ntfs_fs *fs, uint64_t mftno, int isdir, uint64_t size, uint64_t mtime,
                                    struct vnode *parent)
{
    struct ntfs_vnode *v;
    for (v = fs->vnodes; v; v = v->next) {
        if (v->mftno == mftno) {
            vnode_ref(&v->vn);
            return v;
        }
    }
    v = (struct ntfs_vnode *)kzalloc(sizeof(*v));
    if (!v) return NULL;
    v->fs = fs;
    v->mftno = mftno;
    v->vn.type = isdir ? VNODE_DIR : VNODE_FILE;
    v->vn.mode = isdir ? 0555 : 0444;
    v->vn.size = isdir ? 0 : size;
    v->vn.inode = mftno;
    v->vn.mtime = mtime;
    v->vn.nlink = 1;
    v->vn.refcount = 1;
    v->vn.ops = &ntfs_ops;
    v->vn.fs_data = fs;
    if (isdir) v->vn.parent = parent;
    if (parent) v->vn.mount = parent->mount;
    v->next = fs->vnodes;
    fs->vnodes = v;
    return v;
}

static int load_data(struct ntfs_vnode *v)
{
    int err;
    if (v->data_loaded) return 0;
    err = load_attr(v->fs, v->mftno, AT_DATA, NULL, &v->data);
    if (err == -ENOENT) { memset(&v->data, 0, sizeof(v->data)); v->data.resident = 1; err = 0; }
    if (err < 0) return err;
    v->data_loaded = 1;
    v->vn.size = v->data.size;
    return 0;
}

static int64_t ntfs_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct ntfs_vnode *v = NV(vn);
    int err;
    if (vn->type == VNODE_DIR) return -EISDIR;
    err = load_data(v);
    if (err < 0) return err;
    if (v->data.flags & 0x4000) return -EACCES;          /* encrypted */
    if ((v->data.flags & 0x00FF) && !v->data.resident) return -EIO;   /* compressed: এখনো নয় */
    return runs_read(v->fs, &v->data, off, buf, len);
}

static int64_t ntfs_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)vn; (void)off; (void)buf; (void)len;
    return -EROFS;
}

static int ntfs_truncate(struct vnode *vn, uint64_t size) { (void)vn; (void)size; return -EROFS; }

/* ---------- ডিরেক্টরি ---------- */

struct dent {
    uint64_t mft;
    int      isdir;
    uint64_t size, mtime;
    char     name[VFS_NAME_MAX + 1];
};

typedef int (*dent_fn)(const struct dent *d, void *ctx);   /* ১ = থামো */

/* একসারি index entry (index header-এর পরে) দেখা */
static int walk_entries(const uint8_t *p, const uint8_t *end, dent_fn fn, void *ctx)
{
    while (p + 16 <= end) {
        uint16_t elen = rd16(p + 8), klen = rd16(p + 10), fl = rd16(p + 12);
        if (fl & 2) break;                               /* শেষ entry */
        if (elen < 16 || p + elen > end) break;
        if (klen >= 66) {
            const uint8_t *k = p + 16;
            int nl = k[64], ns = k[65];
            uint64_t ref = rd64(p) & 0xFFFFFFFFFFFFull;
            if (ns != 2 && 66 + 2 * nl <= klen && !(ref < 24 && nl > 0 && rd16(k + 66) == '$')) {
                struct dent d;
                d.mft = ref;
                d.isdir = (rd32(k + 56) & 0x10000000u) != 0;
                d.size = rd64(k + 48);
                d.mtime = ntfs_time(rd64(k + 16));
                name_to_utf8(k + 66, nl, d.name, sizeof(d.name));
                if (strcmp(d.name, ".") && fn(&d, ctx)) return 1;
            }
        }
        p += elen;
    }
    return 0;
}

static int walk_dir(struct ntfs_vnode *dir, dent_fn fn, void *ctx)
{
    struct ntfs_fs *fs = dir->fs;
    struct ntfs_runs root, alloc;
    const uint8_t *ih;
    uint8_t *blk = NULL;
    int err, stop;
    uint64_t off;
    err = load_attr(fs, dir->mftno, AT_INDEX_ROOT, "$I30", &root);
    if (err < 0) return err;
    if (!root.resident || root.size < 32) { runs_free(&root); return -EIO; }
    ih = root.res_data + 16;
    stop = walk_entries(ih + rd32(ih), ih + rd32(ih + 4), fn, ctx);
    runs_free(&root);
    if (stop) return 1;
    if (load_attr(fs, dir->mftno, AT_INDEX_ALLOCATION, "$I30", &alloc) < 0) return 0;
    blk = (uint8_t *)kmalloc(fs->idx_size);
    if (!blk) { runs_free(&alloc); return -ENOMEM; }
    for (off = 0; off + fs->idx_size <= alloc.size && !stop; off += fs->idx_size) {
        if (runs_read(fs, &alloc, off, blk, fs->idx_size) != (int64_t)fs->idx_size) continue;
        if (memcmp(blk, "INDX", 4) || fixup(blk, fs->idx_size, fs->bps) < 0) continue;
        ih = blk + 24;
        if (24 + rd32(ih + 4) > fs->idx_size) continue;
        stop = walk_entries(ih + rd32(ih), ih + rd32(ih + 4), fn, ctx);
    }
    kfree(blk);
    runs_free(&alloc);
    return stop;
}

struct find_ctx { const char *name; struct dent found; int ok; };

static int find_fn(const struct dent *d, void *ctx)
{
    struct find_ctx *f = (struct find_ctx *)ctx;
    if (!name_eq(d->name, f->name)) return 0;
    f->found = *d;
    f->ok = 1;
    return 1;
}

static int ntfs_lookup(struct vnode *dirvn, const char *name, struct vnode **out)
{
    struct find_ctx f;
    struct ntfs_vnode *c;
    int r;
    if (dirvn->type != VNODE_DIR) return -ENOTDIR;
    f.name = name;
    f.ok = 0;
    r = walk_dir(NV(dirvn), find_fn, &f);
    if (r < 0) return r;
    if (!f.ok) return -ENOENT;
    c = get_vnode(NV(dirvn)->fs, f.found.mft, f.found.isdir, f.found.size, f.found.mtime, dirvn);
    if (!c) return -ENOMEM;
    *out = &c->vn;
    return 0;
}

struct rd_ctx { uint64_t want, seen; struct vfs_dirent *out; };

static int rd_fn(const struct dent *d, void *ctx)
{
    struct rd_ctx *r = (struct rd_ctx *)ctx;
    if (r->seen++ != r->want) return 0;
    memcpy(r->out->name, d->name, sizeof(r->out->name));
    r->out->type = d->isdir ? VNODE_DIR : VNODE_FILE;
    r->out->inode = d->mft;
    r->out->size = d->isdir ? 0 : d->size;
    return 1;
}

static int ntfs_readdir(struct vnode *dirvn, uint64_t index, struct vfs_dirent *out)
{
    struct rd_ctx r;
    r.want = index;
    r.seen = 0;
    r.out = out;
    return walk_dir(NV(dirvn), rd_fn, &r);
}

static int ntfs_create(struct vnode *d, const char *n, enum vnode_type t, struct vnode **o)
{
    (void)d; (void)n; (void)t; (void)o;
    return -EROFS;
}

static int ntfs_unlink(struct vnode *d, const char *n) { (void)d; (void)n; return -EROFS; }

static int ntfs_rename(struct vnode *a, const char *b, struct vnode *c, const char *d)
{
    (void)a; (void)b; (void)c; (void)d;
    return -EROFS;
}

static void ntfs_release(struct vnode *vn) { (void)vn; }   /* cache-এ থাকে (umount পর্যন্ত) */

static const struct vnode_ops ntfs_ops = {
    ntfs_read, ntfs_write, ntfs_truncate, ntfs_lookup, ntfs_create,
    ntfs_unlink, ntfs_readdir, ntfs_release, NULL, ntfs_rename, NULL
};

/* ---------- mount ---------- */

static int ntfs_mount(const void *arg, struct vnode **root)
{
    struct block_device *dev = block_find((const char *)arg);
    struct ntfs_fs *fs;
    struct ntfs_vnode *r;
    uint8_t bs[512];
    int8_t c;
    uint8_t *rec;
    int err;

    if (!dev) return -ENODEV;
    if (block_read_bytes(dev, 0, bs, sizeof(bs)) < 0) return -EIO;
    if (memcmp(bs + 3, "NTFS    ", 8)) return -EINVAL;
    fs = (struct ntfs_fs *)kzalloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    fs->dev = dev;
    fs->bps = rd16(bs + 11);
    fs->cluster_size = bs[13] <= 128 ? fs->bps * bs[13] : 1u << (256 - bs[13]);
    fs->total_clusters = rd64(bs + 40) * fs->bps / (fs->cluster_size ? fs->cluster_size : 1);
    fs->mft_lcn = rd64(bs + 48);
    c = (int8_t)bs[64];
    fs->rec_size = c > 0 ? (uint32_t)c * fs->cluster_size : 1u << (-c);
    c = (int8_t)bs[68];
    fs->idx_size = c > 0 ? (uint32_t)c * fs->cluster_size : 1u << (-c);
    if (fs->bps < 256 || fs->bps > 4096 || !fs->cluster_size || fs->rec_size < 512 || fs->rec_size > 65536 ||
        fs->idx_size < 512 || fs->idx_size > 65536) {
        kfree(fs);
        return -EINVAL;
    }
    /* $MFT (record 0)-এর $DATA: বাকি সব record খুঁজতে */
    err = load_attr(fs, 0, AT_DATA, NULL, &fs->mft);
    if (err < 0) { kfree(fs); return err; }
    /* নাম: $Volume (record 3)-এর $VOLUME_NAME (0x60) */
    rec = (uint8_t *)kmalloc(fs->rec_size);
    if (rec && read_record(fs, 3, rec) == 0) {
        struct ntfs_runs vn;
        int f = 0;
        memset(&vn, 0, sizeof(vn));
        if (collect_in_record(fs, rec, 0x60, NULL, &vn, &f) == 0 && f && vn.resident)
            name_to_utf8(vn.res_data, (int)(vn.size / 2), fs->label, sizeof(fs->label));
        runs_free(&vn);
    }
    kfree(rec);

    r = (struct ntfs_vnode *)kzalloc(sizeof(*r));
    if (!r) { runs_free(&fs->mft); kfree(fs); return -ENOMEM; }
    r->fs = fs;
    r->mftno = 5;                                        /* root ডিরেক্টরি */
    r->vn.type = VNODE_DIR;
    r->vn.mode = 0555;
    r->vn.inode = 5;
    r->vn.nlink = 1;
    r->vn.refcount = 1;
    r->vn.ops = &ntfs_ops;
    r->vn.fs_data = fs;
    r->next = NULL;
    fs->vnodes = r;
    *root = &r->vn;
    return 0;
}

/* খালি জায়গা: $Bitmap (record 6)-এ শূন্য বিট গোনা — একবারই (বড় ডিস্কে কয়েক সেকেন্ড) */
static void count_free(struct ntfs_fs *fs)
{
    struct ntfs_runs bm;
    uint8_t *buf;
    uint64_t off, used = 0, bits = fs->total_clusters;
    fs->free_known = 1;
    if (load_attr(fs, 6, AT_DATA, NULL, &bm) < 0) return;
    buf = (uint8_t *)kmalloc(65536);
    for (off = 0; buf && off * 8 < bits; off += 65536) {
        int64_t n = runs_read(fs, &bm, off, buf, 65536), i;
        if (n <= 0) break;
        for (i = 0; i < n && (off + (uint64_t)i) * 8 < bits; i++) {
            uint8_t b = buf[i];
            while (b) { used += b & 1; b >>= 1; }
        }
    }
    kfree(buf);
    runs_free(&bm);
    fs->free_clusters = used < bits ? bits - used : 0;
}

static int ntfs_statfs(struct vnode *root, struct vfs_statfs *out)
{
    struct ntfs_fs *fs = NV(root)->fs;
    size_t n = strlen(fs->dev->name);
    if (!fs->free_known) count_free(fs);
    out->block_size = fs->cluster_size;
    out->total_blocks = fs->total_clusters;
    out->free_blocks = fs->free_clusters;
    out->total_inodes = 0;
    out->free_inodes = 0;
    out->read_only = 1;
    if (n >= sizeof(out->source)) n = sizeof(out->source) - 1;
    memcpy(out->source, fs->dev->name, n);
    out->source[n] = '\0';
    return 0;
}

static void ntfs_info(struct vnode *root)
{
    struct ntfs_fs *fs = NV(root)->fs;
    kprintf("      type            NTFS (read-only)\n");
    kprintf("      label           \"%s\"\n", fs->label);
    kprintf("      cluster size    %u bytes, %llu clusters (%llu MiB)\n", (unsigned)fs->cluster_size,
            (unsigned long long)fs->total_clusters,
            (unsigned long long)(fs->total_clusters * fs->cluster_size / (1024 * 1024)));
    kprintf("      MFT             at cluster %llu, record %u bytes, index record %u bytes, %u extents\n",
            (unsigned long long)fs->mft_lcn, (unsigned)fs->rec_size, (unsigned)fs->idx_size, (unsigned)fs->mft.n);
}

static int ntfs_umount(struct vnode *root)
{
    struct ntfs_fs *fs = NV(root)->fs;
    struct ntfs_vnode *v, *nx;
    for (v = fs->vnodes; v; v = v->next) {
        if (&v->vn != root && v->vn.refcount > 0) return -EBUSY;
    }
    for (v = fs->vnodes; v; v = nx) {                    /* সব vnode ফেরত (root vfs ছাড়বে) */
        nx = v->next;
        runs_free(&v->data);
        if (&v->vn != root) kfree(v);
    }
    fs->vnodes = NV(root);
    NV(root)->next = NULL;
    return 0;
}

/* ধাপ ২৪গ: ভলিউমের নাম (Files-এ দেখাতে) */
const char *ntfs_label(struct vnode *root) { return NV(root)->fs->label; }

const struct filesystem ntfs_filesystem = { "ntfs", ntfs_mount, ntfs_statfs, ntfs_info, ntfs_umount };
