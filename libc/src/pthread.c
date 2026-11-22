/* rozlibc — pthread, semaphore, sched (ধাপ ১৬খ-২)
 *
 * থ্রেড = কার্নেলের clone(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
 * CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID)।
 * এলাকা (mmap): [ফাঁকা পেজ][স্ট্যাক …][TCB (struct __pthread)] — TCB-ই FS base।
 * join: TCB-র tid কার্নেল 0 করে futex wake দেয় (CHILD_CLEARTID), তখন এলাকা munmap।
 * detached থ্রেড নিজের স্ট্যাকে থেকে নিজেকে মুছতে পারে না — "মৃতের তালিকা"-য় যায়,
 * পরের pthread_create/join সেগুলো munmap করে।
 *
 * mutex (Drepper-এর তিন-অবস্থা): 0 খালি, 1 ধরা, 2 ধরা + কেউ অপেক্ষায় (তখনই futex wake)।
 * cond: seq গোনা — wait আগে seq পড়ে mutex ছাড়ে, তারপর FUTEX_WAIT(seq, পুরনো);
 * signal/broadcast seq বাড়িয়ে wake। ভুয়া জাগা (spurious) POSIX-এ চলে, ডাকা কোড লুপে দেখে। */

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <sys/mman.h>
#include <rozos.h>
#include "pthread_impl.h"

#define SYS_sched_yield 24
#define SYS_gettid      186
#define SYS_futex       202
#define SYS_arch_prctl  158
#define SYS_set_tid_address 218
#define SYS_tgkill      234
#define ARCH_SET_FS     0x1002

#define CLONE_FLAGS (0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x100000 | 0x200000)
#define DEFAULT_STACK (512 * 1024)

long __roz_clone(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid, void *tls, int *ctid);

int __roz_threaded;
static struct __pthread main_tcb;
static int tls_ready;

/* ---------- TLS ও errno ---------- */

#undef errno
int errno;                           /* মূল থ্রেডের errno — পুরনো প্রোগ্রামও এই নামে পায় (আগে syscall.c-তে) */

void __roz_tls_init(void)
{
    if (tls_ready) return;
    main_tcb.self = &main_tcb;
    main_tcb.is_main = 1;
    main_tcb.tid = (int)__syscall6(SYS_set_tid_address, 0, 0, 0, 0, 0, 0);
    if (__syscall6(SYS_arch_prctl, ARCH_SET_FS, (long)&main_tcb, 0, 0, 0, 0) == 0) {
        tls_ready = 1;
    }
}

struct __pthread *__roz_self(void)
{
    struct __pthread *s;
    if (!tls_ready) return &main_tcb;
    __asm__ __volatile__("movq %%fs:0, %0" : "=r"(s));
    return s;
}

int *__errno_location(void)
{
    if (!tls_ready) return &errno;
    {
        struct __pthread *s = __roz_self();
        return s->is_main ? &errno : &s->errno_val;   /* মূল থ্রেড = পুরনো `errno` */
    }
}

pid_t gettid(void) { return (pid_t)__syscall6(SYS_gettid, 0, 0, 0, 0, 0, 0); }

/* fork-এর পরে সন্তানের tid নতুন (syscall.c-র fork ডাকে) */
void __roz_after_fork_child(void)
{
    if (tls_ready) __roz_self()->tid = gettid();
}

/* ---------- futex ও ভেতরের lock ---------- */

long __roz_futex(volatile int *addr, int op, int val, const void *ts)
{
    return __syscall6(SYS_futex, (long)addr, op, val, (long)ts, 0, 0);
}

static int cas(volatile int *p, int old, int nw)
{
    return __sync_val_compare_and_swap(p, old, nw);
}

static int xchg(volatile int *p, int v)
{
    return __sync_lock_test_and_set(p, v);
}

