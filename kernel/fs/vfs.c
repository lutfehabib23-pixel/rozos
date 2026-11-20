/* rozOS — Virtual File System (rozos/vfs.h দেখুন)
 *
 * refcount-এর নিয়ম: lookup/resolve যে vnode ফেরত দেয় তার refcount বাড়ানো থাকে;
 * ব্যবহার শেষে vnode_unref() ডাকতে হয়। ফাইলসিস্টেম নিজে nlink রাখে — nlink আর
 * refcount দুটোই শূন্য হলে release() ডাকা হয় (Unix-এর মতো: খোলা ফাইল মুছলেও
 * যতক্ষণ খোলা আছে ততক্ষণ পড়া/লেখা যায়)।
 *
 * ধাপ ৬:
 *   - lock: একটা recursive kmutex (vfs_lock) সব public ফাংশন ঘিরে রাখে। ext2 ও
 *     tmpfs/devfs শুধু VFS দিয়েই ডাকা হয়, তাই তারাও এর ভেতরে সুরক্ষিত। ডিভাইস
 *     (VNODE_CHARDEV) পড়া/লেখার সময় lock ছাড়া হয় — কিবোর্ডের জন্য অপেক্ষারত
 *     প্রসেস যেন বাকি সবার ফাইল-অ্যাক্সেস আটকে না রাখে।
 *   - file descriptor: "open file description" (struct file: offset, flags, vnode)
 *     একটা গ্লোবাল পুলে, refcount সহ; প্রতিটি প্রসেসের নিজস্ব fd টেবিল (struct
 *     vfs_fdtable) সেগুলোর দিকে পয়েন্টার রাখে — fork/dup-এ একই description ভাগ হয়
 *     (Unix-এর মতো offset-ও)। কোন টেবিল চলবে তা proc.c-র hook বলে দেয়; কার্নেলের
 *     থ্রেডগুলো (kmon) kernel_fdt ব্যবহার করে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/heap.h>
#include <rozos/sched.h>

#define VFS_MAX_FILESYSTEMS 8

struct file {
    int used;
    int refs;                    /* কয়টা fd (সব টেবিল মিলিয়ে) এটাকে দেখায় */
    int flags;
    uint64_t offset;             /* ফাইলে অবস্থান; ডিরেক্টরিতে পরের এন্ট্রির index */
    struct vnode *vn;
    struct mount *mnt;
};

static const struct filesystem *filesystems[VFS_MAX_FILESYSTEMS];
static size_t fs_count;
static struct mount mounts[VFS_MAX_MOUNTS];
static size_t mount_count;
static struct mount *root_mount;
static struct file files[VFS_MAX_FILES];

struct vfs_fdtable {
    struct file *fd[VFS_MAX_FDS];
};

static struct vfs_fdtable kernel_fdt;
static struct vfs_fdtable *(*fdt_hook)(void);
static struct kmutex vfs_lock = KMUTEX_INIT("vfs");

#define LOCK()   kmutex_lock(&vfs_lock)
#define UNLOCK() kmutex_unlock(&vfs_lock)

static struct vfs_fdtable *cur_fdt(void)
{
    struct vfs_fdtable *t = fdt_hook ? fdt_hook() : NULL;
    return t ? t : &kernel_fdt;
}

/* ---------- refcount ---------- */

void vnode_ref(struct vnode *vn)
{
    if (vn) {
        vn->refcount++;
    }
}

void vnode_unref(struct vnode *vn)
{
    if (!vn || vn->refcount == 0) {
        return;
    }
    if (--vn->refcount == 0 && vn->nlink == 0 && vn->ops && vn->ops->release) {
        vn->ops->release(vn);
    }
}

/* ---------- সেটআপ ---------- */

int vfs_init(void)
{
    memset(files, 0, sizeof(files));
    memset(mounts, 0, sizeof(mounts));
    fs_count = 0;
    mount_count = 0;
    root_mount = NULL;
    return 0;
}

