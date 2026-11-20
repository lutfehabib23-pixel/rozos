/* rozOS — থ্রেড ও round-robin শিডিউলার (rozos/sched.h দেখুন)
 *
 * একক CPU। সব থ্রেড একটা বৃত্তাকার তালিকায় (all_threads); schedule() বর্তমানের
 * পর থেকে খুঁজে প্রথম READY থ্রেড নেয় — কেউ না থাকলে idle।
 *
 * CPU-র নিজস্ব তথ্য (cpu_local) GS-এ থাকে: syscall_entry.S কার্নেল স্ট্যাক
 * এখান থেকে নেয়। কার্নেলে GS_BASE = &cpu0, ইউজারে KERNEL_GS_BASE-এ রাখা (swapgs)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/sched.h>
#include <rozos/proc.h>
#include <rozos/timer.h>
#include <rozos/heap.h>
#include <rozos/vmm.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/panic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>

#define MSR_FS_BASE        0xC0000100
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)

/* syscall_entry.S-এর %gs:0 ও %gs:8 — ক্রম বদলানো যাবে না */
struct cpu_local {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
    struct thread *current;
};

extern void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
extern char kthread_start[], user_thread_start[];

static struct cpu_local cpu0;
static struct thread boot_thread;
static struct thread *current;
static struct thread *idle_thread;
static struct thread *all_threads;           /* বৃত্তাকার নয় — NULL-সমাপ্ত তালিকা */
static struct thread *dead_thread;           /* পরের schedule()-এ মুক্ত হবে */
static size_t thread_count;
static int next_tid;
static int active;
static volatile int need_resched;
static volatile unsigned slice_left;
static unsigned slice_ticks = SCHED_SLICE_MS;
static uint64_t switches;
static uint8_t fpu_initial[512] ROZ_ALIGNED(16);

/* ---------- FPU / SSE ---------- */

#ifdef __TINYC__
/* ধাপ ১১: TCC-র assembler fxsave64/fxrstor64 চেনে না — একই নির্দেশ বাইটে
 * (REX.W 0F AE /0 আর /1, ঠিকানা rdi-তে) */
static inline void fxsave(uint8_t *area)
{
    __asm__ __volatile__(".byte 0x48, 0x0f, 0xae, 0x07" : : "D"(area) : "memory");
}

static inline void fxrstor(const uint8_t *area)
{
    __asm__ __volatile__(".byte 0x48, 0x0f, 0xae, 0x0f" : : "D"(area) : "memory");
}
#else
static inline void fxsave(uint8_t *area)
{
    __asm__ __volatile__("fxsave64 (%0)" : : "r"(area) : "memory");
}

static inline void fxrstor(const uint8_t *area)
{
    __asm__ __volatile__("fxrstor64 (%0)" : : "r"(area) : "memory");
}
#endif

static void write_cr0(uint64_t v) { __asm__ __volatile__("mov %0, %%cr0" : : "r"(v) : "memory"); }
static void write_cr4(uint64_t v) { __asm__ __volatile__("mov %0, %%cr4" : : "r"(v) : "memory"); }

/* কার্নেল নিজে SSE ব্যবহার করে না (-mgeneral-regs-only); ইউজার প্রোগ্রামের জন্য চালু */
static void fpu_init(void)
{
    uint32_t mxcsr = 0x1F80;                 /* সব এক্সেপশন masked, round-to-nearest */
    uint64_t cr0 = cpu_read_cr0();

    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP;
    write_cr0(cr0);
    write_cr4(cpu_read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
    __asm__ __volatile__("fninit" : : : "memory");
    __asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr));
    fxsave(fpu_initial);
}

const uint8_t *sched_fpu_initial_state(void) { return fpu_initial; }

void sched_fpu_save_current(void)
{
    if (current) {
        fxsave(current->fpu);
    }
}