static void lock_word(volatile int *l)
{
    int c = cas(l, 0, 1);
    if (!c) return;
    if (c != 2) c = xchg(l, 2);
    while (c) {
        __roz_futex(l, ROZ_FUTEX_WAIT, 2, NULL);
        c = xchg(l, 2);
    }
}

static void unlock_word(volatile int *l)
{
    if (__sync_fetch_and_sub(l, 1) != 1) {
        *l = 0;
        __roz_futex(l, ROZ_FUTEX_WAKE, 1, NULL);
    }
}

void __roz_rlock_lock(struct __roz_rlock *l)
{
    int me;
    if (!__roz_threaded) return;
    me = __roz_self()->tid;
    if (l->owner == me && l->state) {
        l->depth++;
        return;
    }
    lock_word(&l->state);
    l->owner = me;
    l->depth = 1;
}

void __roz_rlock_unlock(struct __roz_rlock *l)
{
    if (!__roz_threaded || !l->state) return;
    if (--l->depth > 0) return;
    l->owner = 0;
    unlock_word(&l->state);
}

/* ---------- মৃত detached থ্রেডের এলাকা ---------- */

static volatile int dead_lock;
static struct __pthread *dead_list;

static void reap_dead(void)
{
    struct __pthread *d, *keep = NULL, *next;
    lock_word(&dead_lock);
    for (d = dead_list; d; d = next) {
        next = d->next_dead;
        if (d->tid == 0 && d->map) munmap(d->map, d->map_size);    /* সত্যিই শেষ */
        else { d->next_dead = keep; keep = d; }
    }
    dead_list = keep;
    unlock_word(&dead_lock);
}

/* ---------- থ্রেড ---------- */

static pthread_key_t nkeys;
static void (*key_dtor[ROZ_KEYS_MAX])(void *);
static int key_used[ROZ_KEYS_MAX];

static void run_tsd_destructors(struct __pthread *s)
{
    int round, i, again = 1;
    for (round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS && again; round++) {
        again = 0;
        for (i = 0; i < ROZ_KEYS_MAX; i++) {
            void *v = s->tsd[i];
            if (v && key_used[i] && key_dtor[i]) {
                s->tsd[i] = NULL;
                key_dtor[i](v);
                again = 1;
            }
        }
    }
}

void pthread_exit(void *result)
{
    struct __pthread *s = __roz_self();
    while (s->cleanup) {
        struct __roz_cleanup *c = s->cleanup;
        s->cleanup = c->next;
        c->fn(c->arg);
    }
    run_tsd_destructors(s);
    s->result = result;
    if (s->is_main) {
        /* মূল থ্রেড: বাকিরা চলতে থাকে; শেষ থ্রেড শেষ হলে প্রসেসও (exit কোড ০) */
        for (;;) __syscall6(60, 0, 0, 0, 0, 0, 0);
    }
    if (s->detached) {
        lock_word(&dead_lock);
        s->detached = 2;
        s->next_dead = dead_list;
        dead_list = s;
        unlock_word(&dead_lock);
    }
    for (;;) __syscall6(60, 0, 0, 0, 0, 0, 0);
}