int vfs_register_filesystem(const struct filesystem *fs)
{
    if (fs_count >= VFS_MAX_FILESYSTEMS) {
        return -ENOMEM;
    }
    filesystems[fs_count++] = fs;
    return 0;
}

static const struct filesystem *find_fs(const char *name)
{
    size_t i;
    for (i = 0; i < fs_count; i++) {
        if (!strcmp(filesystems[i]->name, name)) {
            return filesystems[i];
        }
    }
    return NULL;
}

size_t vfs_mount_count(void) { return mount_count; }

const struct mount *vfs_mount_at(size_t index)
{
    return index < mount_count ? &mounts[index] : NULL;
}

/* ---------- পাথ সমাধান ---------- */

/* path → vnode (refcount বাড়ানো)। *mnt_out = যে mount-এর ভেতরে */
static int resolve(const char *path, struct vnode **out, struct mount **mnt_out)
{
    struct vnode *cur, *next;
    struct mount *m;
    char comp[VFS_NAME_MAX + 1];

    if (!path || path[0] != '/') {
        return -EINVAL;
    }
    if (!root_mount) {
        return -ENOENT;
    }
    if (strnlen(path, VFS_PATH_MAX) >= VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    m = root_mount;
    cur = m->root;
    vnode_ref(cur);

    while (*path) {
        size_t len = 0;
        int err;

        while (*path == '/') {
            path++;
        }
        if (!*path) {
            break;
        }
        while (path[len] && path[len] != '/') {
            len++;
        }
        if (len > VFS_NAME_MAX) {
            vnode_unref(cur);
            return -ENAMETOOLONG;
        }
        memcpy(comp, path, len);
        comp[len] = '\0';
        path += len;

        if (!strcmp(comp, ".")) {
            continue;
        }
        if (!strcmp(comp, "..")) {
            /* mount-এর root থেকে ওপরে: যে ডিরেক্টরির ওপর বসানো সেখানে ফিরে যাওয়া */
            while (cur == m->root && m->covered) {
                next = m->covered;
                vnode_ref(next);
                vnode_unref(cur);
                cur = next;
                m = cur->mount ? cur->mount : root_mount;
            }
            if (cur->parent) {
                next = cur->parent;
                vnode_ref(next);
                vnode_unref(cur);
                cur = next;
            }
            continue;
        }

        if (cur->type != VNODE_DIR) {
            vnode_unref(cur);
            return -ENOTDIR;
        }
        if (!cur->ops || !cur->ops->lookup) {
            vnode_unref(cur);
            return -ENOSYS;
        }
        err = cur->ops->lookup(cur, comp, &next);
        vnode_unref(cur);
        if (err < 0) {
            return err;
        }
        cur = next;
        if (!cur->mount) {
            cur->mount = m;               /* কোন mount-এর — ".." ও stat-এর জন্য */
        }

        /* mount point পার হওয়া */
        while (cur->covered_by) {
            m = cur->covered_by;
            next = m->root;
            vnode_ref(next);
            vnode_unref(cur);
            cur = next;
        }
    }

    *out = cur;
    if (mnt_out) {
        *mnt_out = m;
    }
    return 0;
}

static int do_vfs_lookup(const char *path, struct vnode **out)
{
    return resolve(path, out, NULL);
}

int vfs_split_path(const char *path, char *parent, size_t parent_size, char *name, size_t name_size)
{
    size_t len, end, start;

    if (!path || path[0] != '/') {
        return -EINVAL;
    }
    len = strnlen(path, VFS_PATH_MAX);
    if (len >= VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    end = len;
    while (end > 1 && path[end - 1] == '/') {
        end--;                            /* শেষের '/' বাদ */
    }
    start = end;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    if (end - start == 0) {
        return -EINVAL;                   /* "/" নিজে — শেষ অংশ নেই */
    }
    if (end - start >= name_size || end - start > VFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    memcpy(name, path + start, end - start);
    name[end - start] = '\0';

    /* parent = start-এর আগের অংশ (শেষের '/' বাদে), খালি হলে "/" */
    {
        size_t plen = start;
        while (plen > 1 && path[plen - 1] == '/') {
            plen--;
        }
        if (plen == 0) {
            plen = 1;
        }
        if (plen >= parent_size) {
            return -ENAMETOOLONG;
        }
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }
    return 0;
}

/* parent ডিরেক্টরি ও শেষ নাম একসাথে */
static int resolve_parent(const char *path, struct vnode **dir, char *name)
{
    char parent[VFS_PATH_MAX];
    int err = vfs_split_path(path, parent, sizeof(parent), name, VFS_NAME_MAX + 1);
    if (err < 0) {
        return err;
    }
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        return -EINVAL;
    }
    err = resolve(parent, dir, NULL);
    if (err < 0) {
        return err;
    }
    if ((*dir)->type != VNODE_DIR) {
        vnode_unref(*dir);
        return -ENOTDIR;
    }
    return 0;
}

/* ---------- mount ---------- */

static int do_vfs_mount(const char *path, const char *fs_name, const void *arg)
{
    const struct filesystem *fs = find_fs(fs_name);
    struct mount *m;
    struct vnode *root = NULL, *covered = NULL;
    struct mount *parent_mount = NULL;
    int err;

    if (!fs) {
        return -ENODEV;
    }
    if (mount_count >= VFS_MAX_MOUNTS) {
        return -ENOMEM;
    }
    if (strnlen(path, VFS_PATH_MAX) >= VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }

    if (!strcmp(path, "/") && !root_mount) {
        covered = NULL;
    } else {
        err = resolve(path, &covered, &parent_mount);
        if (err < 0) {
            return err;
        }
        if (covered->type != VNODE_DIR) {
            vnode_unref(covered);
            return -ENOTDIR;
        }
        if (covered->covered_by) {
            vnode_unref(covered);
            return -EBUSY;
        }
    }

    err = fs->mount(arg, &root);
    if (err < 0) {
        vnode_unref(covered);
        return err;
    }

    m = &mounts[mount_count++];
    strncpy(m->path, path, sizeof(m->path) - 1);
    m->fs = fs;
    m->root = root;
    m->covered = covered;                 /* covered-এর ref mount যতক্ষণ আছে ততক্ষণ থাকে */
    root->mount = m;
    root->parent = covered ? covered->parent : NULL;
    vnode_ref(root);
    (void)parent_mount;

    if (covered) {
        covered->covered_by = m;
    } else {
        root_mount = m;
    }
    return 0;
}

/* ---------- file descriptor ---------- */

static struct file *get_file(int fd)
{
    struct vfs_fdtable *t = cur_fdt();
    if (fd < 0 || fd >= VFS_MAX_FDS || !t->fd[fd]) {
        return NULL;
    }
    return t->fd[fd];
}

static void file_put(struct file *f)
{
    if (f && --f->refs <= 0) {
        vnode_unref(f->vn);
        memset(f, 0, sizeof(*f));
    }
}

/* নতুন description বানিয়ে চলমান টেবিলের সবচেয়ে ছোট খালি fd-তে বসানো */
static int install_file(int flags, struct vnode *vn, struct mount *mnt)
{
    struct vfs_fdtable *t = cur_fdt();
    int fd, i;

    for (fd = 0; fd < VFS_MAX_FDS && t->fd[fd]; fd++) {
    }
    if (fd == VFS_MAX_FDS) {
        return -EMFILE;
    }
    for (i = 0; i < VFS_MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].used = 1;
            files[i].refs = 1;
            files[i].flags = flags;
            files[i].offset = 0;
            files[i].vn = vn;
            files[i].mnt = mnt;
            t->fd[fd] = &files[i];
            return fd;
        }
    }
    return -ENFILE;
}

