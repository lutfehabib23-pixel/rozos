/* rozOS — ext2 ফাইলসিস্টেম (পড়া + লেখা)
 *
 * ডিস্কের বিন্যাস (block size = 1024 << s_log_block_size):
 *   [boot 1 KiB][superblock 1 KiB][group descriptor table][...block groups...]
 *   প্রতিটি group: block bitmap, inode bitmap, inode table, ডেটা ব্লক
 * inode-এর ১৫টা ব্লক পয়েন্টার: ১২টা সরাসরি, তারপর single/double/triple indirect।
 *
 * সাপোর্ট:
 *   incompat: FILETYPE (ডিরেক্টরি এন্ট্রিতে ফাইলের ধরন) — অন্য কিছু থাকলে mount হবে না
 *   ro_compat: SPARSE_SUPER, LARGE_FILE — অজানা কিছু থাকলে শুধু-পড়া mount
 *   compat ফিচার (dir_index, resize_inode, ext_attr) উপেক্ষা করা নিরাপদ; dir_index-ওয়ালা
 *   ডিরেক্টরিতে লিখলে তার INDEX ফ্ল্যাগ মুছে দেওয়া হয় (e2fsck এটা মেনে নেয়)।
 * সীমা: ফাইল সর্বোচ্চ 4 GiB-1, symlink/hardlink/rename নেই (পরে)।
 *
 * প্রতিটি পরিবর্তন সাথে সাথে ডিস্কে লেখা হয় (bitmap → group descriptor → superblock),
 * ব্লক স্তরের cache write-through — তাই হঠাৎ বন্ধ হলেও e2fsck সাধারণত পরিষ্কার পায়।
 *
 * vnode cache: প্রতিটি inode-এর একটাই vnode (mount point ঠিক রাখতে জরুরি)।
 * মুছে না ফেলা পর্যন্ত vnode মেমোরিতে থাকে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/block.h>
#include <rozos/errno.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/clock.h>
#include <rozos/compiler.h>

#define EXT2_MAGIC              0xEF53
#define EXT2_ROOT_INO           2
#define EXT2_NDIR_BLOCKS        12

#define EXT2_INCOMPAT_FILETYPE  0x0002
#define EXT2_RO_SPARSE_SUPER    0x0001
#define EXT2_RO_LARGE_FILE      0x0002
#define EXT2_RO_BTREE_DIR       0x0004
#define EXT2_INDEX_FL           0x00001000

#define S_IFMT   0xF000
#define S_IFDIR  0x4000
#define S_IFREG  0x8000
#define S_IFCHR  0x2000

#define FT_REG   1
#define FT_DIR   2

#define EXT2_MAX_FILE 0xFFFFFFFFULL

struct ext2_super {
    uint32_t inodes_count, blocks_count, r_blocks_count, free_blocks_count, free_inodes_count;
    uint32_t first_data_block, log_block_size, log_frag_size;
    uint32_t blocks_per_group, frags_per_group, inodes_per_group;
    uint32_t mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev_level;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size, block_group_nr;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
} ROZ_PACKED;

struct ext2_gd {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, pad;
    uint32_t reserved[3];
} ROZ_PACKED;

struct ext2_inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_high, faddr;
    uint8_t  osd2[12];
};   /* সব ফিল্ড স্বাভাবিকভাবেই aligned — packed লাগে না (আকার 128, নিচে যাচাই) */

/* কম্পাইলের সময় আকার যাচাই (C99-তে static_assert নেই) */
typedef char ext2_inode_size_check[(sizeof(struct ext2_inode) == 128) ? 1 : -1];
typedef char ext2_gd_size_check[(sizeof(struct ext2_gd) == 32) ? 1 : -1];

struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len, file_type;
    char     name[];
} ROZ_PACKED;

struct ext2_vnode;

struct ext2_fs {
    struct block_device *dev;
    uint8_t  sb_raw[1024];
    struct ext2_super *sb;
    struct ext2_gd *gdt;
    uint32_t block_size, ptrs_per_block, groups, inode_size;
    uint32_t gdt_block;
    int      read_only;
    struct ext2_vnode *vnodes;
};

struct ext2_vnode {
    struct vnode vn;                       /* প্রথম সদস্য */
    struct ext2_fs *fs;
    uint32_t ino;
    struct ext2_inode inode;
    struct ext2_vnode *next;
};

static const struct vnode_ops ext2_ops;

static inline struct ext2_vnode *EV(struct vnode *vn) { return (struct ext2_vnode *)vn; }
static inline uint32_t now32(void) { return (uint32_t)clock_unix_time(); }

/* ---------- নিম্নস্তরের I/O ---------- */

static int read_block(struct ext2_fs *fs, uint32_t blk, void *buf)
{
    return block_read_bytes(fs->dev, (uint64_t)blk * fs->block_size, buf, fs->block_size);
}