static int thread_start(void *arg)
{
    struct __pthread *s = (struct __pthread *)arg;
    pthread_exit(s->start(s->arg));
    return 0;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    size_t stack = attr && attr->stacksize ? attr->stacksize : DEFAULT_STACK;
    size_t size;
    char *map;
    struct __pthread *s;
    long r;

    if (!fn) return EINVAL;
    reap_dead();
    stack = (stack + 4095) & ~(size_t)4095;
    size = stack + ((sizeof(struct __pthread) + 4095) & ~(size_t)4095);
    map = (char *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return EAGAIN;
    s = (struct __pthread *)(map + stack);
    memset(s, 0, sizeof(*s));
    s->self = s;
    s->start = fn;
    s->arg = arg;
    s->map = map;
    s->map_size = size;
    s->detached = attr && attr->detachstate == PTHREAD_CREATE_DETACHED;
    if (!tls_ready) __roz_tls_init();
    __roz_threaded = 1;
    r = __roz_clone(thread_start, map + stack, CLONE_FLAGS, s, (int *)&s->tid, s, (int *)&s->tid);
    if (r < 0) {
        munmap(map, size);
        return (int)-r == ENOMEM ? EAGAIN : (int)-r;
    }
    *out = (pthread_t)s;
    return 0;
}

static int join_wait(struct __pthread *t, const struct timespec *abstime)
{
    for (;;) {
        int tid = t->tid;
        long r;
        if (!tid) return 0;
        if (abstime) {
            struct timespec now, rel;
            clock_gettime(CLOCK_REALTIME, &now);
            rel.tv_sec = abstime->tv_sec - now.tv_sec;
            rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
            if (rel.tv_sec < 0) return ETIMEDOUT;
            r = __roz_futex(&t->tid, 0 /* FUTEX_WAIT (shared নয় — কার্নেলের CLEARTID-এর সাথে মেলে) */, tid, &rel);
            if (r == -ETIMEDOUT) return ETIMEDOUT;
        } else {
            __roz_futex(&t->tid, 0, tid, NULL);
        }
    }
}

static int join_finish(struct __pthread *t, void **result)
{
    if (result) *result = t->result;
    if (t->map) munmap(t->map, t->map_size);
    reap_dead();
    return 0;
}

int pthread_join(pthread_t th, void **result)
{
    struct __pthread *t = (struct __pthread *)th;
    if (!t || t == __roz_self()) return t ? EDEADLK : ESRCH;
    if (t->detached) return EINVAL;
    join_wait(t, NULL);
    return join_finish(t, result);
}

int pthread_tryjoin_np(pthread_t th, void **result)
{
    struct __pthread *t = (struct __pthread *)th;
    if (t->detached) return EINVAL;
    if (t->tid) return EBUSY;
    return join_finish(t, result);
}

int pthread_timedjoin_np(pthread_t th, void **result, const struct timespec *abstime)
{
    struct __pthread *t = (struct __pthread *)th;
    int r;
    if (t->detached) return EINVAL;
    r = join_wait(t, abstime);
    if (r) return r;
    return join_finish(t, result);
}

int pthread_detach(pthread_t th)
{
    struct __pthread *t = (struct __pthread *)th;
    if (t->detached) return EINVAL;
    if (!t->tid) return join_finish(t, NULL);        /* আগেই শেষ: এখনই মোছা */
    lock_word(&dead_lock);
    t->detached = 1;
    unlock_word(&dead_lock);
    /* এর মধ্যে শেষ হয়ে গেলে মৃতের তালিকায় নেই — নিজে যোগ করা */
    if (!t->tid && t->detached == 1) {
        lock_word(&dead_lock);
        t->detached = 2;
        t->next_dead = dead_list;
        dead_list = t;
        unlock_word(&dead_lock);
    }
    return 0;
}

pthread_t pthread_self(void)
{
    if (!tls_ready) __roz_tls_init();
    return (pthread_t)__roz_self();
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

pid_t pthread_gettid_np(pthread_t t) { return ((struct __pthread *)t)->tid; }

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    /* 0 = হয়নি, 1 = চলছে, 2 = শেষ */
    volatile int *o = (volatile int *)once;
    for (;;) {
        int v = *o;
        if (v == 2) return 0;
        if (v == 0 && cas(o, 0, 1) == 0) {
            fn();
            *o = 2;
            __roz_futex(o, ROZ_FUTEX_WAKE, INT_MAX, NULL);
            return 0;
        }
        __roz_futex(o, ROZ_FUTEX_WAIT, 1, NULL);
    }
}

int pthread_cancel(pthread_t th)
{
    ((struct __pthread *)th)->cancel = 1;            /* বিলম্বিত: pthread_testcancel-এ শেষ */
    return 0;
}

void pthread_testcancel(void)
{
    struct __pthread *s = __roz_self();
    if (s->cancel && !s->cancel_disabled) pthread_exit(PTHREAD_CANCELED);
}

int pthread_setcancelstate(int state, int *old)
{
    struct __pthread *s = __roz_self();
    if (old) *old = s->cancel_disabled;
    s->cancel_disabled = state == PTHREAD_CANCEL_DISABLE;
    return 0;
}

int pthread_setcanceltype(int type, int *old)
{
    if (old) *old = PTHREAD_CANCEL_DEFERRED;
    return type == PTHREAD_CANCEL_DEFERRED || type == PTHREAD_CANCEL_ASYNCHRONOUS ? 0 : EINVAL;
}

int pthread_kill(pthread_t th, int sig)
{
    long r = __syscall6(SYS_tgkill, getpid(), ((struct __pthread *)th)->tid, sig, 0, 0, 0);
    return r < 0 ? (int)-r : 0;
}

int pthread_yield(void) { return sched_yield(); }
int pthread_getconcurrency(void) { return 0; }
int pthread_setconcurrency(int n) { return n < 0 ? EINVAL : 0; }
int pthread_setname_np(pthread_t t, const char *name) { (void)t; (void)name; return 0; }

int pthread_getname_np(pthread_t t, char *name, size_t len)
{
    (void)t;
    if (len) { strncpy(name, "rozthread", len - 1); name[len - 1] = '\0'; }
    return 0;
}

int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p)
{
    (void)t;
    *policy = SCHED_OTHER;
    p->sched_priority = 0;
    return 0;
}

