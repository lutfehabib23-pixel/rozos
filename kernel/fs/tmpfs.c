/* rozOS — tmpfs: সম্পূর্ণ RAM-এ থাকা লেখা-যোগ্য ফাইলসিস্টেম
 *
 * ধাপ ৪-এ এটাই root ("/"): বুটের সময় initrd (tar) খুলে এর ভেতরে কপি হয়।
 * ধাপ ৫-এ ডিস্কের ext2 "/"-এ এলে tmpfs "/tmp"-এ সরে যাবে।
 *
 * ডিরেক্টরি = নাম-vnode জোড়ার linked list; ফাইল = kmalloc করা বাফার
 * (krealloc দিয়ে বাড়ে)। সব মেমোরি kernel heap থেকে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/clock.h>
#include <rozos/pmm.h>

#define TMPFS_MAX_FILE_SIZE (256ULL * 1024 * 1024)

struct tmpfs_dirent {
    char *name;
    struct vnode *vn;
    struct tmpfs_dirent *next;
};

struct tmpfs_node {
    struct vnode vn;                     /* প্রথম সদস্য — vnode* থেকে সরাসরি cast */
    uint8_t *data;                       /* ফাইল */
    uint64_t capacity;
    struct tmpfs_dirent *entries;        /* ডিরেক্টরি */
    uint64_t entry_count;
};

static uint64_t next_inode = 1;
static const struct vnode_ops tmpfs_ops;

static struct tmpfs_node *TN(struct vnode *vn)
{
    return (struct tmpfs_node *)vn;
}

static struct vnode *new_node(enum vnode_type type, struct vnode *parent)
{
    struct tmpfs_node *n = (struct tmpfs_node *)kzalloc(sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->vn.type = type;
    n->vn.mode = (type == VNODE_DIR) ? 0755 : 0644;
    n->vn.inode = next_inode++;
    n->vn.mtime = (uint64_t)clock_unix_time();
    n->vn.ops = &tmpfs_ops;
    n->vn.fs_data = n;
    n->vn.parent = (type == VNODE_DIR) ? parent : NULL;
    n->vn.mount = parent ? parent->mount : NULL;
    return &n->vn;
}

/* ---------- ফাইল ---------- */

static int64_t tmpfs_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct tmpfs_node *n = TN(vn);
    if (off >= vn->size) {
        return 0;
    }
    if (len > vn->size - off) {
        len = (size_t)(vn->size - off);
    }
    memcpy(buf, n->data + off, len);
    return (int64_t)len;
}

static int ensure_capacity(struct tmpfs_node *n, uint64_t need)
{
    uint64_t cap;
    uint8_t *p;
    if (need <= n->capacity) {
        return 0;
    }
    if (need > TMPFS_MAX_FILE_SIZE) {
        return -EFBIG;
    }
    cap = n->capacity ? n->capacity : 64;
    while (cap < need) {
        cap *= 2;
    }
    if (cap > TMPFS_MAX_FILE_SIZE) {
        cap = TMPFS_MAX_FILE_SIZE;
    }
    p = (uint8_t *)krealloc(n->data, (size_t)cap);
    if (!p) {
        return -ENOSPC;
    }
    n->data = p;
    n->capacity = cap;
    return 0;
}

static int64_t tmpfs_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct tmpfs_node *n = TN(vn);
    int err;

    if (len == 0) {
        return 0;
    }
    err = ensure_capacity(n, off + len);
    if (err < 0) {
        return err;
    }
    if (off > vn->size) {
        memset(n->data + vn->size, 0, (size_t)(off - vn->size));   /* ফাঁকা অংশ শূন্য */
    }
    memcpy(n->data + off, buf, len);
    if (off + len > vn->size) {
        vn->size = off + len;
    }
    vn->mtime = (uint64_t)clock_unix_time();
    return (int64_t)len;
}