static int write_block(struct ext2_fs *fs, uint32_t blk, const void *buf)
{
    return block_write_bytes(fs->dev, (uint64_t)blk * fs->block_size, buf, fs->block_size);
}

static int write_super(struct ext2_fs *fs)
{
    fs->sb->wtime = now32();
    return block_write_bytes(fs->dev, 1024, fs->sb_raw, 1024);
}

static int write_gd(struct ext2_fs *fs, uint32_t g)
{
    return block_write_bytes(fs->dev, (uint64_t)fs->gdt_block * fs->block_size + g * sizeof(struct ext2_gd),
                             &fs->gdt[g], sizeof(struct ext2_gd));
}

static uint64_t inode_offset(struct ext2_fs *fs, uint32_t ino)
{
    uint32_t g = (ino - 1) / fs->sb->inodes_per_group;
    uint32_t i = (ino - 1) % fs->sb->inodes_per_group;
    return (uint64_t)fs->gdt[g].inode_table * fs->block_size + (uint64_t)i * fs->inode_size;
}

static int read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *out)
{
    if (ino == 0 || ino > fs->sb->inodes_count) {
        return -EINVAL;
    }
    return block_read_bytes(fs->dev, inode_offset(fs, ino), out, sizeof(*out));
}

/* শুধু প্রথম 128 বাইট লেখা হয় — বড় inode-এর বাকি অংশ (ext. fields) অক্ষত থাকে */
static int write_inode(struct ext2_vnode *ev)
{
    return block_write_bytes(ev->fs->dev, inode_offset(ev->fs, ev->ino), &ev->inode, sizeof(ev->inode));
}

static void sync_vnode_fields(struct ext2_vnode *ev)
{
    struct vnode *vn = &ev->vn;
    uint16_t fmt = ev->inode.mode & S_IFMT;
    vn->type = (fmt == S_IFDIR) ? VNODE_DIR : (fmt == S_IFCHR) ? VNODE_CHARDEV : VNODE_FILE;
    vn->mode = ev->inode.mode & 0xFFF;
    vn->size = ev->inode.size;
    if (fmt == S_IFREG && (ev->fs->sb->feature_ro_compat & EXT2_RO_LARGE_FILE)) {
        vn->size |= (uint64_t)ev->inode.size_high << 32;
    }
    vn->mtime = ev->inode.mtime;
    vn->nlink = ev->inode.links;
}

/* ---------- bitmap বরাদ্দ ---------- */

static int find_and_set_bit(uint8_t *bm, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        if ((i & 7) == 0 && i + 8 <= count && bm[i >> 3] == 0xFF) {
            i += 7;
            continue;
        }
        if (!(bm[i >> 3] & (1u << (i & 7)))) {
            bm[i >> 3] |= (uint8_t)(1u << (i & 7));
            return (int)i;
        }
    }
    return -1;
}

static uint32_t blocks_in_group(struct ext2_fs *fs, uint32_t g)
{
    uint32_t start = fs->sb->first_data_block + g * fs->sb->blocks_per_group;
    uint32_t n = fs->sb->blocks_count - start;
    return n < fs->sb->blocks_per_group ? n : fs->sb->blocks_per_group;
}

/* শূন্য-করা একটা নতুন ব্লক; ব্যর্থ হলে 0 */
static uint32_t alloc_block(struct ext2_fs *fs, uint32_t goal_group)
{
    uint8_t *bm;
    uint32_t i, result = 0;

    if (fs->sb->free_blocks_count == 0) {
        return 0;
    }
    bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) {
        return 0;
    }
    for (i = 0; i < fs->groups && !result; i++) {
        uint32_t g = (goal_group + i) % fs->groups;
        int bit;
        if (fs->gdt[g].free_blocks == 0 || read_block(fs, fs->gdt[g].block_bitmap, bm) < 0) {
            continue;
        }
        bit = find_and_set_bit(bm, blocks_in_group(fs, g));
        if (bit < 0) {
            continue;
        }
        if (write_block(fs, fs->gdt[g].block_bitmap, bm) < 0) {
            break;
        }
        fs->gdt[g].free_blocks--;
        fs->sb->free_blocks_count--;
        write_gd(fs, g);
        write_super(fs);
        result = fs->sb->first_data_block + g * fs->sb->blocks_per_group + (uint32_t)bit;
    }
    if (result) {
        memset(bm, 0, fs->block_size);
        write_block(fs, result, bm);
    }
    kfree(bm);
    return result;
}

static void free_block(struct ext2_fs *fs, uint32_t blk)
{
    uint8_t *bm;
    uint32_t g, bit;

    if (blk < fs->sb->first_data_block || blk >= fs->sb->blocks_count) {
        return;
    }
    g = (blk - fs->sb->first_data_block) / fs->sb->blocks_per_group;
    bit = (blk - fs->sb->first_data_block) % fs->sb->blocks_per_group;
    bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm || read_block(fs, fs->gdt[g].block_bitmap, bm) < 0) {
        kfree(bm);
        return;
    }
    if (bm[bit >> 3] & (1u << (bit & 7))) {
        bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
        write_block(fs, fs->gdt[g].block_bitmap, bm);
        fs->gdt[g].free_blocks++;
        fs->sb->free_blocks_count++;
        write_gd(fs, g);
        write_super(fs);
    }
    kfree(bm);
}

