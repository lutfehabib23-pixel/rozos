/* rozOS — Virtual File System
 *
 * Unix-ধাঁচের নকশা:
 *   - প্রতিটি ফাইল/ডিরেক্টরি/ডিভাইস একটা vnode; ফাইলসিস্টেম (tmpfs, devfs, পরে ext2)
 *     vnode_ops দিয়ে আসল কাজ করে।
 *   - mount: কোনো ডিরেক্টরি vnode-এর ওপর অন্য ফাইলসিস্টেমের root বসানো।
 *   - পাথ সবসময় পরম ('/' দিয়ে শুরু); ".", ".." ও পরপর '/' বোঝে।
 *   - file descriptor: প্রতিটি প্রসেসের নিজস্ব টেবিল (ধাপ ৬); কার্নেলের থ্রেডগুলো
 *     একটা কার্নেল টেবিল ব্যবহার করে — vfs_open/read/... API আগের মতোই।
 *   - সব ফাংশন একটা recursive mutex দিয়ে সুরক্ষিত (ধাপ ৬)।
 * সব ফাংশন ব্যর্থ হলে -ERRNO (rozos/errno.h) দেয়। */
#ifndef ROZOS_VFS_H
#define ROZOS_VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_NAME_MAX   255
#define VFS_PATH_MAX   1024
#define VFS_MAX_FDS    64       /* প্রতি fd টেবিলে */
#define VFS_MAX_FILES  256      /* সব মিলিয়ে খোলা file description (ধাপ ৬) */
#define VFS_MAX_MOUNTS 16

/* open() ফ্ল্যাগ — Linux-এর একই মান */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

enum vnode_type {
    VNODE_FILE = 1,
    VNODE_DIR  = 2,
    VNODE_CHARDEV = 3,
    VNODE_SOCKET = 4,            /* ধাপ ৯: নেটওয়ার্ক socket (kernel/net/socket.c) */
    VNODE_FIFO = 5               /* ধাপ ১৩: pipe-এর এক মুখ (kernel/fs/pipe.c) */
};

/* ধাপ ১৩: poll()-এর ঘটনা — Linux-এর একই মান */
#define VFS_POLLIN   0x0001      /* পড়ার মতো কিছু আছে (বা শেষ/EOF) */
#define VFS_POLLPRI  0x0002
#define VFS_POLLOUT  0x0004      /* আটকে না গিয়ে লেখা যাবে */
#define VFS_POLLERR  0x0008
#define VFS_POLLHUP  0x0010      /* অন্য মুখ বন্ধ */
#define VFS_POLLNVAL 0x0020      /* fd খোলা নেই */

struct vnode;
struct vfs_dirent;

