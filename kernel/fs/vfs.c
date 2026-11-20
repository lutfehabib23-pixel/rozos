/* rozOS — Virtual File System (rozos/vfs.h দেখুন)
 *
 * refcount-এর নিয়ম: lookup/resolve যে vnode ফেরত দেয় তার refcount বাড়ানো থাকে;
 * ব্যবহার শেষে vnode_unref() ডাকতে হয়। ফাইলসিস্টেম নিজে nlink রাখে — nlink আর
 * refcount দুটোই শূন্য হলে release() ডাকা হয় (Unix-এর মতো: খোলা ফাইল মুছলেও
 * যতক্ষণ খোলা আছে ততক্ষণ পড়া/লেখা যায়)।
 *
 * এখনো lock নেই: VFS শুধু সাধারণ কার্নেল কোড থেকে ডাকা হয়, ইন্টারাপ্ট থেকে নয়।
 * ধাপ ৬-এ মাল্টিটাস্কিং এলে এখানে lock যোগ হবে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/string.h>

#define VFS_MAX_FILESYSTEMS 8

struct file {
    int used;
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
static struct file files[VFS_MAX_FDS];

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

int vfs_lookup(const char *path, struct vnode **out)
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

int vfs_mount(const char *path, const char *fs_name, const void *arg)
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
    if (fd < 0 || fd >= VFS_MAX_FDS || !files[fd].used) {
        return NULL;
    }
    return &files[fd];
}

int vfs_open_count(void)
{
    int i, n = 0;
    for (i = 0; i < VFS_MAX_FDS; i++) {
        n += files[i].used;
    }
    return n;
}

int vfs_open(const char *path, int flags)
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

    for (fd = 0; fd < VFS_MAX_FDS; fd++) {
        if (!files[fd].used) {
            files[fd].used = 1;
            files[fd].flags = flags;
            files[fd].offset = 0;
            files[fd].vn = vn;
            files[fd].mnt = mnt;
            return fd;
        }
    }
    vnode_unref(vn);
    return -EMFILE;
}

int vfs_close(int fd)
{
    struct file *f = get_file(fd);
    if (!f) {
        return -EBADF;
    }
    vnode_unref(f->vn);
    memset(f, 0, sizeof(*f));
    return 0;
}

int64_t vfs_read(int fd, void *buf, size_t len)
{
    struct file *f = get_file(fd);
    int64_t n;

    if (!f) {
        return -EBADF;
    }
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        return -EBADF;
    }
    if (f->vn->type == VNODE_DIR) {
        return -EISDIR;
    }
    if (!f->vn->ops->read) {
        return -EINVAL;
    }
    n = f->vn->ops->read(f->vn, f->offset, buf, len);
    if (n > 0 && f->vn->type == VNODE_FILE) {
        f->offset += (uint64_t)n;
    }
    return n;
}

int64_t vfs_write(int fd, const void *buf, size_t len)
{
    struct file *f = get_file(fd);
    int64_t n;

    if (!f) {
        return -EBADF;
    }
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        return -EBADF;
    }
    if (!f->vn->ops->write) {
        return -EROFS;
    }
    if ((f->flags & O_APPEND) && f->vn->type == VNODE_FILE) {
        f->offset = f->vn->size;
    }
    n = f->vn->ops->write(f->vn, f->offset, buf, len);
    if (n > 0 && f->vn->type == VNODE_FILE) {
        f->offset += (uint64_t)n;
    }
    return n;
}

int64_t vfs_seek(int fd, int64_t offset, int whence)
{
    struct file *f = get_file(fd);
    int64_t base;

    if (!f) {
        return -EBADF;
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

int vfs_fstat(int fd, struct vfs_stat *st)
{
    struct file *f = get_file(fd);
    if (!f) {
        return -EBADF;
    }
    fill_stat(f->vn, f->mnt, st);
    return 0;
}

int vfs_readdir(int fd, struct vfs_dirent *out)
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

int vfs_stat(const char *path, struct vfs_stat *st)
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

int vfs_mkdir(const char *path)
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

int vfs_mkdir_p(const char *path)
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

int vfs_unlink(const char *path) { return remove_entry(path, 0); }
int vfs_rmdir(const char *path)  { return remove_entry(path, 1); }

int vfs_truncate(const char *path, uint64_t size)
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
