/* rozlibc — pthread.h (ধাপ ১৬খ-২)
 *
 * থ্রেড: clone(CLONE_VM|CLONE_THREAD|…) দিয়ে, প্রতিটার নিজের mmap করা স্ট্যাক (ডিফল্ট ৫১২ KiB)
 * আর TLS (FS base)। mutex/cond/rwlock/barrier/once/semaphore — সব futex-এর ওপর।
 * rozOS-এ এখন একটা CPU চলে: থ্রেডরা পালা করে চলে (preemptive), একসাথে নয়।
 * signal mask প্রসেসের সব থ্রেডের একটাই (pthread_sigmask = sigprocmask)। */
#ifndef _PTHREAD_H
#define _PTHREAD_H
#ifdef __cplusplus
extern "C" {
#endif

#include <_rozlibc.h>
#include <time.h>
#include <sched.h>

typedef unsigned long pthread_t;
typedef unsigned int  pthread_key_t;
typedef int           pthread_once_t;
typedef volatile int  pthread_spinlock_t;

typedef struct {
    size_t stacksize;
    size_t guardsize;
    void  *stackaddr;
    int    detachstate;
    int    inheritsched, schedpolicy, schedprio, scope;
} pthread_attr_t;

typedef struct {
    volatile int lock;               /* 0 খালি, 1 ধরা, 2 ধরা + অপেক্ষমাণ */
    volatile int owner;              /* tid (recursive/errorcheck-এর জন্য) */
    int count;
    int type;
    int __pad[6];
} pthread_mutex_t;

typedef struct { int type, pshared, robust, protocol; } pthread_mutexattr_t;

typedef struct {
    volatile int seq;                /* প্রতি signal/broadcast-এ বাড়ে (futex-এর ঠিকানা) */
    volatile int waiters;
    int clock;
    int __pad[5];
} pthread_cond_t;

typedef struct { int pshared, clock; } pthread_condattr_t;

typedef struct {
    pthread_mutex_t m;
    volatile int seq;
    int readers, writer, wwait;
} pthread_rwlock_t;

typedef struct { int pshared, kind; } pthread_rwlockattr_t;

typedef struct {
    pthread_mutex_t m;
    volatile int seq;
    unsigned count, waiting;
} pthread_barrier_t;

typedef struct { int pshared; } pthread_barrierattr_t;

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_STALLED    0
#define PTHREAD_MUTEX_ROBUST     1
#define PTHREAD_PRIO_NONE        0
#define PTHREAD_PRIO_INHERIT     1
#define PTHREAD_PRIO_PROTECT     2
#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1
#define PTHREAD_INHERIT_SCHED    0
#define PTHREAD_EXPLICIT_SCHED   1
#define PTHREAD_SCOPE_SYSTEM     0
#define PTHREAD_SCOPE_PROCESS    1
#define PTHREAD_PROCESS_PRIVATE  0
#define PTHREAD_PROCESS_SHARED   1
#define PTHREAD_CANCEL_ENABLE    0
#define PTHREAD_CANCEL_DISABLE   1
#define PTHREAD_CANCEL_DEFERRED  0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED         ((void *)-1)
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define PTHREAD_KEYS_MAX         128
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_STACK_MIN        16384
#define PTHREAD_THREADS_MAX      64

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0, { 0 } }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { 0, 0, 0, PTHREAD_MUTEX_RECURSIVE, { 0 } }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP { 0, 0, 0, PTHREAD_MUTEX_ERRORCHECK, { 0 } }
#define PTHREAD_COND_INITIALIZER  { 0, 0, 0, { 0 } }
#define PTHREAD_RWLOCK_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0 }
#define PTHREAD_ONCE_INIT 0

/* থ্রেড */
int       pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int       pthread_join(pthread_t t, void **result);
int       pthread_tryjoin_np(pthread_t t, void **result);
int       pthread_timedjoin_np(pthread_t t, void **result, const struct timespec *abstime);
int       pthread_detach(pthread_t t);
void      pthread_exit(void *result) __attribute__((noreturn));
pthread_t pthread_self(void);
int       pthread_equal(pthread_t a, pthread_t b);
int       pthread_once(pthread_once_t *once, void (*fn)(void));
int       pthread_cancel(pthread_t t);
void      pthread_testcancel(void);
int       pthread_setcancelstate(int state, int *old);
int       pthread_setcanceltype(int type, int *old);
int       pthread_kill(pthread_t t, int sig);
int       pthread_yield(void);
int       pthread_getconcurrency(void);
int       pthread_setconcurrency(int n);
int       pthread_setname_np(pthread_t t, const char *name);
int       pthread_getname_np(pthread_t t, char *name, size_t len);
int       pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p);
int       pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p);
int       pthread_getcpuclockid(pthread_t t, clockid_t *clk);
int       pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));
pid_t     pthread_gettid_np(pthread_t t);

/* বৈশিষ্ট্য */
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setdetachstate(pthread_attr_t *a, int s);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n);
int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n);
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *n);
int pthread_attr_setguardsize(pthread_attr_t *a, size_t n);
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n);
int pthread_attr_setscope(pthread_attr_t *a, int s);
int pthread_attr_getscope(const pthread_attr_t *a, int *s);
int pthread_attr_setinheritsched(pthread_attr_t *a, int s);
int pthread_attr_getinheritsched(const pthread_attr_t *a, int *s);
int pthread_attr_setschedpolicy(pthread_attr_t *a, int p);
int pthread_attr_getschedpolicy(const pthread_attr_t *a, int *p);
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p);
int pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p);
int pthread_getattr_np(pthread_t t, pthread_attr_t *a);

/* mutex */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_consistent(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *t);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int s);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *s);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *a, int r);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *a, int *r);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int p);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *a, int *p);

/* cond */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clk);
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clk);
int pthread_condattr_setpshared(pthread_condattr_t *a, int s);
int pthread_condattr_getpshared(const pthread_condattr_t *a, int *s);

/* rwlock */
int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);

/* spinlock, barrier */
int pthread_spin_init(pthread_spinlock_t *s, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);
int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *a);
int pthread_barrierattr_destroy(pthread_barrierattr_t *a);

/* থ্রেডের নিজস্ব ডেটা */
int   pthread_key_create(pthread_key_t *k, void (*destructor)(void *));
int   pthread_key_delete(pthread_key_t k);
void *pthread_getspecific(pthread_key_t k);
int   pthread_setspecific(pthread_key_t k, const void *v);

/* cleanup (ম্যাক্রো — একই ব্লকে push আর pop) */
struct __roz_cleanup_frame { void (*fn)(void *); void *arg; void *next; };
void __roz_cleanup_push(struct __roz_cleanup_frame *f, void (*fn)(void *), void *arg);
void __roz_cleanup_pop(struct __roz_cleanup_frame *f, int run);
#define pthread_cleanup_push(fn, arg) do { struct __roz_cleanup_frame __cf; __roz_cleanup_push(&__cf, (fn), (arg));
#define pthread_cleanup_pop(run) __roz_cleanup_pop(&__cf, (run)); } while (0)

#include <signal.h>

#ifdef __cplusplus
}
#endif
#endif