struct vnode_ops {
    int64_t (*read)(struct vnode *vn, uint64_t offset, void *buf, size_t len);
    int64_t (*write)(struct vnode *vn, uint64_t offset, const void *buf, size_t len);
    int     (*truncate)(struct vnode *vn, uint64_t size);
    /* ডিরেক্টরি অপারেশন */
    int     (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int     (*create)(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out);
    int     (*unlink)(struct vnode *dir, const char *name);
    /* index-তম এন্ট্রি; ফেরত: 1 = পাওয়া গেছে, 0 = শেষ, <0 = ত্রুটি */
    int     (*readdir)(struct vnode *dir, uint64_t index, struct vfs_dirent *out);
    /* refcount শূন্য হলে (এবং ডিরেক্টরি থেকে সরানো থাকলে) মেমোরি ফেরত */
    void    (*release)(struct vnode *vn);
    /* ধাপ ১৩ (ঐচ্ছিক): এখনই কোন ঘটনা তৈরি (VFS_POLL*), আটকে থাকে না; NULL হলে
     * সাধারণ ফাইলের মতো সবসময় পড়া/লেখা যায় ধরা হয় */
    int     (*poll)(struct vnode *vn, int events);
    /* ধাপ ১৫ক (ঐচ্ছিক): olddir/oldname → newdir/newname, একই ফাইলসিস্টেমে। newname আগে
     * থাকলে ফাইল হলে বদলে যায়, ডিরেক্টরি হলে -EEXIST। NULL হলে vfs_rename -EXDEV দেয়
     * (mv তখন কপি করে) */
    int     (*rename)(struct vnode *olddir, const char *oldname, struct vnode *newdir, const char *newname);
    /* ধাপ ১৯ছ (ঐচ্ছিক): inode-এর flag বদলানো (এখন শুধু VN_IMMUTABLE) — ডিস্কে রাখে আর
     * vn->flags হালনাগাদ করে। NULL হলে ioctl FS_IOC_SETFLAGS -EOPNOTSUPP দেয় */
    int     (*setflags)(struct vnode *vn, uint32_t flags);
};

/* ধাপ ১৯ছ: vnode->flags (Linux-এর FS_*_FL-এর একই মান) */
#define VN_IMMUTABLE 0x00000010u   /* মোছা, নাম বদল, লেখা, truncate — সব -EPERM */

struct mount;

struct vnode {
    enum vnode_type type;
    uint32_t mode;               /* অনুমতি বিট (এখন তথ্য হিসেবে; যাচাই ধাপ ৬-এ) */
    uint64_t size;
    uint64_t inode;              /* ফাইলসিস্টেমের ভেতরে অনন্য নম্বর */
    uint64_t mtime;              /* শেষ পরিবর্তন: UNIX সময় (সেকেন্ড, UTC); 0 = অজানা (ধাপ ৫) */
    uint32_t refcount;
    uint32_t nlink;              /* কয়টা ডিরেক্টরি এন্ট্রি এটাকে দেখায় (0 = মুছে ফেলা) */
    const struct vnode_ops *ops;
    void *fs_data;               /* ফাইলসিস্টেমের নিজস্ব তথ্য */
    struct vnode *parent;        /* ".." — ডিরেক্টরির জন্য */
    struct mount *mount;         /* এই vnode কোন mount-এর */
    struct mount *covered_by;    /* এই ডিরেক্টরির ওপর কিছু mount করা থাকলে */
    uint32_t flags;              /* ধাপ ১৯ছ: VN_IMMUTABLE ইত্যাদি (ফাইলসিস্টেম ভরে) */
};

struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    enum vnode_type type;
    uint64_t inode;
    uint64_t size;
};

struct vfs_stat {
    enum vnode_type type;
    uint32_t mode;
    uint64_t size;
    uint64_t inode;
    uint64_t mtime;
    uint32_t nlink;
    const char *fs_name;         /* কোন ফাইলসিস্টেমে */
};

/* df-এর জন্য ফাইলসিস্টেমের জায়গার হিসাব */
struct vfs_statfs {
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t total_inodes;
    uint64_t free_inodes;
    int      read_only;
    char     source[32];         /* যেমন "sda" বা "ram" */
};

/* ফাইলসিস্টেম ড্রাইভার */
struct filesystem {
    const char *name;
    /* নতুন instance-এর root vnode বানায় (arg ফাইলসিস্টেম-নির্ভর) */
    int (*mount)(const void *arg, struct vnode **root);
    /* ঐচ্ছিক (ধাপ ৫): জায়গার হিসাব; NULL হলে শূন্য দেখায় */
    int (*statfs)(struct vnode *root, struct vfs_statfs *out);
    /* ঐচ্ছিক (ধাপ ১১খ): kmon-এর `disks`-এর জন্য বিস্তারিত বিবরণ kprintf দিয়ে */
    void (*info)(struct vnode *root);
    /* ঐচ্ছিক (ধাপ ১৫ক): umount-এর আগে — কোনো ফাইল খোলা থাকলে -EBUSY */
    int (*umount)(struct vnode *root);
};

struct mount {
    char path[VFS_PATH_MAX];
    const struct filesystem *fs;
    struct vnode *root;          /* mount করা ফাইলসিস্টেমের root */
    struct vnode *covered;       /* যে ডিরেক্টরির ওপর বসানো (/ হলে NULL) */
    /* ধাপ ১৫ক */
    char source[24];             /* ডিভাইস (sda2, nvmea1 ...) বা ফাইলসিস্টেমের নাম */
    int active;                  /* umount হলে 0 (ঘর আবার ব্যবহার হয়) */
    uint32_t base_ref;           /* mount-এর পর root-এর refcount — বেশি হলে কেউ ব্যবহার করছে */
};

/* ---------- সেটআপ ---------- */
int  vfs_init(void);
int  vfs_register_filesystem(const struct filesystem *fs);
int  vfs_mount(const char *path, const char *fs_name, const void *arg);
int  vfs_umount(const char *path);           /* ধাপ ১৫ক: -EBUSY = ব্যবহার হচ্ছে */
size_t vfs_mount_count(void);
const struct mount *vfs_mount_at(size_t index);

