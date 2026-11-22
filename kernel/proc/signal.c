/* rozOS — signal (ধাপ ১৬খ-১): ইউজারের handler, mask, sigreturn, alarm
 *
 * আগে (ধাপ ৭-১৪) signal মানে ছিল শুধু প্রসেস শেষ (বা SIG_IGN)। এখন Linux-এর মতো:
 *
 *   পাঠানো  proc_send_signal(): SIG_IGN বা ডিফল্টে-উপেক্ষা (CHLD, WINCH, URG, CONT)
 *           হলে ফেলে দেওয়া; নইলে sigpending-এ বিট, আর বন্ধ (blocked) না থাকলে থ্রেড
 *           জাগানো — ঘুমন্ত syscall তখন -EINTR দেয় (sched_should_abort)।
 *   পৌঁছানো ইউজার-মোডে ফেরার ঠিক আগে (sched_return_to_user → proc_signal_deliver):
 *           সবচেয়ে ছোট নম্বরের অপেক্ষমাণ, বন্ধ নয় এমন signal বাছা।
 *             ডিফল্ট: প্রসেস শেষ (term_signal) বা উপেক্ষা।
 *             handler: ইউজার স্ট্যাকে rt_sigframe (Linux-এর বিন্যাস: pretcode, ucontext
 *             — সব রেজিস্টার, mask —, siginfo, FPU-র fxsave), তারপর rip = handler,
 *             rdi/rsi/rdx = signo/&info/&uc, ফেরার ঠিকানা = libc-র __restore_rt।
 *           EINTR পাওয়া syscall-এ SA_RESTART থাকলে rip দুই বাইট পিছিয়ে (syscall বা
 *           int 0x80 = ২ বাইট) আবার চালানো — poll/select/nanosleep/pause ছাড়া।
 *   ফেরা    rt_sigreturn (15): ফ্রেম থেকে রেজিস্টার, mask, FPU ফেরানো — RFLAGS/CS/SS
 *           যাচাই করে (ইউজার কার্নেলের অধিকার পেতে না পারে), ফেরা iretq দিয়ে।
 *
 * থামানো signal (STOP/TSTP/TTIN/TTOU)-এর ডিফল্ট এখন উপেক্ষা — job control পরে।
 * ধাপ ১৬খ-২: থ্রেড এলেও mask আর pending প্রসেসের (সব থ্রেডের একটাই) — যে থ্রেড আগে
 * ইউজারে ফেরে সে-ই handler চালায়; জাগানো হয় প্রসেসের signal-থ্রেড আর sigsuspend/pause/
 * sigwait-এ থাকা থ্রেডগুলো। tgkill-ও প্রসেসকেই পাঠায়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/proc.h>
#include <rozos/sched.h>
#include <rozos/syscall.h>
#include <rozos/uaccess.h>
#include <rozos/timer.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/console.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>

#define SIG_DFL_H 0
#define SIG_IGN_H 1

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

#define SS_ONSTACK   1
#define SS_DISABLE   2
#define MINSIGSTKSZ  2048

#define SI_USER      0
#define SI_KERNEL    0x80
#define SI_TIMER     (-2)
#define CLD_EXITED   1
#define CLD_KILLED   2

#define BIT(sig) (1ULL << ((sig) - 1))
#define UNBLOCKABLE (BIT(SIGKILL) | BIT(SIGSTOP))

/* ---------- Linux-এর ফ্রেম-বিন্যাস (x86-64) ---------- */

struct k_sigcontext {               /* = mcontext_t: gregs[23] + fpregs + reserved[8] */
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;               /* ইউজার-ঠিকানা: fxsave-এর ৫১২ বাইট */
    uint64_t reserved1[8];
};

struct k_stack {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  pad;
    uint64_t ss_size;
};

struct k_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct k_stack uc_stack;
    struct k_sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};

struct k_siginfo {                  /* ১২৮ বাইট */
    int32_t si_signo, si_errno, si_code, pad0;
    union {
        struct { int32_t pid; uint32_t uid; int32_t status; } kill;
        uint64_t addr;
        uint8_t raw[112];
    } u;
};