int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p)
{
    (void)t; (void)p;
    return policy == SCHED_OTHER ? 0 : EPERM;
}

int pthread_getcpuclockid(pthread_t t, clockid_t *clk) { (void)t; *clk = CLOCK_THREAD_CPUTIME_ID; return 0; }

/* atfork: fork-এর আগে/পরে ডাকা (syscall.c-র fork এগুলো চালায়) */
#define ATFORK_MAX 16
static void (*af_prepare[ATFORK_MAX])(void), (*af_parent[ATFORK_MAX])(void), (*af_child[ATFORK_MAX])(void);
static int af_n;

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    if (af_n >= ATFORK_MAX) return ENOMEM;
    af_prepare[af_n] = prepare;
    af_parent[af_n] = parent;
    af_child[af_n] = child;
    af_n++;
    return 0;
}

void __roz_atfork(int phase)
{
    int i;
    if (phase == 0) {
        for (i = af_n - 1; i >= 0; i--) if (af_prepare[i]) af_prepare[i]();
    } else {
        for (i = 0; i < af_n; i++) {
            void (*f)(void) = phase == 1 ? af_parent[i] : af_child[i];
            if (f) f();
        }
    }
}

void __roz_cleanup_push(struct __roz_cleanup_frame *f, void (*fn)(void *), void *arg)
{
    struct __pthread *s = __roz_self();
    f->fn = fn;
    f->arg = arg;
    f->next = s->cleanup;
    s->cleanup = (struct __roz_cleanup *)f;
}

void __roz_cleanup_pop(struct __roz_cleanup_frame *f, int run)
{
    struct __pthread *s = __roz_self();
    s->cleanup = (struct __roz_cleanup *)f->next;
    if (run) f->fn(f->arg);
}

/* ---------- বৈশিষ্ট্য ---------- */