static uint32_t alloc_inode(struct ext2_fs *fs, int is_dir, uint32_t goal_group)
{
    uint8_t *bm;
    uint32_t i, result = 0;

    if (fs->sb->free_inodes_count == 0) {
        return 0;
    }
    bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) {
        return 0;
    }
    for (i = 0; i < fs->groups && !result; i++) {
        uint32_t g = (goal_group + i) % fs->groups;
        int bit;
        if (fs->gdt[g].free_inodes == 0 || read_block(fs, fs->gdt[g].inode_bitmap, bm) < 0) {
            continue;
        }
        bit = find_and_set_bit(bm, fs->sb->inodes_per_group);
        if (bit < 0 || g * fs->sb->inodes_per_group + (uint32_t)bit + 1 < fs->sb->first_ino) {
            continue;                                   /* সংরক্ষিত inode কখনো নয় */
        }
        if (write_block(fs, fs->gdt[g].inode_bitmap, bm) < 0) {
            break;
        }
        fs->gdt[g].free_inodes--;
        if (is_dir) {
            fs->gdt[g].used_dirs++;
        }
        fs->sb->free_inodes_count--;
        write_gd(fs, g);
        write_super(fs);
        result = g * fs->sb->inodes_per_group + (uint32_t)bit + 1;
    }
    kfree(bm);
    return result;
}

static void free_inode(struct ext2_fs *fs, uint32_t ino, int is_dir)
{
    uint8_t *bm;
    uint32_t g = (ino - 1) / fs->sb->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->sb->inodes_per_group;

    bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm || read_block(fs, fs->gdt[g].inode_bitmap, bm) < 0) {
        kfree(bm);
        return;
    }
    if (bm[bit >> 3] & (1u << (bit & 7))) {
        bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
        write_block(fs, fs->gdt[g].inode_bitmap, bm);
        fs->gdt[g].free_inodes++;
        if (is_dir && fs->gdt[g].used_dirs) {
            fs->gdt[g].used_dirs--;
        }
        fs->sb->free_inodes_count++;
        write_gd(fs, g);
        write_super(fs);
    }
    kfree(bm);
}

/* ---------- logical → physical ব্লক ---------- */

static uint32_t group_of_inode(struct ext2_fs *fs, uint32_t ino)
{
    return (ino - 1) / fs->sb->inodes_per_group;
}

/* *slot শূন্য হলে create = 1 এ নতুন ব্লক বসায়; ফেরত ব্লক নম্বর (0 = নেই/ব্যর্থ) */
static uint32_t ensure_slot(struct ext2_vnode *ev, uint32_t *slot, int create, int *dirty)
{
    if (*slot == 0 && create) {
        uint32_t b = alloc_block(ev->fs, group_of_inode(ev->fs, ev->ino));
        if (b) {
            *slot = b;
            ev->inode.blocks += ev->fs->block_size / 512;
            *dirty = 1;
        }
    }
    return *slot;
}

static uint32_t get_block(struct ext2_vnode *ev, uint32_t lblock, int create)
{
    struct ext2_fs *fs = ev->fs;
    uint32_t P = fs->ptrs_per_block, path[3], depth, top, blk, i;
    uint32_t *buf;
    int dirty = 0;

    if (lblock < EXT2_NDIR_BLOCKS) {
        blk = ensure_slot(ev, &ev->inode.block[lblock], create, &dirty);
        if (dirty) write_inode(ev);
        return blk;
    }
    lblock -= EXT2_NDIR_BLOCKS;
    if (lblock < P) {
        depth = 1; top = 12; path[0] = lblock;
    } else if ((lblock -= P) < P * P) {
        depth = 2; top = 13; path[0] = lblock / P; path[1] = lblock % P;
    } else {
        lblock -= P * P;
        if (lblock / (P * P) >= P) return 0;            /* সীমার বাইরে */
        depth = 3; top = 14; path[0] = lblock / (P * P); path[1] = (lblock / P) % P; path[2] = lblock % P;
    }

    blk = ensure_slot(ev, &ev->inode.block[top], create, &dirty);
    if (dirty) write_inode(ev);
    if (!blk) return 0;

    buf = (uint32_t *)kmalloc(fs->block_size);
    if (!buf) return 0;
    for (i = 0; i < depth; i++) {
        int bdirty = 0;
        if (read_block(fs, blk, buf) < 0) { blk = 0; break; }
        ensure_slot(ev, &buf[path[i]], create, &bdirty);
        if (bdirty) {
            write_block(fs, blk, buf);
            write_inode(ev);
        }
        blk = buf[path[i]];
        if (!blk) break;
    }
    kfree(buf);
    return blk;
}