void sched_fpu_load(const uint8_t *state)
{
    if (current && state != current->fpu) {
        memcpy(current->fpu, state, sizeof(current->fpu));
    }
    fxrstor(current ? current->fpu : state);
}

void sched_set_fs_base(uint64_t base)
{
    if (current) {
        current->fs_base = base;
    }
    cpu_wrmsr(MSR_FS_BASE, base);
}

/* ---------- তালিকা ---------- */

static void list_add(struct thread *t)
{
    struct thread **pp = &all_threads;
    while (*pp) {
        pp = &(*pp)->next;
    }
    t->next = NULL;
    *pp = t;
    thread_count++;
}

static void list_remove(struct thread *t)
{
    struct thread **pp = &all_threads;
    while (*pp && *pp != t) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = t->next;
        thread_count--;
    }
}

size_t sched_thread_count(void) { return thread_count; }
uint64_t sched_switch_count(void) { return switches; }

struct thread *sched_thread_at(size_t index)
{
    struct thread *t = all_threads;
    while (t && index--) {
        t = t->next;
    }
    return t;
}

const char *sched_state_name(enum thread_state s)
{
    switch (s) {
    case THREAD_READY:    return "ready";
    case THREAD_RUNNING:  return "running";
    case THREAD_SLEEPING: return "sleeping";
    case THREAD_BLOCKED:  return "blocked";
    case THREAD_DEAD:     return "dead";
    }
    return "?";
}

/* ---------- থ্রেড তৈরি ---------- */

static struct thread *alloc_thread(const char *name)
{
    struct thread *t = (struct thread *)kmalloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->kstack = kmalloc(SCHED_KSTACK_SIZE);
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }
    t->kstack_size = SCHED_KSTACK_SIZE;
    t->tid = next_tid++;
    strncpy(t->name, name, SCHED_NAME_MAX - 1);
    memcpy(t->fpu, fpu_initial, sizeof(t->fpu));
    t->state = THREAD_BLOCKED;               /* sched_start_thread() না দেওয়া পর্যন্ত */
    return t;
}

static uint64_t kstack_top(const struct thread *t)
{
    return ((uint64_t)(uintptr_t)t->kstack + t->kstack_size) & ~15ULL;
}

/* switch_context যেভাবে তোলে: r15 r14 r13 r12 rbx rbp ret (নিচ থেকে ওপরে) */
static void prepare_stack(struct thread *t, uint64_t top, void *entry, uint64_t rbx, uint64_t r12)
{
    uint64_t *sp = (uint64_t *)(uintptr_t)top;
    *--sp = (uint64_t)(uintptr_t)entry;      /* ret */
    *--sp = 0;                               /* rbp */
    *--sp = rbx;
    *--sp = r12;
    *--sp = 0;                               /* r13 */
    *--sp = 0;                               /* r14 */
    *--sp = 0;                               /* r15 */
    t->ksp = (uint64_t)(uintptr_t)sp;
}

struct thread *sched_create_kthread(const char *name, kthread_fn_t fn, void *arg)
{
    struct thread *t = alloc_thread(name);
    uint64_t flags;

    if (!t) {
        return NULL;
    }
    /* `ret`-এর পরে rsp 16-এর গুণিতক হতে হবে (তারপর `call` ABI মেনে চলে) */
    prepare_stack(t, kstack_top(t) - 16, kthread_start,
                  (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)arg);
    flags = cpu_irq_save();
    list_add(t);
    t->state = THREAD_READY;
    cpu_irq_restore(flags);
    return t;
}

