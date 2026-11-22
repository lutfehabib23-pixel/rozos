/* rozlibc — থ্রেডের ভেতরের কাঠামো (ধাপ ১৬খ-২; শুধু libc/src-এর জন্য)
 *
 * প্রতিটা থ্রেডের একটা TCB (struct __pthread), FS base এর দিকে: %fs:0 = নিজের ঠিকানা
 * (x86-64-এর নিয়ম)। মূল থ্রেডের TCB স্ট্যাটিক, বাকিদেরটা তাদের mmap করা স্ট্যাকের মাথায়। */
#ifndef _ROZLIBC_PTHREAD_IMPL_H
#define _ROZLIBC_PTHREAD_IMPL_H

#include <stddef.h>
#include <stdint.h>

#define ROZ_KEYS_MAX 128

struct __pthread {
    struct __pthread *self;          /* %fs:0 */
    int errno_val;
    volatile int tid;                /* কার্নেল শেষে 0 করে futex wake দেয় (CLONE_CHILD_CLEARTID) */
    void *(*start)(void *);
    void *arg;
    void *result;
    void *map;                       /* mmap করা পুরো এলাকা (স্ট্যাক + TCB); মূল থ্রেডে NULL */
    size_t map_size;
    volatile int detached;           /* 1 = detach, 2 = শেষ হয়েছে আর detached (মুছতে হবে) */
    volatile int cancel;
    int cancel_disabled;
    struct __roz_cleanup *cleanup;
    void *tsd[ROZ_KEYS_MAX];
    struct __pthread *next_dead;     /* detached-মৃতদের তালিকা (পরে munmap) */
    int is_main;
};

struct __roz_cleanup {
    void (*fn)(void *);
    void *arg;
    struct __roz_cleanup *next;
};

extern int __roz_threaded;           /* একবার থ্রেড তৈরি হলে 1 — তখন থেকে lock লাগে */
struct __pthread *__roz_self(void);
void __roz_tls_init(void);

/* ভেতরের পুনঃপ্রবেশযোগ্য (recursive) lock — malloc আর stdio-র জন্য */
struct __roz_rlock {
    volatile int state;              /* 0 খালি, 1 ধরা, 2 ধরা + কেউ অপেক্ষায় */
    volatile int owner;
    int depth;
};
void __roz_rlock_lock(struct __roz_rlock *l);
void __roz_rlock_unlock(struct __roz_rlock *l);

long __roz_futex(volatile int *addr, int op, int val, const void *ts);
#define ROZ_FUTEX_WAIT 128           /* | FUTEX_PRIVATE_FLAG */
#define ROZ_FUTEX_WAKE 129

#endif