/* first_lblock থেকে শুরু করে সব ব্লক মুক্ত (truncate) — level = এই ব্লকের নিচে কয় স্তর */
static int free_tree(struct ext2_vnode *ev, uint32_t blk, uint32_t level, uint64_t base, uint64_t first)
{
    struct ext2_fs *fs = ev->fs;
    uint32_t P = fs->ptrs_per_block, i;
    uint64_t span = 1;
    uint32_t *buf = (uint32_t *)kmalloc(fs->block_size);
    int changed = 0;

    for (i = 1; i < level; i++) span *= P;
    if (!buf || read_block(fs, blk, buf) < 0) {
        kfree(buf);
        return 0;
    }
    for (i = 0; i < P; i++) {
        uint64_t cbase = base + (uint64_t)i * span;
        if (cbase + span <= first || !buf[i]) {
            continue;
        }
        if (level > 1) {
            free_tree(ev, buf[i], level - 1, cbase, first);
        }
        if (cbase >= first) {
            free_block(fs, buf[i]);
            ev->inode.blocks -= fs->block_size / 512;
            buf[i] = 0;
            changed = 1;
        }
    }
    if (changed) write_block(fs, blk, buf);
    kfree(buf);
    return 0;
}

static void free_blocks_from(struct ext2_vnode *ev, uint64_t first)
{
    struct ext2_fs *fs = ev->fs;
    uint64_t P = fs->ptrs_per_block;
    uint64_t base[3];
    uint32_t i;

    for (i = (uint32_t)(first < EXT2_NDIR_BLOCKS ? first : EXT2_NDIR_BLOCKS); i < EXT2_NDIR_BLOCKS; i++) {
        if (ev->inode.block[i]) {
            free_block(fs, ev->inode.block[i]);
            ev->inode.blocks -= fs->block_size / 512;
            ev->inode.block[i] = 0;
        }
    }
    base[0] = EXT2_NDIR_BLOCKS;
    base[1] = base[0] + P;
    base[2] = base[1] + P * P;
    for (i = 0; i < 3; i++) {
        uint32_t *slot = &ev->inode.block[12 + i];
        if (!*slot) continue;
        free_tree(ev, *slot, i + 1, base[i], first);
        if (base[i] >= first) {
            free_block(fs, *slot);
            ev->inode.blocks -= fs->block_size / 512;
            *slot = 0;
        }
    }
    write_inode(ev);
}

/* ---------- vnode cache ---------- */

static struct ext2_vnode *get_vnode(struct ext2_fs *fs, uint32_t ino, struct vnode *parent)
{
    struct ext2_vnode *ev;
    for (ev = fs->vnodes; ev; ev = ev->next) {
        if (ev->ino == ino) {
            vnode_ref(&ev->vn);
            return ev;
        }
    }
    ev = (struct ext2_vnode *)kzalloc(sizeof(*ev));
    if (!ev) return NULL;
    if (read_inode(fs, ino, &ev->inode) < 0) {
        kfree(ev);
        return NULL;
    }
    ev->fs = fs;
    ev->ino = ino;
    ev->vn.ops = &ext2_ops;
    ev->vn.fs_data = fs;
    ev->vn.inode = ino;
    ev->vn.refcount = 1;
    sync_vnode_fields(ev);
    if (ev->vn.type == VNODE_DIR) {
        ev->vn.parent = parent;
    }
    if (parent) {
        ev->vn.mount = parent->mount;
    }
    ev->next = fs->vnodes;
    fs->vnodes = ev;
    return ev;
}

static void drop_from_cache(struct ext2_vnode *ev)
{
    struct ext2_vnode **pp;
    for (pp = &ev->fs->vnodes; *pp; pp = &(*pp)->next) {
        if (*pp == ev) {
            *pp = ev->next;
            return;
        }
    }
}

/* লিংক শূন্য: ব্লক ও inode মুক্ত */
static void destroy_inode(struct ext2_vnode *ev)
{
    int is_dir = (ev->inode.mode & S_IFMT) == S_IFDIR;
    free_blocks_from(ev, 0);
    ev->inode.size = 0;
    ev->inode.size_high = 0;
    ev->inode.dtime = now32();
    write_inode(ev);
    free_inode(ev->fs, ev->ino, is_dir);
    drop_from_cache(ev);
    kfree(ev);
}

/* ---------- ফাইল অপারেশন ---------- */