struct k_rt_sigframe {
    uint64_t pretcode;              /* handler যেখানে ret করবে */
    struct k_ucontext uc;
    struct k_siginfo info;
};

/* RFLAGS-এর যে বিটগুলো ইউজার বদলাতে পারে (Linux-এর FIX_EFLAGS) */
#define FIX_EFLAGS (0x1ULL | 0x4ULL | 0x10ULL | 0x40ULL | 0x80ULL | 0x100ULL | 0x400ULL | 0x800ULL | \
                    0x10000ULL | 0x40000ULL | 0x200000ULL)
#define RFLAGS_IF  0x200ULL

/* সবচেয়ে নিচের চালু বিট (০ থেকে) — TCC-তে __builtin_ctzll নেই, তাই নিজে */
static int lowest_bit(uint64_t v)
{
    int n = 0;
    while (!(v & 1)) { v >>= 1; n++; }
    return n;
}

static int default_ignored(int sig)
{
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH ||
           sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

static void wake_sigwaiter(struct thread *t, void *arg)
{
    (void)arg;
    if (t->sig_waiting) sched_wake(t);
}

void proc_wake_for_signal(struct process *p)
{
    if (p->thread) sched_wake(p->thread);
    if (p->nthreads > 1) sched_for_each_proc_thread(p, wake_sigwaiter, NULL);
}

int proc_signal_pending(const struct process *p)
{
    return p && (p->killed || (p->sigpending & ~p->sigmask));
}

int proc_send_signal(struct process *p, int sig, int sender, int code, int status)
{
    const struct k_sigaction *a;
    if (!p) return -ESRCH;
    if (p->state == PROC_ZOMBIE) return 0;
    if (sig == 0) return 0;
    if (sig < 1 || sig > NSIG_ROZ) return -EINVAL;
    if (sig == SIGKILL) {
        p->killed = SIGKILL;
        proc_wake_for_signal(p);
        return 0;
    }
    a = &p->sigact[sig - 1];
    if (a->handler == SIG_IGN_H) return 0;
    if (a->handler == SIG_DFL_H && default_ignored(sig)) return 0;
    p->sigpending |= BIT(sig);
    p->sig_sender[sig - 1] = sender;
    p->sig_code[sig - 1] = code;
    p->sig_status[sig - 1] = status;
    if (!(p->sigmask & BIT(sig))) {
        proc_wake_for_signal(p);                 /* ঘুম/অপেক্ষা থেকে তোলা (EINTR) */
    }
    return 0;
}

void proc_signal_fork(struct process *child, const struct process *parent)
{
    memcpy(child->sigact, parent->sigact, sizeof(child->sigact));
    child->sigmask = parent->sigmask;
    child->sigpending = 0;                        /* POSIX: সন্তান অপেক্ষমাণগুলো পায় না */
    child->altstack_sp = parent->altstack_sp;
    child->altstack_size = parent->altstack_size;
    child->altstack_flags = parent->altstack_flags;
    child->alarm_at = 0;                          /* alarm-ও না */
    child->alarm_interval = 0;
}

void proc_signal_exec(struct process *p)
{
    int i;
    /* POSIX: ধরা signal ডিফল্টে ফেরে; SIG_IGN আর mask থাকে; বিকল্প স্ট্যাক বাদ */
    for (i = 0; i < NSIG_ROZ; i++) {
        if (p->sigact[i].handler != SIG_IGN_H) {
            memset(&p->sigact[i], 0, sizeof(p->sigact[i]));
        }
    }
    p->altstack_sp = p->altstack_size = 0;
    p->altstack_flags = SS_DISABLE;
}

/* ---------- পৌঁছানো ---------- */

static int restartable(uint64_t nr)
{
    switch (nr) {
    case 7:    /* poll */
    case 23:   /* select */
    case 34:   /* pause */
    case 35:   /* nanosleep */
    case 128:  /* rt_sigtimedwait */
    case 130:  /* rt_sigsuspend */
    case 230:  /* clock_nanosleep */
        return 0;
    }
    return 1;
}

static int on_altstack(const struct process *p, uint64_t sp)
{
    return p->altstack_size && sp > p->altstack_sp && sp <= p->altstack_sp + p->altstack_size;
}

ROZ_NORETURN static void die(struct process *p, int sig)
{
    (void)p;
    cpu_sti();
    proc_exit(0, sig);
    for (;;) {}
}

static void setup_frame(struct process *p, struct interrupt_frame *f, int sig,
                        const struct k_sigaction *a, uint64_t oldmask)
{
    struct k_rt_sigframe fr;
    struct thread *t = sched_current();
    uint64_t sp = f->rsp, fpaddr, frame_addr;

    if ((a->flags & SA_ONSTACK) && p->altstack_size && !(p->altstack_flags & SS_DISABLE) &&
        !on_altstack(p, sp)) {
        sp = p->altstack_sp + p->altstack_size;
    } else {
        sp -= 128;                               /* x86-64 ABI-র red zone */
    }
    fpaddr = (sp - 512) & ~63ULL;
    frame_addr = ((fpaddr - sizeof(fr)) & ~15ULL) - 8;     /* handler-এ ঢোকার সময় rsp ≡ 8 (mod 16) */

    memset(&fr, 0, sizeof(fr));
    fr.pretcode = a->restorer;
    fr.uc.uc_stack.ss_sp = p->altstack_sp;
    fr.uc.uc_stack.ss_size = p->altstack_size;
    fr.uc.uc_stack.ss_flags = p->altstack_size ? (on_altstack(p, f->rsp) ? SS_ONSTACK : 0) : SS_DISABLE;
    fr.uc.uc_mcontext.r8 = f->r8;   fr.uc.uc_mcontext.r9 = f->r9;
    fr.uc.uc_mcontext.r10 = f->r10; fr.uc.uc_mcontext.r11 = f->r11;
    fr.uc.uc_mcontext.r12 = f->r12; fr.uc.uc_mcontext.r13 = f->r13;
    fr.uc.uc_mcontext.r14 = f->r14; fr.uc.uc_mcontext.r15 = f->r15;
    fr.uc.uc_mcontext.rdi = f->rdi; fr.uc.uc_mcontext.rsi = f->rsi;
    fr.uc.uc_mcontext.rbp = f->rbp; fr.uc.uc_mcontext.rbx = f->rbx;
    fr.uc.uc_mcontext.rdx = f->rdx; fr.uc.uc_mcontext.rax = f->rax;
    fr.uc.uc_mcontext.rcx = f->rcx; fr.uc.uc_mcontext.rsp = f->rsp;
    fr.uc.uc_mcontext.rip = f->rip; fr.uc.uc_mcontext.eflags = f->rflags;
    fr.uc.uc_mcontext.cs = (uint16_t)f->cs;
    fr.uc.uc_mcontext.ss = (uint16_t)f->ss;
    fr.uc.uc_mcontext.err = f->vector < 32 ? f->error_code : 0;
    fr.uc.uc_mcontext.trapno = f->vector < 32 ? f->vector : 0;
    fr.uc.uc_mcontext.oldmask = oldmask;
    fr.uc.uc_mcontext.cr2 = p->fault_addr;
    fr.uc.uc_mcontext.fpstate = fpaddr;
    fr.uc.uc_sigmask = oldmask;
    fr.info.si_signo = sig;
    fr.info.si_code = p->sig_code[sig - 1];
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL) {
        fr.info.u.addr = p->fault_addr;
    } else {
        fr.info.u.kill.pid = p->sig_sender[sig - 1];
        fr.info.u.kill.uid = 0;
        fr.info.u.kill.status = p->sig_status[sig - 1];
    }

    sched_fpu_save_current();
    if (!a->restorer || !(a->flags & SA_RESTORER) ||
        copy_to_user((void *)(uintptr_t)fpaddr, t->fpu, 512) < 0 ||
        copy_to_user((void *)(uintptr_t)frame_addr, &fr, sizeof(fr)) < 0) {
        /* ফ্রেম বসানো গেল না (স্ট্যাক নষ্ট বা restorer নেই): Linux-এর মতো SIGSEGV-তে শেষ */
        tty_set_color(TTY_COLOR_LIGHT_RED, TTY_COLOR_BLACK);
        kprintf("[signal] pid %d (%s): cannot deliver signal %d (bad stack) - killed\n",
                p->pid, p->name, sig);
        tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
        die(p, SIGSEGV);
    }

    f->rsp = frame_addr;
    f->rip = a->handler;
    f->rdi = (uint64_t)sig;
    f->rsi = frame_addr + offsetof(struct k_rt_sigframe, info);
    f->rdx = frame_addr + offsetof(struct k_rt_sigframe, uc);
    f->rax = 0;
    f->rflags &= ~(0x100ULL | 0x400ULL | 0x10000ULL);    /* TF, DF, RF মোছা */
    f->cs = GDT_USER_CODE_SEL;
    f->ss = GDT_USER_DATA_SEL;
    /* handler সাধারণ অবস্থায় শুরু হোক (MXCSR/x87 ডিফল্ট), আগেরটা ফ্রেমে রাখা আছে */
    sched_fpu_load(sched_fpu_initial_state());
}