int vfs_open_count(void)
{
    int i, n = 0;
    LOCK();
    for (i = 0; i < VFS_MAX_FILES; i++) {
        n += files[i].used;
    }
    UNLOCK();
    return n;
}

/* ---------- fd টেবিল (ধাপ ৬) ---------- */

void vfs_set_fdtable_hook(struct vfs_fdtable *(*hook)(void))
{
    fdt_hook = hook;
}

struct vfs_fdtable *vfs_fdtable_create(void)
{
    struct vfs_fdtable *t = (struct vfs_fdtable *)kmalloc(sizeof(*t));
    if (t) {
        memset(t, 0, sizeof(*t));
    }
    return t;
}

struct vfs_fdtable *vfs_fdtable_clone(void)
{
    struct vfs_fdtable *src, *t = vfs_fdtable_create();
    int i;
    if (!t) {
        return NULL;
    }
    LOCK();
    src = cur_fdt();
    for (i = 0; i < VFS_MAX_FDS; i++) {
        if (src->fd[i]) {
            t->fd[i] = src->fd[i];
            t->fd[i]->refs++;
        }
    }
    UNLOCK();
    return t;
}

void vfs_fdtable_destroy(struct vfs_fdtable *t)
{
    int i;
    if (!t) {
        return;
    }
    LOCK();
    for (i = 0; i < VFS_MAX_FDS; i++) {
        if (t->fd[i]) {
            file_put(t->fd[i]);
            t->fd[i] = NULL;
        }
    }
    UNLOCK();
    kfree(t);
}

