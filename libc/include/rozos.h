/* rozlibc — rozOS-এর নিজস্ব সুবিধা (POSIX নয়) */
#ifndef _ROZOS_H
#define _ROZOS_H

/* সরাসরি syscall: ফলাফল কাঁচা (ত্রুটি হলে -ERRNO, errno বসায় না) */
long __syscall6(long n, long a, long b, long c, long d, long e, long f);
long __syscall_int80(long n, long a, long b, long c);   /* int 0x80 পথ (পরীক্ষার জন্য) */
#define __syscall3(n, a, b, c) __syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

/* Linux-এর মতো: -ERRNO হলে errno বসিয়ে -1, নইলে মান */
long __syscall_ret(long r);

#define ROZOS_SYS_KMON 500

int  rozos_kmon(void);                 /* কার্নেল মনিটর; "exit" দিলে ফেরে */
long rozos_now_ms(void);               /* CLOCK_MONOTONIC, মিলিসেকেন্ড */

#endif