void proc_signal_deliver(struct interrupt_frame *f)
{
    struct process *p = proc_current();
    uint64_t ready, oldmask;
    struct k_sigaction a;
    int sig;

    if (!p) return;
    if (p->killed) die(p, p->killed);
    ready = p->sigpending & ~p->sigmask;
    if (!ready) {
        if (p->have_saved_mask) {                /* sigsuspend থেকে signal ছাড়া ফেরা (হয় না, তবু) */
            p->sigmask = p->saved_sigmask;
            p->have_saved_mask = 0;
        }
        return;
    }
    sig = lowest_bit(ready) + 1;
    p->sigpending &= ~BIT(sig);
    a = p->sigact[sig - 1];

    if (a.handler == SIG_IGN_H || (a.handler == SIG_DFL_H && default_ignored(sig))) {
        if (p->have_saved_mask) {
            p->sigmask = p->saved_sigmask;
            p->have_saved_mask = 0;
        }
        return;
    }
    if (a.handler == SIG_DFL_H) die(p, sig);

    /* EINTR পাওয়া syscall: SA_RESTART হলে আবার চালানো */
    if (f->vector == SYSCALL_VECTOR && (int64_t)f->rax == -EINTR) {
        struct thread *t = sched_current();
        if ((a.flags & SA_RESTART) && t && restartable(t->sys_nr)) {
            f->rax = t->sys_nr;
            f->rip -= 2;
        }
    }

    oldmask = p->have_saved_mask ? p->saved_sigmask : p->sigmask;
    p->have_saved_mask = 0;
    setup_frame(p, f, sig, &a, oldmask);

    p->sigmask = oldmask | a.mask;
    if (!(a.flags & SA_NODEFER)) p->sigmask |= BIT(sig);
    p->sigmask &= ~UNBLOCKABLE;
    if (a.flags & SA_RESETHAND) {
        memset(&p->sigact[sig - 1], 0, sizeof(p->sigact[sig - 1]));
    }
    p->fault_addr = 0;
}