struct thread *sched_create_user_thread(const char *name, struct process *proc,
                                        const struct interrupt_frame *frame,
                                        const uint8_t *fpu_state)
{
    struct thread *t = alloc_thread(name);
    struct interrupt_frame *kf;
    uint64_t flags;

    if (!t) {
        return NULL;
    }
    t->proc = proc;
    if (fpu_state) {
        memcpy(t->fpu, fpu_state, sizeof(t->fpu));
    }
    /* ফ্রেম ঠিক সেখানে, যেখানে CPU/syscall_entry ইউজার থেকে ঢোকার সময় রাখে */
    kf = (struct interrupt_frame *)(uintptr_t)(kstack_top(t) - sizeof(*kf));
    memcpy(kf, frame, sizeof(*kf));
    prepare_stack(t, (uint64_t)(uintptr_t)kf, user_thread_start, (uint64_t)(uintptr_t)kf, 0);
    flags = cpu_irq_save();
    list_add(t);
    cpu_irq_restore(flags);
    return t;
}

void sched_start_thread(struct thread *t)
{
    uint64_t flags = cpu_irq_save();
    if (t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
    }
    cpu_irq_restore(flags);
}

/* ---------- শিডিউলিং ---------- */

static void reap_dead(void)
{
    struct thread *t = dead_thread;
    if (!t || t == current) {
        return;
    }
    dead_thread = NULL;
    list_remove(t);
    if (t->proc && t->proc->thread == t) {
        t->proc->thread = NULL;
        if (t->proc->state == PROC_ZOMBIE && t->proc->autoreap) {
            proc_free(t->proc);              /* কেউ wait করবে না */
        }
    }
    kfree(t->kstack);
    kfree(t);
}

static struct thread *pick_next(void)
{
    struct thread *t = current ? current->next : NULL;
    size_t n;

    for (n = 0; n < thread_count; n++) {
        if (!t) {
            t = all_threads;
        }
        if (t != idle_thread && t->state == THREAD_READY) {
            return t;
        }
        t = t->next;
    }
    if (current && current->state == THREAD_RUNNING) {
        return current;
    }
    return idle_thread;
}

void schedule(void)
{
    struct thread *prev = current, *next;

    if (!active) {
        return;
    }
    next = pick_next();
    need_resched = 0;
    slice_left = slice_ticks;
    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    next->state = THREAD_RUNNING;
    switches++;

    fxsave(prev->fpu);
    current = next;
    cpu0.current = next;
    if (next->kstack) {
        cpu0.kernel_rsp = kstack_top(next);
        tss_set_kernel_stack(cpu0.kernel_rsp);
    }
    if (next->proc && next->proc->space && vmm_current_space() != next->proc->space) {
        vmm_switch(next->proc->space);
    }
    if (next->proc) {
        cpu_wrmsr(MSR_FS_BASE, next->fs_base);
    }
    fxrstor(next->fpu);

    switch_context(&prev->ksp, next->ksp);

    /* আবার prev হিসেবে চলছি */
    reap_dead();
}

static void sched_tick(uint64_t ticks)
{
    struct thread *t;

    if (!active) {
        return;
    }
    if (current) {
        current->cpu_ticks++;
    }
    for (t = all_threads; t; t = t->next) {
        if (t->state == THREAD_SLEEPING && t->wake_tick <= ticks) {
            t->state = THREAD_READY;
        }
    }
    if (current == idle_thread) {
        need_resched = 1;
    } else if (slice_left > 0 && --slice_left == 0) {
        need_resched = 1;
    }
}

static int others_ready(void)
{
    struct thread *t;
    for (t = all_threads; t; t = t->next) {
        if (t != current && t != idle_thread && t->state == THREAD_READY) {
            return 1;
        }
    }
    return 0;
}

static int idle_main(void *arg)
{
    (void)arg;
    for (;;) {
        cpu_cli();
        if (others_ready()) {
            schedule();
        }
        cpu_sti_hlt();
    }
    return 0;
}