int vfs_fdtable_used(const struct vfs_fdtable *t)
{
    int i, n = 0;
    if (!t) {
        t = &kernel_fdt;
    }
    for (i = 0; i < VFS_MAX_FDS; i++) {
        n += t->fd[i] != NULL;
    }
    return n;
}

int vfs_dup(int oldfd)
{
    struct vfs_fdtable *t;
    struct file *f;
    int fd;

    LOCK();
    t = cur_fdt();
    f = get_file(oldfd);
    if (!f) {
        UNLOCK();
        return -EBADF;
    }
    for (fd = 0; fd < VFS_MAX_FDS && t->fd[fd]; fd++) {
    }
    if (fd == VFS_MAX_FDS) {
        UNLOCK();
        return -EMFILE;
    }
    t->fd[fd] = f;
    f->refs++;
    UNLOCK();
    return fd;
}

int vfs_dup2(int oldfd, int newfd)
{
    struct vfs_fdtable *t;
    struct file *f;

    if (newfd < 0 || newfd >= VFS_MAX_FDS) {
        return -EBADF;
    }
    LOCK();
    t = cur_fdt();
    f = get_file(oldfd);
    if (!f) {
        UNLOCK();
        return -EBADF;
    }
    if (oldfd != newfd) {
        if (t->fd[newfd]) {
            file_put(t->fd[newfd]);
        }
        t->fd[newfd] = f;
        f->refs++;
    }
    UNLOCK();
    return newfd;
}

static int do_vfs_open(const char *path, int flags)
{
    struct vnode *vn = NULL;
    struct mount *mnt = NULL;
    int err, fd, acc = flags & O_ACCMODE;

    err = resolve(path, &vn, &mnt);
    if (err == -ENOENT && (flags & O_CREAT)) {
        struct vnode *dir;
        char name[VFS_NAME_MAX + 1];
        err = resolve_parent(path, &dir, name);
        if (err < 0) {
            return err;
        }
        if (!dir->ops->create) {
            vnode_unref(dir);
            return -EROFS;
        }
        err = dir->ops->create(dir, name, VNODE_FILE, &vn);
        vnode_unref(dir);
        if (err < 0) {
            return err;
        }
        /* create-এর ref ছেড়ে পাথ দিয়ে আবার খোঁজা — এতে mount তথ্যও পাওয়া যায় */
        vnode_unref(vn);
        err = resolve(path, &vn, &mnt);
        if (err < 0) {
            return err;
        }
    } else if (err < 0) {
        return err;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_unref(vn);
        return -EEXIST;
    }

    if ((flags & O_DIRECTORY) && vn->type != VNODE_DIR) {
        vnode_unref(vn);
        return -ENOTDIR;
    }
    if (vn->type == VNODE_DIR && acc != O_RDONLY) {
        vnode_unref(vn);
        return -EISDIR;
    }
    if ((flags & O_TRUNC) && vn->type == VNODE_FILE && acc != O_RDONLY) {
        if (!vn->ops->truncate) {
            vnode_unref(vn);
            return -EROFS;
        }
        err = vn->ops->truncate(vn, 0);
        if (err < 0) {
            vnode_unref(vn);
            return err;
        }
    }

    fd = install_file(flags, vn, mnt);
    if (fd < 0) {
        vnode_unref(vn);
    }
    return fd;
}

