/* rozOS — থ্রেড ও শিডিউলার (ধাপ ৬)
 *
 * নকশা:
 *   - round-robin, প্রতি থ্রেড 10 ms (SCHED_SLICE_MS); টাইমারের tick hook দিয়ে গোনা।
 *   - preemption শুধু ইউজার-মোডে: ইন্টারাপ্ট বা syscall শেষে ইউজারে ফেরার মুহূর্তে
 *     সময় ফুরালে অন্য থ্রেডে যায়। কার্নেল কোড নিজে থেকে না ছাড়লে (sleep, block,
 *     sched_relax) মাঝপথে থামে না — তাই heap/PMM/VMM-এ lock লাগে না (একক CPU)।
 *   - কার্নেল-থ্রেডের proc = NULL; ইউজার থ্রেডের proc = তার প্রসেস (rozos/proc.h)।
 *   - প্রতিটি থ্রেডের নিজস্ব কার্নেল স্ট্যাক (32 KiB) ও FPU/SSE অবস্থা (fxsave)।
 *   - বুটের থ্রেড ("kmain", tid 0) Limine-এর স্ট্যাকেই চলে; idle থ্রেড hlt করে। */
#ifndef ROZOS_SCHED_H
#define ROZOS_SCHED_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>

#define SCHED_SLICE_MS      10
#define SCHED_KSTACK_SIZE   (32 * 1024)
#define SCHED_NAME_MAX      32

enum thread_state {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_SLEEPING,     /* wake_tick পর্যন্ত */
    THREAD_BLOCKED,      /* sched_wake() না হওয়া পর্যন্ত */
    THREAD_DEAD
};

struct process;
struct interrupt_frame;

struct thread {
    uint8_t fpu[512] ROZ_ALIGNED(16);   /* fxsave এলাকা — প্রথম সদস্য, 16-byte aligned */
    int tid;
    char name[SCHED_NAME_MAX];
    enum thread_state state;
    uint64_t ksp;                        /* switch_context-এর সংরক্ষিত rsp */
    void *kstack;                        /* NULL = বুটের স্ট্যাক */
    size_t kstack_size;
    uint64_t wake_tick;
    uint64_t cpu_ticks;                  /* কত টিক CPU পেয়েছে */
    uint64_t fs_base;                    /* ইউজার TLS (arch_prctl) */
    uint64_t sys_nr;                     /* ধাপ ১৬খ-১: শেষ syscall-এর নম্বর (SA_RESTART-এর জন্য) */
    /* ধাপ ১৬খ-২: একই প্রসেসে কয়েকটা থ্রেড */
    int utid;                            /* ইউজারের দেখা thread id (gettid; মূল থ্রেডে = pid) */
    uint64_t clear_child_tid;            /* শেষ হলে এখানে 0 লিখে futex wake (pthread_join) */
    int exit_req;                        /* exit_group/execve: ইউজারে ফেরার আগে শেষ হও */
    int sig_waiting;                     /* sigsuspend/pause-এ: signal এলে জাগাতে হবে */
    struct process *proc;
    struct thread *next;                 /* সব থ্রেডের তালিকা */
};

typedef int (*kthread_fn_t)(void *arg);

void sched_init(void);                   /* বুটের থ্রেড + idle; টাইমার hook বসায় */
int  sched_active(void);
int  sched_can_sleep(void);              /* শিডিউলার চালু ও চলমান থ্রেড idle নয় */

struct thread *sched_current(void);
void sched_force_iret(void);              /* ধাপ ১৬খ-১: এবারের syscall ফেরা iretq দিয়ে */
/* ধাপ ১৬খ-২: একটা প্রসেসের সব থ্রেডে fn (IF = 0 অবস্থায়), utid দিয়ে থ্রেড খোঁজা */
struct process;
void sched_for_each_proc_thread(struct process *p, void (*fn)(struct thread *t, void *arg), void *arg);
struct thread *sched_find_utid(int utid);
struct thread *sched_create_kthread(const char *name, kthread_fn_t fn, void *arg);
/* ইউজার থ্রেড: frame (সম্পূর্ণ interrupt_frame) কার্নেল স্ট্যাকের মাথায় কপি হয়,
 * প্রথমবার চালু হলে সেখান থেকে iretq করে ইউজার-মোডে যায়। READY অবস্থায় থাকে না —
 * সব তৈরি শেষে sched_start_thread() দিতে হয়। */
struct thread *sched_create_user_thread(const char *name, struct process *proc,
                                        const struct interrupt_frame *frame,
                                        const uint8_t *fpu_state);
void sched_start_thread(struct thread *t);

/* IF = 0 অবস্থায় ডাকতে হয় (cpu_irq_save) */
void schedule(void);
void sched_yield(void);
void sched_sleep_ms(uint64_t ms);
void sched_block(void);                  /* IF = 0 অবস্থায়: BLOCKED করে schedule() */
void sched_wake(struct thread *t);       /* BLOCKED/SLEEPING → READY */
ROZ_NORETURN void sched_thread_exit(void);

/* অপেক্ষার লুপের জন্য (আগের cpu_sti_hlt()-এর জায়গায়): IF = 0 অবস্থায় ডাকা হয়,
 * IF = 1 করে ফেরে। অন্য থ্রেড তৈরি থাকলে তাদের সুযোগ দেয়, নইলে hlt। */
void sched_relax(void);
/* চলমান থ্রেডের প্রসেস kill হয়েছে কিনা — অপেক্ষার লুপগুলো -EINTR দিতে পারে */
int  sched_should_abort(void);

/* ইউজার-মোডে ফেরার ঠিক আগে (ইন্টারাপ্ট ও syscall শেষে, IF = 0): সময় ফুরালে
 * schedule(), kill হলে প্রসেস শেষ */
void sched_return_to_user(struct interrupt_frame *frame);

/* FPU/SSE: নতুন থ্রেডের শুরুর অবস্থা ও চলমান থ্রেডের অবস্থা সংরক্ষণ */
const uint8_t *sched_fpu_initial_state(void);
void sched_fpu_save_current(void);
void sched_fpu_load(const uint8_t *state);
void sched_set_fs_base(uint64_t base);   /* চলমান থ্রেডের */

/* তালিকা দেখা (kmon ps) */
size_t sched_thread_count(void);
struct thread *sched_thread_at(size_t index);
const char *sched_state_name(enum thread_state s);
uint64_t sched_switch_count(void);

/* ---------- kernel mutex (ধাপ ৬) ----------
 * একই থ্রেড একাধিকবার নিতে পারে (recursive)। অন্য থ্রেডের হাতে থাকলে
 * sched_yield() করে অপেক্ষা করে। শিডিউলার চালুর আগে শুধু গণনা রাখে। */
struct kmutex {
    struct thread *owner;
    unsigned depth;
    const char *name;
    uint64_t contended;                  /* কতবার অপেক্ষা করতে হয়েছে */
};

#define KMUTEX_INIT(n) { NULL, 0, (n), 0 }

void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);
/* পুরোপুরি ছেড়ে দিয়ে গভীরতা ফেরত (ব্লকিং ডিভাইস পড়ার আগে); পরে kmutex_relock */
unsigned kmutex_release_all(struct kmutex *m);
void kmutex_relock(struct kmutex *m, unsigned depth);

#endif