void sched_init(void)
{
    uint32_t hz = timer_frequency();

    fpu_init();

    memset(&boot_thread, 0, sizeof(boot_thread));
    boot_thread.tid = next_tid++;
    strncpy(boot_thread.name, "kmain", SCHED_NAME_MAX - 1);
    boot_thread.state = THREAD_RUNNING;
    memcpy(boot_thread.fpu, fpu_initial, sizeof(boot_thread.fpu));
    current = &boot_thread;
    cpu0.current = current;
    list_add(&boot_thread);

    cpu_wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)&cpu0);
    cpu_wrmsr(MSR_KERNEL_GS_BASE, 0);

    if (hz) {
        slice_ticks = (unsigned)((uint64_t)SCHED_SLICE_MS * hz / 1000);
        if (slice_ticks == 0) {
            slice_ticks = 1;
        }
    }
    slice_left = slice_ticks;

    idle_thread = sched_create_kthread("idle", idle_main, NULL);
    if (!idle_thread) {
        kpanic("sched: could not create the idle thread");
    }
    idle_thread->state = THREAD_READY;
    timer_set_tick_hook(sched_tick);
    active = 1;
}

int sched_active(void) { return active; }
int sched_can_sleep(void) { return active && current && current != idle_thread; }
struct thread *sched_current(void) { return current; }

void sched_yield(void)
{
    uint64_t flags = cpu_irq_save();
    schedule();
    cpu_irq_restore(flags);
}

void sched_sleep_ms(uint64_t ms)
{
    uint64_t flags, hz = timer_frequency();

    if (!sched_can_sleep()) {
        timer_sleep_ms(ms);
        return;
    }
    flags = cpu_irq_save();
    current->wake_tick = timer_ticks() + (hz ? (ms * hz + 999) / 1000 : ms);
    current->state = THREAD_SLEEPING;
    schedule();
    cpu_irq_restore(flags);
}

void sched_block(void)
{
    current->state = THREAD_BLOCKED;
    schedule();
}

void sched_wake(struct thread *t)
{
    uint64_t flags = cpu_irq_save();
    if (t && (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING)) {
        t->state = THREAD_READY;
    }
    cpu_irq_restore(flags);
}

void sched_thread_exit(void)
{
    cpu_cli();
    if (dead_thread && dead_thread != current) {
        reap_dead();
    }
    current->state = THREAD_DEAD;
    dead_thread = current;
    schedule();
    kpanic("sched: dead thread was scheduled again");
}

void sched_relax(void)
{
    if (active && others_ready()) {
        schedule();
        cpu_sti();
        return;
    }
    cpu_sti_hlt();
}

int sched_should_abort(void)
{
    return current && current->proc && current->proc->killed;
}

void sched_return_to_user(struct interrupt_frame *frame)
{
    (void)frame;
    if (!active) {
        return;
    }
    if (need_resched) {
        schedule();
    }
    if (current->proc && current->proc->killed) {
        cpu_sti();
        proc_exit(0, current->proc->killed);
    }
}

/* ---------- kernel mutex ---------- */

void kmutex_lock(struct kmutex *m)
{
    struct thread *self = current;
    uint64_t flags;

    if (!self) {
        m->depth++;                          /* শিডিউলার চালুর আগে: একটাই থ্রেড */
        return;
    }
    flags = cpu_irq_save();
    while (m->owner && m->owner != self) {
        m->contended++;
        sched_relax();                       /* মালিককে চলতে দেওয়া (IF = 1 করে ফেরে) */
        cpu_cli();
    }
    m->owner = self;
    m->depth++;
    cpu_irq_restore(flags);
}

void kmutex_unlock(struct kmutex *m)
{
    uint64_t flags = cpu_irq_save();
    if (m->depth > 0 && --m->depth == 0) {
        m->owner = NULL;
    }
    cpu_irq_restore(flags);
}

unsigned kmutex_release_all(struct kmutex *m)
{
    uint64_t flags = cpu_irq_save();
    unsigned d = 0;
    if (m->owner == current || !current) {
        d = m->depth;
        m->depth = 0;
        m->owner = NULL;
    }
    cpu_irq_restore(flags);
    return d;
}

void kmutex_relock(struct kmutex *m, unsigned depth)
{
    if (depth == 0) {
        return;
    }
    kmutex_lock(m);
    m->depth += depth - 1;
}