static int do_vfs_close(int fd)
{
    struct file *f = get_file(fd);
    if (!f) {
        return -EBADF;
    }
    cur_fdt()->fd[fd] = NULL;
    file_put(f);
    return 0;
}

/* ডিভাইস পড়া/লেখা lock ছাড়া (ফাইল-description ধরে রেখে), বাকি সব lock-এর ভেতরে */
static int64_t do_rw(int fd, void *rbuf, const void *wbuf, size_t len)
{
    struct file *f;
    struct vnode *vn;
    uint64_t off;
    int64_t n;

    LOCK();
    f = get_file(fd);
    if (!f) {
        UNLOCK();
        return -EBADF;
    }
    vn = f->vn;
    if (rbuf) {
        if ((f->flags & O_ACCMODE) == O_WRONLY) { UNLOCK(); return -EBADF; }
        if (vn->type == VNODE_DIR)              { UNLOCK(); return -EISDIR; }
        if (!vn->ops->read)                     { UNLOCK(); return -EINVAL; }
    } else {
        if ((f->flags & O_ACCMODE) == O_RDONLY) { UNLOCK(); return -EBADF; }
        if (!vn->ops->write)                    { UNLOCK(); return -EROFS; }
        if ((f->flags & O_APPEND) && vn->type == VNODE_FILE) {
            f->offset = vn->size;
        }
    }
    off = f->offset;

    if (vn->type == VNODE_CHARDEV || vn->type == VNODE_SOCKET) {
        unsigned depth;
        f->refs++;                           /* অন্য থ্রেড close করলেও description থাকে */
        depth = kmutex_release_all(&vfs_lock);
        n = rbuf ? vn->ops->read(vn, off, rbuf, len) : vn->ops->write(vn, off, wbuf, len);
        kmutex_relock(&vfs_lock, depth);
        file_put(f);
        UNLOCK();
        return n;
    }

    n = rbuf ? vn->ops->read(vn, off, rbuf, len) : vn->ops->write(vn, off, wbuf, len);
    if (n > 0 && vn->type == VNODE_FILE) {
        f->offset = off + (uint64_t)n;
    }
    UNLOCK();
    return n;
}

int64_t vfs_read(int fd, void *buf, size_t len)
{
    if (!buf && len) {
        return -EINVAL;
    }
    return do_rw(fd, buf ? buf : (void *)"", NULL, len);
}

int64_t vfs_write(int fd, const void *buf, size_t len)
{
    return do_rw(fd, NULL, buf ? buf : "", len);
}

static int64_t do_vfs_seek(int fd, int64_t offset, int whence)
{
    struct file *f = get_file(fd);
    int64_t base;

    if (!f) {
        return -EBADF;
    }
    if (f->vn->type == VNODE_DIR) {
        /* ধাপ ৬: ডিরেক্টরিতে offset = পরের এন্ট্রির index (getdents64-এর জন্য) */
        if (whence == SEEK_CUR) offset += (int64_t)f->offset;
        else if (whence != SEEK_SET) return -EINVAL;
        if (offset < 0) return -EINVAL;
        f->offset = (uint64_t)offset;
        return offset;
    }
    if (f->vn->type != VNODE_FILE) {
        return -ESPIPE;
    }
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)f->offset; break;
    case SEEK_END: base = (int64_t)f->vn->size; break;
    default: return -EINVAL;
    }
    if (base + offset < 0) {
        return -EINVAL;
    }
    f->offset = (uint64_t)(base + offset);
    return (int64_t)f->offset;
}