static int tmpfs_truncate(struct vnode *vn, uint64_t size)
{
    struct tmpfs_node *n = TN(vn);
    int err;
    if (size > vn->size) {
        err = ensure_capacity(n, size);
        if (err < 0) {
            return err;
        }
        memset(n->data + vn->size, 0, (size_t)(size - vn->size));
    }
    vn->size = size;
    vn->mtime = (uint64_t)clock_unix_time();
    return 0;
}

/* ---------- ডিরেক্টরি ---------- */

static int tmpfs_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct tmpfs_dirent *e;
    for (e = TN(dir)->entries; e; e = e->next) {
        if (!strcmp(e->name, name)) {
            vnode_ref(e->vn);
            *out = e->vn;
            return 0;
        }
    }
    return -ENOENT;
}

static int tmpfs_create(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out)
{
    struct tmpfs_node *d = TN(dir);
    struct tmpfs_dirent *e, **tail;
    struct vnode *vn;
    size_t len = strlen(name);

    if (len == 0 || len > VFS_NAME_MAX || strchr(name, '/')) {
        return -EINVAL;
    }
    for (e = d->entries; e; e = e->next) {
        if (!strcmp(e->name, name)) {
            return -EEXIST;
        }
    }
    if (type != VNODE_FILE && type != VNODE_DIR) {
        return -EINVAL;
    }
    vn = new_node(type, dir);
    e = (struct tmpfs_dirent *)kmalloc(sizeof(*e));
    if (!vn || !e) {
        kfree(vn);
        kfree(e);
        return -ENOSPC;
    }
    e->name = (char *)kmalloc(len + 1);
    if (!e->name) {
        kfree(vn);
        kfree(e);
        return -ENOSPC;
    }
    memcpy(e->name, name, len + 1);
    e->vn = vn;
    e->next = NULL;
    /* তালিকার শেষে যোগ — ls-এ তৈরির ক্রমে দেখায় */
    for (tail = &d->entries; *tail; tail = &(*tail)->next) {
    }
    *tail = e;
    d->entry_count++;
    vn->nlink = 1;
    vn->refcount = 1;                     /* ফেরত দেওয়া ref */
    dir->size = d->entry_count;
    dir->mtime = (uint64_t)clock_unix_time();
    *out = vn;
    return 0;
}

static int tmpfs_unlink(struct vnode *dir, const char *name)
{
    struct tmpfs_node *d = TN(dir);
    struct tmpfs_dirent **pp, *e;

    for (pp = &d->entries; (e = *pp) != NULL; pp = &e->next) {
        if (!strcmp(e->name, name)) {
            struct vnode *vn = e->vn;
            *pp = e->next;
            kfree(e->name);
            kfree(e);
            d->entry_count--;
            dir->size = d->entry_count;
            dir->mtime = (uint64_t)clock_unix_time();
            vn->nlink = 0;
            if (vn->refcount == 0 && vn->ops->release) {
                vn->ops->release(vn);    /* কেউ খুলে রাখেনি — এখনই মুক্ত */
            }
            return 0;
        }
    }
    return -ENOENT;
}

static int tmpfs_readdir(struct vnode *dir, uint64_t index, struct vfs_dirent *out)
{
    struct tmpfs_dirent *e = TN(dir)->entries;
    while (e && index--) {
        e = e->next;
    }
    if (!e) {
        return 0;
    }
    strncpy(out->name, e->name, VFS_NAME_MAX);
    out->name[VFS_NAME_MAX] = '\0';
    out->type = e->vn->type;
    out->inode = e->vn->inode;
    out->size = e->vn->size;
    return 1;
}