int proc_signal_fault(struct process *p, int sig, uint64_t addr)
{
    const struct k_sigaction *a = &p->sigact[sig - 1];
    /* handler আছে আর বন্ধ নয় — তবেই ধরা যায়; নইলে (Linux-এর মতো) জোর করে শেষ */
    if (a->handler <= SIG_IGN_H || (p->sigmask & BIT(sig))) return 0;
    p->sigpending |= BIT(sig);
    p->sig_code[sig - 1] = sig == SIGSEGV ? 1 /* SEGV_MAPERR */ : sig == SIGFPE ? 1 /* FPE_INTDIV */ : 1;
    p->sig_sender[sig - 1] = 0;
    p->fault_addr = addr;
    return 1;
}

/* ---------- syscall ---------- */

static int64_t sys_rt_sigaction(struct interrupt_frame *f, uint64_t sig, uint64_t uact, uint64_t uold,
                                uint64_t size, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    struct k_sigaction old;
    (void)f; (void)a5; (void)a6;
    if (sig < 1 || sig > NSIG_ROZ) return -EINVAL;
    if (size != 8 && size != 0) return -EINVAL;
    old = p->sigact[sig - 1];
    if (uact) {
        struct k_sigaction act;
        if (copy_from_user(&act, (const void *)(uintptr_t)uact, sizeof(act)) < 0) return -EFAULT;
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        act.mask &= ~UNBLOCKABLE;
        p->sigact[sig - 1] = act;
        /* ধাপ ১৪-এর sigignore আগের মতোই মেলানো থাকে (fork ইত্যাদি এটা দেখে) */
        if (act.handler == SIG_IGN_H) p->sigignore |= BIT(sig);
        else p->sigignore &= ~BIT(sig);
        /* POSIX: উপেক্ষায় বসালে অপেক্ষমাণটাও যায় */
        if (act.handler == SIG_IGN_H || (act.handler == SIG_DFL_H && default_ignored(sig))) {
            p->sigpending &= ~BIT(sig);
        }
    }
    if (uold && copy_to_user((void *)(uintptr_t)uold, &old, sizeof(old)) < 0) return -EFAULT;
    return 0;
}