/* ---------- ফাইল descriptor ---------- */
int     vfs_open(const char *path, int flags);
int     vfs_close(int fd);
int64_t vfs_read(int fd, void *buf, size_t len);
int64_t vfs_write(int fd, const void *buf, size_t len);
int64_t vfs_seek(int fd, int64_t offset, int whence);
int     vfs_fstat(int fd, struct vfs_stat *st);
/* ডিরেক্টরি fd থেকে পরের এন্ট্রি: 1 = পাওয়া গেছে, 0 = শেষ, <0 = ত্রুটি
 * (ধাপ ৬: ডিরেক্টরিতে vfs_seek SEEK_SET/SEEK_CUR = এন্ট্রির index) */
int     vfs_readdir(int fd, struct vfs_dirent *out);
int     vfs_open_count(void);              /* সব মিলিয়ে খোলা description */

/* ---------- প্রতি-প্রসেস fd টেবিল (ধাপ ৬) ---------- */
struct vfs_fdtable;
/* চলমান থ্রেডের টেবিল যে দেয় (proc.c); NULL ফেরত দিলে কার্নেল টেবিল */
void vfs_set_fdtable_hook(struct vfs_fdtable *(*hook)(void));
struct vfs_fdtable *vfs_fdtable_create(void);
struct vfs_fdtable *vfs_fdtable_clone(void);          /* চলমান টেবিলের কপি (fork) */
void vfs_fdtable_destroy(struct vfs_fdtable *t);      /* সব fd বন্ধ করে মুক্ত */
int  vfs_fdtable_used(const struct vfs_fdtable *t);   /* NULL = কার্নেল টেবিল */
int  vfs_dup(int oldfd);
int  vfs_dup2(int oldfd, int newfd);
uint64_t vfs_lock_contended(void);
/* ধাপ ৯: ফাইলসিস্টেমের বাইরের vnode (যেমন socket) চলমান টেবিলে fd হিসেবে বসানো —
 * vnode-এর একটা ref fd নিয়ে নেয়; ফেরত fd বা -ERRNO */
int  vfs_install_vnode(struct vnode *vn, int flags);
/* fd-এর vnode (ref বাড়িয়ে; শেষে vnode_unref) — না থাকলে NULL */
struct vnode *vfs_fd_vnode(int fd);
/* ধাপ ১৯ছ: VN_IMMUTABLE পড়া/বসানো (ফাইলসিস্টেম সমর্থন না করলে -EOPNOTSUPP) */
int vfs_getflags(int fd, uint32_t *out);
int vfs_setflags(int fd, uint32_t flags);
/* ধাপ ১৩: fd-এর খোলার ফ্ল্যাগ (O_ACCMODE ইত্যাদি) — না থাকলে -EBADF */
int  vfs_fd_flags(int fd);
/* ধাপ ১৩: pipe — fds[0] পড়ার মুখ, fds[1] লেখার মুখ (kernel/fs/pipe.c) */
int  vfs_pipe(int fds[2]);
/* ধাপ ১৩: fd-এ এখন কোন ঘটনা তৈরি (VFS_POLL*); খোলা না থাকলে VFS_POLLNVAL */
int  vfs_poll_fd(int fd, int events);
/* ধাপ ১৩: খোলা fd-এর মাপ বদলানো (ftruncate) */
int  vfs_ftruncate(int fd, uint64_t size);

/* ---------- পাথ অপারেশন ---------- */
int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_mkdir(const char *path);
int vfs_mkdir_p(const char *path);            /* মাঝের সব ডিরেক্টরিও বানায় */
int vfs_unlink(const char *path);             /* ফাইল বা ডিভাইস */
int vfs_rmdir(const char *path);              /* খালি ডিরেক্টরি */
int vfs_rename(const char *oldpath, const char *newpath);   /* ধাপ ১৫ক */
int vfs_truncate(const char *path, uint64_t size);
int vfs_statfs(const char *path, struct vfs_statfs *st);   /* যে mount-এ path আছে তার হিসাব */

/* ফাইলসিস্টেম ড্রাইভারদের জন্য */
int  vfs_lookup(const char *path, struct vnode **out);   /* refcount বাড়ায় */
void vnode_ref(struct vnode *vn);
void vnode_unref(struct vnode *vn);

/* সহায়ক: পাথের শেষ অংশ আলাদা করা ("/a/b/c" → parent "/a/b", name "c") */
int vfs_split_path(const char *path, char *parent, size_t parent_size, char *name, size_t name_size);

#endif