int pthread_attr_init(pthread_attr_t *a) { memset(a, 0, sizeof(*a)); a->stacksize = DEFAULT_STACK; a->guardsize = 4096; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int s) { if (s != 0 && s != 1) return EINVAL; a->detachstate = s; return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s) { *s = a->detachstate; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n) { if (n < PTHREAD_STACK_MIN) return EINVAL; a->stacksize = n; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n) { *n = a->stacksize ? a->stacksize : DEFAULT_STACK; return 0; }
int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n)
{
    /* নিজের দেওয়া স্ট্যাক এখনো ব্যবহার করা হয় না — শুধু মাপটা নেওয়া হয় */
    if (n < PTHREAD_STACK_MIN) return EINVAL;
    a->stackaddr = addr;
    a->stacksize = n;
    return 0;
}
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *n) { *addr = a->stackaddr; *n = a->stacksize; return 0; }
int pthread_attr_setguardsize(pthread_attr_t *a, size_t n) { a->guardsize = n; return 0; }
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n) { *n = a->guardsize; return 0; }
int pthread_attr_setscope(pthread_attr_t *a, int s) { if (s != PTHREAD_SCOPE_SYSTEM) return ENOTSUP; a->scope = s; return 0; }
int pthread_attr_getscope(const pthread_attr_t *a, int *s) { *s = a->scope; return 0; }
int pthread_attr_setinheritsched(pthread_attr_t *a, int s) { a->inheritsched = s; return 0; }
int pthread_attr_getinheritsched(const pthread_attr_t *a, int *s) { *s = a->inheritsched; return 0; }
int pthread_attr_setschedpolicy(pthread_attr_t *a, int p) { a->schedpolicy = p; return 0; }
int pthread_attr_getschedpolicy(const pthread_attr_t *a, int *p) { *p = a->schedpolicy; return 0; }
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p) { a->schedprio = p->sched_priority; return 0; }
int pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p) { p->sched_priority = a->schedprio; return 0; }

int pthread_getattr_np(pthread_t t, pthread_attr_t *a)
{
    struct __pthread *s = (struct __pthread *)t;
    pthread_attr_init(a);
    a->detachstate = s->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
    if (s->map) {
        a->stackaddr = s->map;
        a->stacksize = (size_t)((char *)s - (char *)s->map);
    }
    return 0;
}

/* ---------- mutex ---------- */

int pthread_mutexattr_init(pthread_mutexattr_t *a) { memset(a, 0, sizeof(*a)); return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) { if (t < 0 || t > 2) return EINVAL; a->type = t; return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *t) { *t = a->type; return 0; }
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int s) { a->pshared = s; return 0; }
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *s) { *s = a->pshared; return 0; }
int pthread_mutexattr_setrobust(pthread_mutexattr_t *a, int r) { a->robust = r; return 0; }
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *a, int *r) { *r = a->robust; return 0; }
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int p) { a->protocol = p; return 0; }
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *a, int *p) { *p = a->protocol; return 0; }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    memset(m, 0, sizeof(*m));
    m->type = a ? a->type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) { return m->lock ? EBUSY : 0; }

static int my_tid(void)
{
    if (!tls_ready) __roz_tls_init();
    return __roz_self()->tid;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    int me = my_tid();
    if (m->type != PTHREAD_MUTEX_NORMAL && m->owner == me && m->lock) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) { m->count++; return 0; }
        return EBUSY;
    }
    if (cas(&m->lock, 0, 1) != 0) return EBUSY;
    m->owner = me;
    m->count = 1;
    return 0;
}

static int mutex_lock_until(pthread_mutex_t *m, const struct timespec *abstime)
{
    int me = my_tid(), c;
    if (m->type != PTHREAD_MUTEX_NORMAL && m->owner == me && m->lock) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            if (m->count == INT_MAX) return EAGAIN;
            m->count++;
            return 0;
        }
        return EDEADLK;
    }
    c = cas(&m->lock, 0, 1);
    if (c) {
        if (c != 2) c = xchg(&m->lock, 2);
        while (c) {
            if (abstime) {
                struct timespec now, rel;
                clock_gettime(CLOCK_REALTIME, &now);
                rel.tv_sec = abstime->tv_sec - now.tv_sec;
                rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
                if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
                if (rel.tv_sec < 0) return ETIMEDOUT;
                __roz_futex(&m->lock, ROZ_FUTEX_WAIT, 2, &rel);
            } else {
                __roz_futex(&m->lock, ROZ_FUTEX_WAIT, 2, NULL);
            }
            c = xchg(&m->lock, 2);
        }
    }
    m->owner = me;
    m->count = 1;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) { return mutex_lock_until(m, NULL); }
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime) { return mutex_lock_until(m, abstime); }

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (!m->lock || m->owner != my_tid()) return EPERM;
        if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count > 0) return 0;
    }
    m->owner = 0;
    m->count = 0;
    unlock_word(&m->lock);
    return 0;
}

