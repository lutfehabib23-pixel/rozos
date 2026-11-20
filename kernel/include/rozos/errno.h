/* rozOS — ত্রুটি কোড (Linux/POSIX-এর একই সংখ্যা, যাতে ধাপ ৭-এর libc সহজে মেলে)
 * কার্নেল ফাংশন ব্যর্থ হলে -ERRNO ফেরত দেয় (যেমন -ENOENT)। */
#ifndef ROZOS_ERRNO_H
#define ROZOS_ERRNO_H

#define EPERM         1
#define ENOENT        2
#define EIO           5
#define EBADF         9
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

const char *kstrerror(int err);   /* err ধনাত্মক বা ঋণাত্মক দুটোই চলে */

#endif