static int64_t sys_rt_sigprocmask(struct interrupt_frame *f, uint64_t how, uint64_t uset, uint64_t uold,
                                  uint64_t size, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t old = p->sigmask;
    (void)f; (void)a5; (void)a6;
    if (size != 8) return -EINVAL;
    if (uset) {
        uint64_t set;
        if (copy_from_user(&set, (const void *)(uintptr_t)uset, sizeof(set)) < 0) return -EFAULT;
        switch (how) {
        case 0: p->sigmask |= set; break;          /* SIG_BLOCK */
        case 1: p->sigmask &= ~set; break;         /* SIG_UNBLOCK */
        case 2: p->sigmask = set; break;           /* SIG_SETMASK */
        default: return -EINVAL;
        }
        p->sigmask &= ~UNBLOCKABLE;
    }
    if (uold && copy_to_user((void *)(uintptr_t)uold, &old, sizeof(old)) < 0) return -EFAULT;
    return 0;
}

static int64_t sys_rt_sigpending(struct interrupt_frame *f, uint64_t uset, uint64_t size, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t set = p->sigpending & p->sigmask;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (size != 8) return -EINVAL;
    return copy_to_user((void *)(uintptr_t)uset, &set, sizeof(set)) < 0 ? -EFAULT : 0;
}

/* signal না আসা পর্যন্ত ঘুম (IF = 0 রেখে পরীক্ষা, যাতে জাগানোর বার্তা হারায় না) */
static void wait_for_signal(struct process *p)
{
    uint64_t fl = cpu_irq_save();
    sched_current()->sig_waiting = 1;
    while (!proc_signal_pending(p) && !sched_current()->exit_req) {
        sched_block();
    }
    sched_current()->sig_waiting = 0;
    cpu_irq_restore(fl);
}

static int64_t sys_rt_sigsuspend(struct interrupt_frame *f, uint64_t uset, uint64_t size, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t set;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (size != 8) return -EINVAL;
    if (copy_from_user(&set, (const void *)(uintptr_t)uset, sizeof(set)) < 0) return -EFAULT;
    p->saved_sigmask = p->sigmask;
    p->have_saved_mask = 1;
    p->sigmask = set & ~UNBLOCKABLE;
    wait_for_signal(p);
    return -EINTR;                               /* handler-এর পরে sigreturn আগের mask ফেরায় */
}

static int64_t sys_pause(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    wait_for_signal(proc_current());
    return -EINTR;
}

struct k_timespec { int64_t tv_sec, tv_nsec; };