int pthread_mutex_consistent(pthread_mutex_t *m) { (void)m; return 0; }

/* ---------- cond ---------- */

int pthread_condattr_init(pthread_condattr_t *a) { memset(a, 0, sizeof(*a)); return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c) { a->clock = c; return 0; }
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *c) { *c = a->clock; return 0; }
int pthread_condattr_setpshared(pthread_condattr_t *a, int s) { a->pshared = s; return 0; }
int pthread_condattr_getpshared(const pthread_condattr_t *a, int *s) { *s = a->pshared; return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    memset(c, 0, sizeof(*c));
    c->clock = a ? a->clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

static int cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    int seq = c->seq, r = 0, saved_count = m->count;
    __sync_fetch_and_add(&c->waiters, 1);
    /* mutex ছাড়া (recursive-এর গভীরতা মনে রেখে) */
    m->count = 1;
    pthread_mutex_unlock(m);
    if (abstime) {
        struct timespec now, rel;
        clock_gettime(c->clock, &now);
        rel.tv_sec = abstime->tv_sec - now.tv_sec;
        rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
        if (rel.tv_sec < 0) r = ETIMEDOUT;
        else if (__roz_futex(&c->seq, ROZ_FUTEX_WAIT, seq, &rel) == -ETIMEDOUT && c->seq == seq) r = ETIMEDOUT;
    } else {
        __roz_futex(&c->seq, ROZ_FUTEX_WAIT, seq, NULL);
    }
    __sync_fetch_and_sub(&c->waiters, 1);
    mutex_lock_until(m, NULL);
    m->count = saved_count;
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return cond_wait_until(c, m, NULL); }
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    return cond_wait_until(c, m, abstime);
}

int pthread_cond_signal(pthread_cond_t *c)
{
    __sync_fetch_and_add(&c->seq, 1);
    if (c->waiters) __roz_futex(&c->seq, ROZ_FUTEX_WAKE, 1, NULL);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    __sync_fetch_and_add(&c->seq, 1);
    if (c->waiters) __roz_futex(&c->seq, ROZ_FUTEX_WAKE, INT_MAX, NULL);
    return 0;
}

/* ---------- rwlock (mutex + seq; লেখকের অগ্রাধিকার) ---------- */

int pthread_rwlockattr_init(pthread_rwlockattr_t *a) { memset(a, 0, sizeof(*a)); return 0; }
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { (void)a; return 0; }
int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) { (void)a; memset(l, 0, sizeof(*l)); return 0; }
int pthread_rwlock_destroy(pthread_rwlock_t *l) { return l->readers || l->writer ? EBUSY : 0; }

static void rw_wait(pthread_rwlock_t *l)
{
    int seq = l->seq;
    pthread_mutex_unlock(&l->m);
    __roz_futex(&l->seq, ROZ_FUTEX_WAIT, seq, NULL);
    pthread_mutex_lock(&l->m);
}

