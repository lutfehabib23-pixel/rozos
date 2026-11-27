/* rozlibc — fcntl.h (মান Linux-এর একই) */
#ifndef _FCNTL_H
#define _FCNTL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <_rozlibc.h>

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

/* ধাপ ১৬ক */
#define O_SYNC      0x101000
#define O_DSYNC     0x1000
#define O_NOFOLLOW  0x20000
#define O_NDELAY    O_NONBLOCK
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_GETLK     5
#define F_SETLK     6
#define F_SETLKW    7
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC  1
#define F_RDLCK     0
#define F_WRLCK     1
#define F_UNLCK     2
#define POSIX_FADV_NORMAL 0
struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};
int fcntl(int fd, int cmd, ...);
int openat(int dirfd, const char *path, int flags, ...);
int posix_fadvise(int fd, off_t off, off_t len, int advice);

int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);

#define AT_FDCWD (-100)        /* ধাপ ১১: *at() ফাংশনে "বর্তমান ডিরেক্টরি" */
/* ধাপ ১৯ছ: *at() ফাংশনের flag (Linux-এর মান; sbase ব্যবহার করে) */
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EACCESS          0x200
#define AT_SYMLINK_FOLLOW   0x400

#ifdef __cplusplus
}
#endif
#endif