/* sigwait/sigtimedwait: set-এর কোনো signal (বন্ধ থাকলেও) এলে তুলে নিয়ে নম্বর ফেরত */
static int64_t sys_rt_sigtimedwait(struct interrupt_frame *f, uint64_t uset, uint64_t uinfo, uint64_t uts,
                                   uint64_t size, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t set, deadline = 0;
    (void)f; (void)a5; (void)a6;
    if (size != 8) return -EINVAL;
    if (copy_from_user(&set, (const void *)(uintptr_t)uset, sizeof(set)) < 0) return -EFAULT;
    if (uts) {
        struct k_timespec ts;
        if (copy_from_user(&ts, (const void *)(uintptr_t)uts, sizeof(ts)) < 0) return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
        deadline = timer_uptime_ms() + (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec + 999999) / 1000000;
    }
    for (;;) {
        uint64_t got = p->sigpending & set;
        if (got) {
            int sig = lowest_bit(got) + 1;
            p->sigpending &= ~BIT(sig);
            if (uinfo) {
                struct k_siginfo info;
                memset(&info, 0, sizeof(info));
                info.si_signo = sig;
                info.si_code = p->sig_code[sig - 1];
                info.u.kill.pid = p->sig_sender[sig - 1];
                info.u.kill.status = p->sig_status[sig - 1];
                if (copy_to_user((void *)(uintptr_t)uinfo, &info, sizeof(info)) < 0) return -EFAULT;
            }
            return sig;
        }
        if (p->killed || sched_current()->exit_req || (p->sigpending & ~p->sigmask & ~set)) return -EINTR;
        if (uts && timer_uptime_ms() >= deadline) return -EAGAIN;
        sched_current()->sig_waiting = 1;
        if (uts) {
            uint64_t left = deadline - timer_uptime_ms();
            sched_sleep_ms(left > 20 ? 20 : left);
        } else {
            /* বন্ধ signal-এ জাগানো হয় না — তাই মাঝে মাঝে দেখা */
            sched_sleep_ms(10);
        }
        sched_current()->sig_waiting = 0;
    }
}

static int64_t sys_rt_sigreturn(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    struct k_rt_sigframe fr;
    uint64_t addr = f->rsp - 8;                  /* handler-এর ret pretcode তুলে নিয়েছে */
    uint8_t fpu[512] __attribute__((aligned(16)));
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (copy_from_user(&fr, (const void *)(uintptr_t)addr, sizeof(fr)) < 0) die(p, SIGSEGV);
    if (fr.uc.uc_mcontext.rip >= 0x0000800000000000ULL) die(p, SIGSEGV);
    if (fr.uc.uc_mcontext.fpstate) {
        if (copy_from_user(fpu, (const void *)(uintptr_t)fr.uc.uc_mcontext.fpstate, 512) < 0) die(p, SIGSEGV);
        *(uint32_t *)(fpu + 24) &= 0xFFBF;       /* MXCSR-এর সংরক্ষিত বিট (fxrstor-এর #GP এড়াতে) */
        sched_fpu_load(fpu);
    }
    f->r8 = fr.uc.uc_mcontext.r8;   f->r9 = fr.uc.uc_mcontext.r9;
    f->r10 = fr.uc.uc_mcontext.r10; f->r11 = fr.uc.uc_mcontext.r11;
    f->r12 = fr.uc.uc_mcontext.r12; f->r13 = fr.uc.uc_mcontext.r13;
    f->r14 = fr.uc.uc_mcontext.r14; f->r15 = fr.uc.uc_mcontext.r15;
    f->rdi = fr.uc.uc_mcontext.rdi; f->rsi = fr.uc.uc_mcontext.rsi;
    f->rbp = fr.uc.uc_mcontext.rbp; f->rbx = fr.uc.uc_mcontext.rbx;
    f->rdx = fr.uc.uc_mcontext.rdx; f->rax = fr.uc.uc_mcontext.rax;
    f->rcx = fr.uc.uc_mcontext.rcx; f->rsp = fr.uc.uc_mcontext.rsp;
    f->rip = fr.uc.uc_mcontext.rip;
    f->rflags = (f->rflags & ~FIX_EFLAGS) | (fr.uc.uc_mcontext.eflags & FIX_EFLAGS) | RFLAGS_IF;
    f->cs = GDT_USER_CODE_SEL;
    f->ss = GDT_USER_DATA_SEL;
    p->sigmask = fr.uc.uc_sigmask & ~UNBLOCKABLE;
    sched_force_iret();                          /* rcx/r11 হুবহু ফেরাতে sysret নয় */
    return (int64_t)f->rax;                      /* dispatch এটাই rax-এ বসায় */
}