static void rw_wake(pthread_rwlock_t *l)
{
    __sync_fetch_and_add(&l->seq, 1);
    __roz_futex(&l->seq, ROZ_FUTEX_WAKE, INT_MAX, NULL);
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    while (l->writer || l->wwait) rw_wait(l);
    l->readers++;
    pthread_mutex_unlock(&l->m);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    int r = 0;
    pthread_mutex_lock(&l->m);
    if (l->writer || l->wwait) r = EBUSY;
    else l->readers++;
    pthread_mutex_unlock(&l->m);
    return r;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    l->wwait++;
    while (l->writer || l->readers) rw_wait(l);
    l->wwait--;
    l->writer = 1;
    pthread_mutex_unlock(&l->m);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    int r = 0;
    pthread_mutex_lock(&l->m);
    if (l->writer || l->readers) r = EBUSY;
    else l->writer = 1;
    pthread_mutex_unlock(&l->m);
    return r;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->m);
    if (l->writer) l->writer = 0;
    else if (l->readers) l->readers--;
    rw_wake(l);
    pthread_mutex_unlock(&l->m);
    return 0;
}

/* ---------- spinlock, barrier ---------- */

int pthread_spin_init(pthread_spinlock_t *s, int pshared) { (void)pshared; *s = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t *s) { (void)s; return 0; }
int pthread_spin_lock(pthread_spinlock_t *s)
{
    /* একটা CPU: ঘুরে অপেক্ষা না করে অন্যকে চলতে দেওয়া */
    while (__sync_lock_test_and_set(s, 1)) sched_yield();
    return 0;
}
int pthread_spin_trylock(pthread_spinlock_t *s) { return __sync_lock_test_and_set(s, 1) ? EBUSY : 0; }
int pthread_spin_unlock(pthread_spinlock_t *s) { __sync_lock_release(s); return 0; }

int pthread_barrierattr_init(pthread_barrierattr_t *a) { a->pshared = 0; return 0; }
int pthread_barrierattr_destroy(pthread_barrierattr_t *a) { (void)a; return 0; }

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count)
{
    (void)a;
    if (count == 0) return EINVAL;
    memset(b, 0, sizeof(*b));
    b->count = count;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) { return b->waiting ? EBUSY : 0; }

int pthread_barrier_wait(pthread_barrier_t *b)
{
    int seq;
    pthread_mutex_lock(&b->m);
    seq = b->seq;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        __sync_fetch_and_add(&b->seq, 1);
        __roz_futex(&b->seq, ROZ_FUTEX_WAKE, INT_MAX, NULL);
        pthread_mutex_unlock(&b->m);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    pthread_mutex_unlock(&b->m);
    while (b->seq == seq) __roz_futex(&b->seq, ROZ_FUTEX_WAIT, seq, NULL);
    return 0;
}

/* ---------- থ্রেডের নিজস্ব ডেটা ---------- */

static volatile int key_lock;