static int64_t ext2_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct ext2_vnode *ev = EV(vn);
    struct ext2_fs *fs = ev->fs;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;

    if (off >= vn->size) return 0;
    if (len > vn->size - off) len = (size_t)(vn->size - off);

    while (done < len) {
        uint32_t lblock = (uint32_t)((off + done) / fs->block_size);
        uint32_t in = (uint32_t)((off + done) % fs->block_size);
        size_t n = fs->block_size - in;
        uint32_t pblk = get_block(ev, lblock, 0);
        if (n > len - done) n = len - done;
        if (pblk == 0) {
            memset(out + done, 0, n);                  /* sparse ফাইলের ফাঁকা অংশ */
        } else if (block_read_bytes(fs->dev, (uint64_t)pblk * fs->block_size + in, out + done, n) < 0) {
            return done ? (int64_t)done : -EIO;
        }
        done += n;
    }
    return (int64_t)done;
}

static int64_t ext2_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct ext2_vnode *ev = EV(vn);
    struct ext2_fs *fs = ev->fs;
    const uint8_t *in_buf = (const uint8_t *)buf;
    size_t done = 0;

    if (fs->read_only) return -EROFS;
    if (len == 0) return 0;
    if (off + len > EXT2_MAX_FILE) return -EFBIG;

    while (done < len) {
        uint32_t lblock = (uint32_t)((off + done) / fs->block_size);
        uint32_t in = (uint32_t)((off + done) % fs->block_size);
        size_t n = fs->block_size - in;
        uint32_t pblk;
        if (n > len - done) n = len - done;
        pblk = get_block(ev, lblock, 1);
        if (!pblk) break;                               /* ডিস্ক ভরা */
        if (block_write_bytes(fs->dev, (uint64_t)pblk * fs->block_size + in, in_buf + done, n) < 0) {
            break;
        }
        done += n;
    }
    if (off + done > vn->size) {
        vn->size = off + done;
        ev->inode.size = (uint32_t)vn->size;
    }
    ev->inode.mtime = ev->inode.ctime = now32();
    vn->mtime = ev->inode.mtime;
    write_inode(ev);
    return done ? (int64_t)done : -ENOSPC;
}

static int ext2_truncate(struct vnode *vn, uint64_t size)
{
    struct ext2_vnode *ev = EV(vn);
    struct ext2_fs *fs = ev->fs;

    if (fs->read_only) return -EROFS;
    if (size > EXT2_MAX_FILE) return -EFBIG;
    if (size < vn->size) {
        uint64_t keep = (size + fs->block_size - 1) / fs->block_size;
        uint32_t tail = (uint32_t)(size % fs->block_size);
        free_blocks_from(ev, keep);
        if (tail) {                                     /* শেষ ব্লকের বাকি অংশ শূন্য */
            uint32_t pblk = get_block(ev, (uint32_t)(size / fs->block_size), 0);
            if (pblk) {
                uint8_t *z = (uint8_t *)kzalloc(fs->block_size - tail);
                if (z) {
                    block_write_bytes(fs->dev, (uint64_t)pblk * fs->block_size + tail, z, fs->block_size - tail);
                    kfree(z);
                }
            }
        }
    }
    vn->size = size;
    ev->inode.size = (uint32_t)size;
    ev->inode.mtime = ev->inode.ctime = now32();
    vn->mtime = ev->inode.mtime;
    return write_inode(ev);
}

/* ---------- ডিরেক্টরি ---------- */

typedef int (*dir_visit_t)(struct ext2_dirent *e, void *ctx);

/* সব এন্ট্রিতে visit; visit 1 ফেরত দিলে থামে (1 ফেরত দেয়), ত্রুটি হলে <0 */
static int walk_dir(struct ext2_vnode *dir, dir_visit_t visit, void *ctx)
{
    struct ext2_fs *fs = dir->fs;
    uint32_t nblocks = (uint32_t)((dir->vn.size + fs->block_size - 1) / fs->block_size), b;
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    int r = 0;

    if (!buf) return -ENOMEM;
    for (b = 0; b < nblocks && r == 0; b++) {
        uint32_t pblk = get_block(dir, b, 0), pos = 0;
        if (!pblk || read_block(fs, pblk, buf) < 0) continue;
        while (pos + 8 <= fs->block_size) {
            struct ext2_dirent *e = (struct ext2_dirent *)(buf + pos);
            if (e->rec_len < 8 || pos + e->rec_len > fs->block_size) break;   /* নষ্ট এন্ট্রি */
            if (e->inode && (r = visit(e, ctx)) != 0) break;
            pos += e->rec_len;
        }
    }
    kfree(buf);
    return r;
}

struct find_ctx {
    const char *name;
    size_t len;
    uint32_t ino;
    uint8_t type;
};

static int find_visit(struct ext2_dirent *e, void *ctx)
{
    struct find_ctx *f = (struct find_ctx *)ctx;
    if (e->name_len == f->len && !memcmp(e->name, f->name, f->len)) {
        f->ino = e->inode;
        f->type = e->file_type;
        return 1;
    }
    return 0;
}