static int64_t sys_sigaltstack(struct interrupt_frame *f, uint64_t uss, uint64_t uold, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    struct k_stack old;
    (void)a3; (void)a4; (void)a5; (void)a6;
    old.ss_sp = p->altstack_sp;
    old.ss_size = p->altstack_size;
    old.ss_flags = !p->altstack_size || (p->altstack_flags & SS_DISABLE) ? SS_DISABLE :
                   (on_altstack(p, f->rsp) ? SS_ONSTACK : 0);
    old.pad = 0;
    if (uss) {
        struct k_stack ss;
        if (copy_from_user(&ss, (const void *)(uintptr_t)uss, sizeof(ss)) < 0) return -EFAULT;
        if (on_altstack(p, f->rsp)) return -EPERM;
        if (ss.ss_flags & SS_DISABLE) {
            p->altstack_sp = p->altstack_size = 0;
            p->altstack_flags = SS_DISABLE;
        } else {
            if (ss.ss_flags & ~SS_ONSTACK) return -EINVAL;
            if (ss.ss_size < MINSIGSTKSZ) return -ENOMEM;
            p->altstack_sp = ss.ss_sp;
            p->altstack_size = ss.ss_size;
            p->altstack_flags = 0;
        }
    }
    if (uold && copy_to_user((void *)(uintptr_t)uold, &old, sizeof(old)) < 0) return -EFAULT;
    return 0;
}

/* ---------- alarm / setitimer (ITIMER_REAL) ---------- */

struct k_timeval { int64_t tv_sec, tv_usec; };
struct k_itimerval { struct k_timeval interval, value; };

static volatile uint64_t next_alarm;             /* সবচেয়ে কাছের alarm (ms) — টিকে দ্রুত বাদ দিতে */

static void recompute_next_alarm(void)
{
    uint64_t n = 0;
    size_t i, c = proc_count();
    for (i = 0; i < c; i++) {
        struct process *q = proc_at(i);
        if (q && q->state != PROC_ZOMBIE && q->alarm_at && (!n || q->alarm_at < n)) n = q->alarm_at;
    }
    next_alarm = n;
}

static void alarm_tick(uint64_t ticks)
{
    uint64_t now;
    size_t i, c;
    (void)ticks;
    if (!next_alarm) return;
    now = timer_uptime_ms();
    if (now < next_alarm) return;
    c = proc_count();
    for (i = 0; i < c; i++) {
        struct process *q = proc_at(i);
        if (!q || q->state == PROC_ZOMBIE || !q->alarm_at || now < q->alarm_at) continue;
        q->alarm_at = q->alarm_interval ? now + q->alarm_interval : 0;
        proc_send_signal(q, SIGALRM, 0, SI_TIMER, 0);
    }
    recompute_next_alarm();
}

static uint64_t ms_left(const struct process *p)
{
    uint64_t now = timer_uptime_ms();
    if (!p->alarm_at) return 0;
    return p->alarm_at > now ? p->alarm_at - now : 1;
}

static int64_t sys_alarm(struct interrupt_frame *f, uint64_t secs, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    uint64_t left = ms_left(p), fl;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    fl = cpu_irq_save();
    p->alarm_interval = 0;
    p->alarm_at = secs ? timer_uptime_ms() + secs * 1000 : 0;
    recompute_next_alarm();
    cpu_irq_restore(fl);
    return (int64_t)((left + 999) / 1000);
}

static void to_tv(uint64_t ms, struct k_timeval *tv)
{
    tv->tv_sec = (int64_t)(ms / 1000);
    tv->tv_usec = (int64_t)(ms % 1000) * 1000;
}

static int64_t sys_getitimer(struct interrupt_frame *f, uint64_t which, uint64_t uval, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    struct k_itimerval v;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    memset(&v, 0, sizeof(v));
    if (which > 2) return -EINVAL;
    if (which == 0) {                            /* ITIMER_REAL; VIRTUAL/PROF এখনো শূন্য */
        to_tv(p->alarm_interval, &v.interval);
        to_tv(ms_left(p), &v.value);
    }
    return copy_to_user((void *)(uintptr_t)uval, &v, sizeof(v)) < 0 ? -EFAULT : 0;
}