int pthread_key_create(pthread_key_t *k, void (*destructor)(void *))
{
    unsigned i;
    lock_word(&key_lock);
    for (i = 0; i < ROZ_KEYS_MAX; i++) {
        if (!key_used[i]) {
            key_used[i] = 1;
            key_dtor[i] = destructor;
            if (i >= nkeys) nkeys = i + 1;
            unlock_word(&key_lock);
            *k = i;
            return 0;
        }
    }
    unlock_word(&key_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t k)
{
    if (k >= ROZ_KEYS_MAX || !key_used[k]) return EINVAL;
    key_used[k] = 0;
    key_dtor[k] = NULL;
    return 0;
}

void *pthread_getspecific(pthread_key_t k)
{
    if (k >= ROZ_KEYS_MAX) return NULL;
    if (!tls_ready) __roz_tls_init();
    return __roz_self()->tsd[k];
}

int pthread_setspecific(pthread_key_t k, const void *v)
{
    if (k >= ROZ_KEYS_MAX || !key_used[k]) return EINVAL;
    if (!tls_ready) __roz_tls_init();
    __roz_self()->tsd[k] = (void *)v;
    return 0;
}

/* ---------- semaphore ---------- */

int sem_init(sem_t *s, int pshared, unsigned value)
{
    (void)pshared;
    if (value > SEM_VALUE_MAX) { (*__errno_location()) = EINVAL; return -1; }
    memset(s, 0, sizeof(*s));
    s->value = (int)value;
    return 0;
}

int sem_destroy(sem_t *s) { (void)s; return 0; }

int sem_trywait(sem_t *s)
{
    int v;
    while ((v = s->value) > 0) {
        if (cas(&s->value, v, v - 1) == v) return 0;
    }
    (*__errno_location()) = EAGAIN;
    return -1;
}

static int sem_wait_until(sem_t *s, const struct timespec *abstime)
{
    for (;;) {
        long r;
        if (sem_trywait(s) == 0) return 0;
        __sync_fetch_and_add(&s->waiters, 1);
        if (abstime) {
            struct timespec now, rel;
            clock_gettime(CLOCK_REALTIME, &now);
            rel.tv_sec = abstime->tv_sec - now.tv_sec;
            rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
            if (rel.tv_sec < 0) {
                __sync_fetch_and_sub(&s->waiters, 1);
                (*__errno_location()) = ETIMEDOUT;
                return -1;
            }
            r = __roz_futex(&s->value, ROZ_FUTEX_WAIT, 0, &rel);
        } else {
            r = __roz_futex(&s->value, ROZ_FUTEX_WAIT, 0, NULL);
        }
        __sync_fetch_and_sub(&s->waiters, 1);
        if (r == -EINTR) { (*__errno_location()) = EINTR; return -1; }
    }
}

int sem_wait(sem_t *s) { return sem_wait_until(s, NULL); }
int sem_timedwait(sem_t *s, const struct timespec *abstime) { return sem_wait_until(s, abstime); }

int sem_post(sem_t *s)
{
    if (s->value == SEM_VALUE_MAX) { (*__errno_location()) = EOVERFLOW; return -1; }
    __sync_fetch_and_add(&s->value, 1);
    if (s->waiters) __roz_futex(&s->value, ROZ_FUTEX_WAKE, 1, NULL);
    return 0;
}

int sem_getvalue(sem_t *s, int *value) { *value = s->value; return 0; }
sem_t *sem_open(const char *name, int oflag, ...) { (void)name; (void)oflag; (*__errno_location()) = ENOSYS; return SEM_FAILED; }
int sem_close(sem_t *s) { (void)s; (*__errno_location()) = ENOSYS; return -1; }
int sem_unlink(const char *name) { (void)name; (*__errno_location()) = ENOSYS; return -1; }

/* ---------- sched ---------- */

int sched_yield(void) { return (int)__syscall_ret(__syscall6(SYS_sched_yield, 0, 0, 0, 0, 0, 0)); }
int sched_get_priority_max(int policy) { return policy == SCHED_FIFO || policy == SCHED_RR ? 99 : 0; }
int sched_get_priority_min(int policy) { return policy == SCHED_FIFO || policy == SCHED_RR ? 1 : 0; }
int sched_getparam(pid_t pid, struct sched_param *p) { (void)pid; p->sched_priority = 0; return 0; }
int sched_setparam(pid_t pid, const struct sched_param *p) { (void)pid; (void)p; return 0; }
int sched_getscheduler(pid_t pid) { (void)pid; return SCHED_OTHER; }
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *p)
{
    (void)pid; (void)p;
    if (policy != SCHED_OTHER) { (*__errno_location()) = EPERM; return -1; }
    return 0;
}
int sched_rr_get_interval(pid_t pid, struct timespec *t) { (void)pid; t->tv_sec = 0; t->tv_nsec = 10000000; return 0; }

int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
    (void)pid;
    memset(set, 0, size < sizeof(*set) ? size : sizeof(*set));
    CPU_SET(0, set);
    return 0;
}

int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set) { (void)pid; (void)size; (void)set; return 0; }
int sched_getcpu(void) { return 0; }

int __roz_cpu_count(const cpu_set_t *s)
{
    int i, n = 0;
    for (i = 0; i < CPU_SETSIZE; i++) n += (int)CPU_ISSET(i, s);
    return n;
}
