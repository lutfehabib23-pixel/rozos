/* rozlibc — sys/statfs.h (ধাপ ১৯চ): একটা file system-এর মাপ আর খালি জায়গা (Linux-এর মতো) */
#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int __val[2]; } fsid_t;

struct statfs {
    long          f_type;          /* EXT2_SUPER_MAGIC 0xEF53, MSDOS 0x4d44, TMPFS 0x01021994 ... */
    long          f_bsize;         /* ব্লকের মাপ (বাইট) */
    unsigned long f_blocks;        /* মোট ব্লক */
    unsigned long f_bfree;         /* খালি ব্লক */
    unsigned long f_bavail;        /* সাধারণ ব্যবহারকারীর জন্য খালি */
    unsigned long f_files, f_ffree;   /* inode: মোট, খালি */
    fsid_t        f_fsid;
    long          f_namelen;
    long          f_frsize;
    long          f_flags;
    long          f_spare[4];
};

int statfs(const char *path, struct statfs *buf);

#ifdef __cplusplus
}
#endif
#endif