static int ext2_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct find_ctx f;
    struct ext2_vnode *child;

    f.name = name;
    f.len = strlen(name);
    f.ino = 0;
    if (f.len > 255) return -ENAMETOOLONG;
    if (walk_dir(EV(dir), find_visit, &f) <= 0 || !f.ino) return -ENOENT;
    child = get_vnode(EV(dir)->fs, f.ino, dir);
    if (!child) return -EIO;
    *out = &child->vn;
    return 0;
}

struct readdir_ctx {
    uint64_t want;
    uint64_t seen;
    struct vfs_dirent *out;
    struct ext2_fs *fs;
};

static int readdir_visit(struct ext2_dirent *e, void *ctx)
{
    struct readdir_ctx *r = (struct readdir_ctx *)ctx;
    if ((e->name_len == 1 && e->name[0] == '.') ||
        (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.')) {
        return 0;                                       /* "." ও ".." দেখানো হয় না */
    }
    if (r->seen++ == r->want) {
        struct ext2_inode ino;
        memcpy(r->out->name, e->name, e->name_len);
        r->out->name[e->name_len] = '\0';
        r->out->inode = e->inode;
        r->out->size = 0;
        r->out->type = (e->file_type == FT_DIR) ? VNODE_DIR : VNODE_FILE;
        if (read_inode(r->fs, e->inode, &ino) == 0) {
            r->out->size = ino.size;
            r->out->type = ((ino.mode & S_IFMT) == S_IFDIR) ? VNODE_DIR : VNODE_FILE;
        }
        return 1;
    }
    return 0;
}

static int ext2_readdir(struct vnode *dir, uint64_t index, struct vfs_dirent *out)
{
    struct readdir_ctx r;
    int res;
    r.want = index;
    r.seen = 0;
    r.out = out;
    r.fs = EV(dir)->fs;
    res = walk_dir(EV(dir), readdir_visit, &r);
    return res;                                         /* 1 = পাওয়া গেছে, 0 = শেষ, <0 = ত্রুটি */
}

static uint16_t rec_len_for(size_t name_len)
{
    return (uint16_t)((8 + name_len + 3) & ~3u);
}

/* ডিরেক্টরিতে এন্ট্রি যোগ (জায়গা না থাকলে নতুন ব্লক) */
static int add_entry(struct ext2_vnode *dir, const char *name, uint32_t ino, uint8_t ftype)
{
    struct ext2_fs *fs = dir->fs;
    size_t len = strlen(name);
    uint16_t need = rec_len_for(len);
    uint32_t nblocks = (uint32_t)(dir->vn.size / fs->block_size), b;
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    int done = 0;

    if (!buf) return -ENOMEM;
    for (b = 0; b < nblocks && !done; b++) {
        uint32_t pblk = get_block(dir, b, 0), pos = 0;
        if (!pblk || read_block(fs, pblk, buf) < 0) continue;
        while (pos + 8 <= fs->block_size) {
            struct ext2_dirent *e = (struct ext2_dirent *)(buf + pos);
            uint16_t used = e->inode ? rec_len_for(e->name_len) : 0;
            if (e->rec_len < 8) break;
            if (e->rec_len - used >= need) {
                struct ext2_dirent *n = e;
                if (e->inode) {
                    n = (struct ext2_dirent *)(buf + pos + used);
                    n->rec_len = (uint16_t)(e->rec_len - used);
                    e->rec_len = used;
                }
                n->inode = ino;
                n->name_len = (uint8_t)len;
                n->file_type = ftype;
                memcpy(n->name, name, len);
                write_block(fs, pblk, buf);
                done = 1;
                break;
            }
            pos += e->rec_len;
        }
    }
    if (!done) {                                        /* নতুন ব্লক */
        uint32_t pblk = get_block(dir, nblocks, 1);
        struct ext2_dirent *n = (struct ext2_dirent *)buf;
        if (!pblk) {
            kfree(buf);
            return -ENOSPC;
        }
        memset(buf, 0, fs->block_size);
        n->inode = ino;
        n->rec_len = (uint16_t)fs->block_size;
        n->name_len = (uint8_t)len;
        n->file_type = ftype;
        memcpy(n->name, name, len);
        write_block(fs, pblk, buf);
        dir->vn.size += fs->block_size;
        dir->inode.size = (uint32_t)dir->vn.size;
    }
    kfree(buf);
    dir->inode.flags &= ~EXT2_INDEX_FL;                /* htree আর সঠিক নয় */
    dir->inode.mtime = dir->inode.ctime = now32();
    dir->vn.mtime = dir->inode.mtime;
    return write_inode(dir);
}

static int remove_entry(struct ext2_vnode *dir, const char *name)
{
    struct ext2_fs *fs = dir->fs;
    size_t len = strlen(name);
    uint32_t nblocks = (uint32_t)(dir->vn.size / fs->block_size), b;
    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    int found = 0;

    if (!buf) return -ENOMEM;
    for (b = 0; b < nblocks && !found; b++) {
        uint32_t pblk = get_block(dir, b, 0), pos = 0;
        struct ext2_dirent *prev = NULL;
        if (!pblk || read_block(fs, pblk, buf) < 0) continue;
        while (pos + 8 <= fs->block_size) {
            struct ext2_dirent *e = (struct ext2_dirent *)(buf + pos);
            if (e->rec_len < 8) break;
            if (e->inode && e->name_len == len && !memcmp(e->name, name, len)) {
                if (prev) {
                    prev->rec_len = (uint16_t)(prev->rec_len + e->rec_len);
                } else {
                    e->inode = 0;                       /* ব্লকের প্রথম এন্ট্রি — শুধু খালি চিহ্ন */
                }
                write_block(fs, pblk, buf);
                found = 1;
                break;
            }
            prev = e;
            pos += e->rec_len;
        }
    }
    kfree(buf);
    if (!found) return -ENOENT;
    dir->inode.flags &= ~EXT2_INDEX_FL;
    dir->inode.mtime = dir->inode.ctime = now32();
    dir->vn.mtime = dir->inode.mtime;
    return write_inode(dir);
}

static int ext2_create(struct vnode *dirvn, const char *name, enum vnode_type type, struct vnode **out)
{
    struct ext2_vnode *dir = EV(dirvn), *child;
    struct ext2_fs *fs = dir->fs;
    struct find_ctx f;
    struct ext2_inode ino;
    uint32_t inum, t = now32();
    int is_dir = (type == VNODE_DIR), err;
    size_t len = strlen(name);

    if (fs->read_only) return -EROFS;
    if (type != VNODE_FILE && type != VNODE_DIR) return -EINVAL;
    if (len == 0 || len > 255 || strchr(name, '/')) return -EINVAL;
    f.name = name; f.len = len; f.ino = 0;
    if (walk_dir(dir, find_visit, &f) > 0) return -EEXIST;

    inum = alloc_inode(fs, is_dir, group_of_inode(fs, dir->ino));
    if (!inum) return -ENOSPC;

    memset(&ino, 0, sizeof(ino));
    ino.mode = (uint16_t)(is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    ino.links = (uint16_t)(is_dir ? 2 : 1);
    ino.atime = ino.ctime = ino.mtime = t;
    {
        /* পুরো inode (128 বাইটের বড় হলেও) শূন্য করে লেখা — আগের মুছে ফেলা inode-এর
         * পুরনো বাড়তি ফিল্ড যেন না থেকে যায় */
        uint8_t *raw = (uint8_t *)kzalloc(fs->inode_size);
        int werr;
        if (!raw) {
            free_inode(fs, inum, is_dir);
            return -ENOMEM;
        }
        memcpy(raw, &ino, sizeof(ino));
        werr = block_write_bytes(fs->dev, inode_offset(fs, inum), raw, fs->inode_size);
        kfree(raw);
        if (werr < 0) {
            free_inode(fs, inum, is_dir);
            return -EIO;
        }
    }

    child = get_vnode(fs, inum, dirvn);
    if (!child) {
        free_inode(fs, inum, is_dir);
        return -ENOMEM;
    }

    if (is_dir) {
        /* "." ও ".." */
        uint8_t *buf = (uint8_t *)kzalloc(fs->block_size);
        uint32_t pblk = buf ? get_block(child, 0, 1) : 0;
        struct ext2_dirent *d1, *d2;
        if (!pblk) {
            kfree(buf);
            child->inode.links = 0;
            vnode_unref(&child->vn);
            destroy_inode(child);
            return -ENOSPC;
        }
        d1 = (struct ext2_dirent *)buf;
        d1->inode = inum; d1->rec_len = 12; d1->name_len = 1; d1->file_type = FT_DIR; d1->name[0] = '.';
        d2 = (struct ext2_dirent *)(buf + 12);
        d2->inode = dir->ino; d2->rec_len = (uint16_t)(fs->block_size - 12); d2->name_len = 2;
        d2->file_type = FT_DIR; d2->name[0] = '.'; d2->name[1] = '.';
        write_block(fs, pblk, buf);
        kfree(buf);
        child->inode.size = fs->block_size;
        write_inode(child);
        sync_vnode_fields(child);
    }

    err = add_entry(dir, name, inum, (uint8_t)(is_dir ? FT_DIR : FT_REG));
    if (err < 0) {
        /* vn.nlink আগের মানেই থাকে, তাই unref-এ release হয় না — destroy একবারই */
        child->inode.links = 0;
        vnode_unref(&child->vn);
        destroy_inode(child);
        return err;
    }
    if (is_dir) {
        dir->inode.links++;                             /* নতুন ডিরেক্টরির ".." */
        dir->vn.nlink = dir->inode.links;
        write_inode(dir);
    }
    *out = &child->vn;
    return 0;
}

static int ext2_unlink(struct vnode *dirvn, const char *name)
{
    struct ext2_vnode *dir = EV(dirvn), *child;
    struct vnode *cvn;
    int is_dir, err;

    if (dir->fs->read_only) return -EROFS;
    err = ext2_lookup(dirvn, name, &cvn);
    if (err < 0) return err;
    child = EV(cvn);
    is_dir = (child->inode.mode & S_IFMT) == S_IFDIR;

    err = remove_entry(dir, name);
    if (err < 0) {
        vnode_unref(cvn);
        return err;
    }
    if (is_dir) {
        child->inode.links = 0;                         /* "." আর parent-এর এন্ট্রি দুটোই গেল */
        if (dir->inode.links > 2) dir->inode.links--;   /* ".." */
        dir->vn.nlink = dir->inode.links;
        write_inode(dir);
    } else if (child->inode.links) {
        child->inode.links--;
    }
    child->inode.ctime = now32();
    write_inode(child);
    child->vn.nlink = child->inode.links;
    /* কেউ খুলে না রাখলে এখনই মুক্ত হবে (release), নইলে শেষ close-এ */
    vnode_unref(cvn);
    return 0;
}

static void ext2_release(struct vnode *vn)
{
    struct ext2_vnode *ev = EV(vn);
    if (ev->inode.links == 0) {
        destroy_inode(ev);
    }
}

static const struct vnode_ops ext2_ops = {
    ext2_read, ext2_write, ext2_truncate, ext2_lookup, ext2_create,
    ext2_unlink, ext2_readdir, ext2_release
};

/* ---------- mount / statfs ---------- */

static int ext2_mount(const void *arg, struct vnode **root)
{
    struct block_device *dev = block_find((const char *)arg);
    struct ext2_fs *fs;
    struct ext2_vnode *r;
    uint32_t gdt_bytes;

    if (!dev) return -ENODEV;
    fs = (struct ext2_fs *)kzalloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    fs->dev = dev;
    fs->sb = (struct ext2_super *)fs->sb_raw;
    if (block_read_bytes(dev, 1024, fs->sb_raw, 1024) < 0 || fs->sb->magic != EXT2_MAGIC) {
        kfree(fs);
        return -EINVAL;
    }
    if (fs->sb->feature_incompat & ~EXT2_INCOMPAT_FILETYPE) {
        kfree(fs);
        return -EINVAL;                                 /* ext3 journal recovery, ext4 extents ইত্যাদি */
    }
    if (fs->sb->feature_ro_compat & ~(EXT2_RO_SPARSE_SUPER | EXT2_RO_LARGE_FILE | EXT2_RO_BTREE_DIR)) {
        fs->read_only = 1;
    }

    fs->block_size = 1024u << fs->sb->log_block_size;
    fs->ptrs_per_block = fs->block_size / 4;
    fs->inode_size = fs->sb->rev_level >= 1 ? fs->sb->inode_size : 128;
    fs->groups = (fs->sb->blocks_count - fs->sb->first_data_block + fs->sb->blocks_per_group - 1) /
                 fs->sb->blocks_per_group;
    fs->gdt_block = fs->sb->first_data_block + 1;
    if (fs->inode_size < 128 || fs->block_size > 65536 || fs->groups == 0) {
        kfree(fs);
        return -EINVAL;
    }
    if (fs->sb->rev_level == 0) {
        fs->sb->first_ino = 11;
    }

    gdt_bytes = fs->groups * (uint32_t)sizeof(struct ext2_gd);
    fs->gdt = (struct ext2_gd *)kmalloc(gdt_bytes);
    if (!fs->gdt ||
        block_read_bytes(dev, (uint64_t)fs->gdt_block * fs->block_size, fs->gdt, gdt_bytes) < 0) {
        kfree(fs->gdt);
        kfree(fs);
        return -EIO;
    }

    r = get_vnode(fs, EXT2_ROOT_INO, NULL);
    if (!r || r->vn.type != VNODE_DIR) {
        kfree(fs->gdt);
        kfree(fs);
        return -EINVAL;
    }
    if (!fs->read_only) {
        fs->sb->mnt_count++;
        fs->sb->mtime = now32();
        write_super(fs);
    }
    *root = &r->vn;
    return 0;
}

static int ext2_statfs(struct vnode *root, struct vfs_statfs *out)
{
    struct ext2_fs *fs = EV(root)->fs;
    size_t n = strlen(fs->dev->name);
    out->block_size = fs->block_size;
    out->total_blocks = fs->sb->blocks_count;
    out->free_blocks = fs->sb->free_blocks_count;
    out->total_inodes = fs->sb->inodes_count;
    out->free_inodes = fs->sb->free_inodes_count;
    out->read_only = fs->read_only;
    if (n >= sizeof(out->source)) n = sizeof(out->source) - 1;
    memcpy(out->source, fs->dev->name, n);
    out->source[n] = '\0';
    return 0;
}

const struct filesystem ext2_filesystem = { "ext2", ext2_mount, ext2_statfs };
