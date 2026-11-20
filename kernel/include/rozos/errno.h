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
#define ENXIO         6      /* ধাপ ১৩ */
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
#define EPIPE        32      /* ধাপ ১৩: pipe-এর পাঠক নেই */
#define EROFS        30
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
/* ধাপ ৬ */
#define ESRCH         3
#define EINTR         4
#define E2BIG         7
#define ENOEXEC       8
#define ECHILD       10
#define EFAULT       14
#define ENOTTY       25
#define ERANGE       34
#define ETIMEDOUT   110
/* ধাপ ৯: নেটওয়ার্ক */
#define ENOTSOCK       88
#define EDESTADDRREQ   89
#define EMSGSIZE       90
#define EPROTOTYPE     91
#define ENOPROTOOPT    92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP     95
#define EAFNOSUPPORT   97
#define EADDRINUSE     98
#define EADDRNOTAVAIL  99
#define ENETDOWN      100
#define ENETUNREACH   101
#define ECONNABORTED  103
#define ECONNRESET    104
#define ENOBUFS       105
#define EISCONN       106
#define ENOTCONN      107
#define ECONNREFUSED  111
#define EHOSTUNREACH  113
#define EALREADY      114
#define EINPROGRESS   115

const char *kstrerror(int err);   /* err ধনাত্মক বা ঋণাত্মক দুটোই চলে */

#endif
