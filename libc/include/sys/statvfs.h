/* rozlibc — sys/statvfs.h (ধাপ ১৯চ): POSIX-এর statvfs — ডিস্কের মাপ আর খালি জায়গা */
#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct statvfs {
    unsigned long f_bsize;         /* ব্লকের মাপ */
    unsigned long f_frsize;        /* f_blocks ইত্যাদি এই মাপের এককে */
    fsblkcnt_t    f_blocks, f_bfree, f_bavail;
    fsfilcnt_t    f_files, f_ffree, f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;          /* ST_RDONLY */
    unsigned long f_namemax;
};

#define ST_RDONLY 1
#define ST_NOSUID 2

int statvfs(const char *path, struct statvfs *buf);

#endif