static void fill_stat(struct vnode *vn, struct mount *mnt, struct vfs_stat *st)
{
    st->type  = vn->type;
    st->mode  = vn->mode;
    st->size  = vn->size;
    st->inode = vn->inode;
    st->mtime = vn->mtime;
    st->nlink = vn->nlink;
    st->fs_name = (mnt && mnt->fs) ? mnt->fs->name : "?";
}

static int do_vfs_fstat(int fd, struct vfs_stat *st)
{
    struct file *f = get_file(fd);
    if (!f) {
        return -EBADF;
    }
    fill_stat(f->vn, f->mnt, st);
    return 0;
}

static int do_vfs_readdir(int fd, struct vfs_dirent *out)
{
    struct file *f = get_file(fd);
    int r;

    if (!f) {
        return -EBADF;
    }
    if (f->vn->type != VNODE_DIR) {
        return -ENOTDIR;
    }
    if (!f->vn->ops->readdir) {
        return -ENOSYS;
    }
    r = f->vn->ops->readdir(f->vn, f->offset, out);
    if (r > 0) {
        f->offset++;
    }
    return r;
}

/* ---------- পাথ অপারেশন ---------- */

static int do_vfs_stat(const char *path, struct vfs_stat *st)
{
    struct vnode *vn;
    struct mount *mnt;
    int err = resolve(path, &vn, &mnt);
    if (err < 0) {
        return err;
    }
    fill_stat(vn, mnt, st);
    vnode_unref(vn);
    return 0;
}

static int do_vfs_mkdir(const char *path)
{
    struct vnode *dir, *child;
    char name[VFS_NAME_MAX + 1];
    int err = resolve_parent(path, &dir, name);

    if (err < 0) {
        return err;
    }
    if (dir->ops->lookup && dir->ops->lookup(dir, name, &child) == 0) {
        vnode_unref(child);
        vnode_unref(dir);
        return -EEXIST;
    }
    if (!dir->ops->create) {
        vnode_unref(dir);
        return -EROFS;
    }
    err = dir->ops->create(dir, name, VNODE_DIR, &child);
    vnode_unref(dir);
    if (err < 0) {
        return err;
    }
    vnode_unref(child);
    return 0;
}