/* ধাপ ১৫ক: শুধু এন্ট্রি এক তালিকা থেকে অন্য তালিকায় (নাম বদলে) */
static int tmpfs_rename(struct vnode *od, const char *oname, struct vnode *nd, const char *nname)
{
    struct tmpfs_node *o = TN(od), *n = TN(nd);
    struct tmpfs_dirent **pp, *e, *t, **tail;
    size_t len = strlen(nname);
    char *nm;

    if (len == 0 || len > VFS_NAME_MAX || strchr(nname, '/')) {
        return -EINVAL;
    }
    for (pp = &o->entries; (e = *pp) != NULL; pp = &e->next) {
        if (!strcmp(e->name, oname)) {
            break;
        }
    }
    if (!e) {
        return -ENOENT;
    }
    for (t = n->entries; t; t = t->next) {
        if (!strcmp(t->name, nname)) {
            if (t->vn == e->vn) {
                return 0;
            }
            if (t->vn->type == VNODE_DIR) {
                return -EEXIST;
            }
            break;
        }
    }
    nm = (char *)kmalloc(len + 1);
    if (!nm) {
        return -ENOSPC;
    }
    memcpy(nm, nname, len + 1);
    if (t) {
        tmpfs_unlink(nd, nname);                 /* আগের ফাইল বদলে যায় */
    }
    for (pp = &o->entries; *pp != e; pp = &(*pp)->next) {
    }
    *pp = e->next;                               /* পুরোনো তালিকা থেকে খোলা */
    o->entry_count--;
    od->size = o->entry_count;
    kfree(e->name);
    e->name = nm;
    e->next = NULL;
    for (tail = &n->entries; *tail; tail = &(*tail)->next) {
    }
    *tail = e;
    n->entry_count++;
    nd->size = n->entry_count;
    if (e->vn->type == VNODE_DIR) {
        e->vn->parent = nd;
    }
    od->mtime = nd->mtime = (uint64_t)clock_unix_time();
    return 0;
}

static void tmpfs_release(struct vnode *vn)
{
    struct tmpfs_node *n = TN(vn);
    kfree(n->data);
    kfree(n);                             /* ডিরেক্টরি খালি না হলে মোছা যায় না (VFS যাচাই করে) */
}

/* ধাপ ১৯ছ: flag শুধু মেমোরিতে (tmpfs-এ সবকিছুই তাই) */
static int tmpfs_setflags(struct vnode *vn, uint32_t flags)
{
    vn->flags = flags & VN_IMMUTABLE;
    return 0;
}

static const struct vnode_ops tmpfs_ops = {
    tmpfs_read,
    tmpfs_write,
    tmpfs_truncate,
    tmpfs_lookup,
    tmpfs_create,
    tmpfs_unlink,
    tmpfs_readdir,
    tmpfs_release,
    NULL,
    tmpfs_rename,                         /* ধাপ ১৫ক */
    tmpfs_setflags                        /* ধাপ ১৯ছ */
};

/* ---------- mount ---------- */

static int tmpfs_mount(const void *arg, struct vnode **root)
{
    struct vnode *vn;
    (void)arg;
    vn = new_node(VNODE_DIR, NULL);
    if (!vn) {
        return -ENOMEM;
    }
    vn->nlink = 1;                        /* root কখনো মুছবে না */
    *root = vn;
    return 0;
}

/* tmpfs-এর জায়গা = সিস্টেমের খালি RAM (যা kmalloc-এর মাধ্যমে পাওয়া যায়) */
static int tmpfs_statfs(struct vnode *r, struct vfs_statfs *out)
{
    struct pmm_stats ps;
    (void)r;
    pmm_get_stats(&ps);
    out->block_size = PMM_PAGE_SIZE;
    out->total_blocks = ps.usable_pages;
    out->free_blocks = ps.free_pages;
    out->total_inodes = 0;
    out->free_inodes = 0;
    out->source[0] = 'r'; out->source[1] = 'a'; out->source[2] = 'm'; out->source[3] = '\0';
    return 0;
}

const struct filesystem tmpfs_filesystem = { "tmpfs", tmpfs_mount, tmpfs_statfs, NULL, NULL, NULL };