static int64_t sys_setitimer(struct interrupt_frame *f, uint64_t which, uint64_t unew, uint64_t uold,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    struct k_itimerval v, old;
    uint64_t val, iv, fl;
    (void)f; (void)a4; (void)a5; (void)a6;
    if (which != 0) return which <= 2 ? -ENOSYS : -EINVAL;
    memset(&old, 0, sizeof(old));
    to_tv(p->alarm_interval, &old.interval);
    to_tv(ms_left(p), &old.value);
    if (unew) {
        if (copy_from_user(&v, (const void *)(uintptr_t)unew, sizeof(v)) < 0) return -EFAULT;
        if (v.value.tv_sec < 0 || v.value.tv_usec < 0 || v.value.tv_usec >= 1000000 ||
            v.interval.tv_sec < 0 || v.interval.tv_usec < 0 || v.interval.tv_usec >= 1000000) return -EINVAL;
        val = (uint64_t)v.value.tv_sec * 1000 + (uint64_t)(v.value.tv_usec + 999) / 1000;
        iv = (uint64_t)v.interval.tv_sec * 1000 + (uint64_t)(v.interval.tv_usec + 999) / 1000;
        fl = cpu_irq_save();
        p->alarm_interval = val ? iv : 0;
        p->alarm_at = val ? timer_uptime_ms() + val : 0;
        recompute_next_alarm();
        cpu_irq_restore(fl);
    }
    if (uold && copy_to_user((void *)(uintptr_t)uold, &old, sizeof(old)) < 0) return -EFAULT;
    return 0;
}

/* ---------- tkill / tgkill / gettid (ধাপ ১৬খ-২: tid = থ্রেডের utid; signal প্রসেসে যায়) ---------- */

static int64_t sys_tkill(struct interrupt_frame *f, uint64_t tid, uint64_t sig, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *me = proc_current();
    struct thread *t;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if ((int64_t)tid <= 0) return -EINVAL;
    t = sched_find_utid((int)tid);
    if (!t || !t->proc) return -ESRCH;
    return proc_send_signal(t->proc, (int)sig, me ? me->pid : 0, -6 /* SI_TKILL */, 0);
}

static int64_t sys_tgkill(struct interrupt_frame *f, uint64_t tgid, uint64_t tid, uint64_t sig,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct thread *t;
    (void)a4; (void)a5; (void)a6;
    if ((int64_t)tgid <= 0 || (int64_t)tid <= 0) return -EINVAL;
    t = sched_find_utid((int)tid);
    if (!t || !t->proc || t->proc->pid != (int)tgid) return -ESRCH;
    return sys_tkill(f, tid, sig, 0, 0, 0, 0);
}

static int64_t sys_gettid(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return sched_current()->utid ? sched_current()->utid : proc_current()->pid;
}

void proc_signal_init(void)
{
    syscall_register(13, sys_rt_sigaction, "rt_sigaction");
    syscall_register(14, sys_rt_sigprocmask, "rt_sigprocmask");
    syscall_register(15, sys_rt_sigreturn, "rt_sigreturn");
    syscall_register(34, sys_pause, "pause");
    syscall_register(36, sys_getitimer, "getitimer");
    syscall_register(37, sys_alarm, "alarm");
    syscall_register(38, sys_setitimer, "setitimer");
    syscall_register(127, sys_rt_sigpending, "rt_sigpending");
    syscall_register(128, sys_rt_sigtimedwait, "rt_sigtimedwait");
    syscall_register(130, sys_rt_sigsuspend, "rt_sigsuspend");
    syscall_register(131, sys_sigaltstack, "sigaltstack");
    syscall_register(186, sys_gettid, "gettid");
    syscall_register(200, sys_tkill, "tkill");
    syscall_register(234, sys_tgkill, "tgkill");
    timer_add_tick_hook(alarm_tick);
}