static int do_vfs_mkdir_p(const char *path)
{
    char partial[VFS_PATH_MAX];
    size_t i, len = strnlen(path, VFS_PATH_MAX);
    int err;

    if (path[0] != '/') {
        return -EINVAL;
    }
    if (len >= VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    for (i = 1; i <= len; i++) {
        if (path[i] == '/' || path[i] == '\0') {
            struct vfs_stat st;
            memcpy(partial, path, i);
            partial[i] = '\0';
            if (i > 1 && partial[i - 1] == '/') {
                continue;
            }
            if (vfs_stat(partial, &st) == 0) {
                if (st.type != VNODE_DIR) {
                    return -ENOTDIR;
                }
                continue;
            }
            err = vfs_mkdir(partial);
            if (err < 0 && err != -EEXIST) {
                return err;
            }
        }
    }
    return 0;
}

static int remove_entry(const char *path, int want_dir)
{
    struct vnode *dir, *child;
    char name[VFS_NAME_MAX + 1];
    int err = resolve_parent(path, &dir, name);

    if (err < 0) {
        return err;
    }
    if (!dir->ops->lookup || (err = dir->ops->lookup(dir, name, &child)) < 0) {
        vnode_unref(dir);
        return err < 0 ? err : -ENOENT;
    }
    if (child->covered_by || (child->mount && child == child->mount->root)) {
        err = -EBUSY;
    } else if (want_dir && child->type != VNODE_DIR) {
        err = -ENOTDIR;
    } else if (!want_dir && child->type == VNODE_DIR) {
        err = -EISDIR;
    } else if (want_dir) {
        struct vfs_dirent de;
        if (child->ops->readdir && child->ops->readdir(child, 0, &de) > 0) {
            err = -ENOTEMPTY;
        }
    }
    vnode_unref(child);
    if (err == 0) {
        err = dir->ops->unlink ? dir->ops->unlink(dir, name) : -EROFS;
    }
    vnode_unref(dir);
    return err;
}

int vfs_unlink(const char *path)
{
    int r;
    LOCK();
    r = remove_entry(path, 0);
    UNLOCK();
    return r;
}

int vfs_rmdir(const char *path)
{
    int r;
    LOCK();
    r = remove_entry(path, 1);
    UNLOCK();
    return r;
}

static int do_vfs_statfs(const char *path, struct vfs_statfs *st)
{
    struct vnode *vn;
    struct mount *mnt = NULL;
    int err = resolve(path, &vn, &mnt);
    if (err < 0) {
        return err;
    }
    vnode_unref(vn);
    memset(st, 0, sizeof(*st));
    if (mnt && mnt->fs && mnt->fs->statfs) {
        return mnt->fs->statfs(mnt->root, st);
    }
    return 0;
}

static int do_vfs_truncate(const char *path, uint64_t size)
{
    struct vnode *vn;
    int err = resolve(path, &vn, NULL);
    if (err < 0) {
        return err;
    }
    if (vn->type != VNODE_FILE) {
        err = vn->type == VNODE_DIR ? -EISDIR : -EINVAL;
    } else {
        err = vn->ops->truncate ? vn->ops->truncate(vn, size) : -EROFS;
    }
    vnode_unref(vn);
    return err;
}

/* ---------- lock-সহ public ফাংশন (ধাপ ৬) ---------- */

int vfs_mount(const char *path, const char *fs_name, const void *arg)
{ int r; LOCK(); r = do_vfs_mount(path, fs_name, arg); UNLOCK(); return r; }

int vfs_open(const char *path, int flags)
{ int r; LOCK(); r = do_vfs_open(path, flags); UNLOCK(); return r; }

int vfs_close(int fd)
{ int r; LOCK(); r = do_vfs_close(fd); UNLOCK(); return r; }

int64_t vfs_seek(int fd, int64_t offset, int whence)
{ int64_t r; LOCK(); r = do_vfs_seek(fd, offset, whence); UNLOCK(); return r; }

int vfs_fstat(int fd, struct vfs_stat *st)
{ int r; LOCK(); r = do_vfs_fstat(fd, st); UNLOCK(); return r; }

int vfs_readdir(int fd, struct vfs_dirent *out)
{ int r; LOCK(); r = do_vfs_readdir(fd, out); UNLOCK(); return r; }

int vfs_stat(const char *path, struct vfs_stat *st)
{ int r; LOCK(); r = do_vfs_stat(path, st); UNLOCK(); return r; }

int vfs_mkdir(const char *path)
{ int r; LOCK(); r = do_vfs_mkdir(path); UNLOCK(); return r; }

int vfs_mkdir_p(const char *path)
{ int r; LOCK(); r = do_vfs_mkdir_p(path); UNLOCK(); return r; }

int vfs_truncate(const char *path, uint64_t size)
{ int r; LOCK(); r = do_vfs_truncate(path, size); UNLOCK(); return r; }

int vfs_statfs(const char *path, struct vfs_statfs *st)
{ int r; LOCK(); r = do_vfs_statfs(path, st); UNLOCK(); return r; }

int vfs_lookup(const char *path, struct vnode **out)
{ int r; LOCK(); r = do_vfs_lookup(path, out); UNLOCK(); return r; }

uint64_t vfs_lock_contended(void)
{
    return vfs_lock.contended;
}

/* ---------- ধাপ ৯: socket-এর মতো "ফাইলসিস্টেমহীন" vnode ---------- */

int vfs_install_vnode(struct vnode *vn, int flags)
{
    int fd;
    LOCK();
    fd = install_file(flags, vn, NULL);
    UNLOCK();
    return fd;
}

struct vnode *vfs_fd_vnode(int fd)
{
    struct file *f;
    struct vnode *vn = NULL;
    LOCK();
    f = get_file(fd);
    if (f) {
        vn = f->vn;
        vnode_ref(vn);
    }
    UNLOCK();
    return vn;
}
