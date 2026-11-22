/* rozOS — প্রসেস (ধাপ ৬)
 *
 * প্রতিটি প্রসেস: নিজস্ব ঠিকানা-স্থান (PML4), নিজস্ব fd টেবিল, cwd, একটা থ্রেড।
 * pid 0 = কার্নেল (কোনো process struct নেই); কার্নেল থেকে চালানো প্রসেসের ppid 0।
 *
 * ইউজার ঠিকানা-বিন্যাস:
 *   0x0000000000400000…        ELF (স্ট্যাটিক, ET_EXEC) — তারপর brk heap
 *   0x0000700000000000…        mmap এলাকা (নিচ থেকে ওপরে)
 *   0x00007FFFFFFFF000 নিচে     ইউজার স্ট্যাক: শুরুতে 64 KiB, page fault-এ 8 MiB পর্যন্ত বাড়ে */
#ifndef ROZOS_PROC_H
#define ROZOS_PROC_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/sched.h>

#define PROC_MAX            64
#define PROC_NAME_MAX       32

#define USER_STACK_TOP      0x00007FFFFFFFF000ULL
#define USER_STACK_INITIAL  (64ULL * 1024)
#define USER_STACK_MAX      (8ULL * 1024 * 1024)
#define USER_MMAP_BASE      0x0000700000000000ULL
#define USER_MMAP_END       (USER_STACK_TOP - USER_STACK_MAX - 0x100000ULL)
#define USER_MIN_ADDR       0x0000000000001000ULL

/* execve: আর্গুমেন্টের সীমা */
#define PROC_ARG_MAX        64
#define PROC_ARGS_BYTES     (32 * 1024)

/* signal নম্বর (Linux-এর একই) — এখন শুধু প্রসেস শেষ করার কারণ হিসেবে */
#define SIGILL   4
#define SIGFPE   8
#define SIGKILL  9
#define SIGINT   2                  /* ধাপ ১৪: Ctrl+C */
#define SIGQUIT  3
#define SIGPIPE  13
#define SIGHUP   1
#define SIGSEGV  11
#define SIGTERM  15
/* ধাপ ১৬খ-১: আসল signal handler — বাকি নম্বর ও ধারণা rozos/signal.h-এ */
#define SIGABRT  6
#define SIGBUS   7
#define SIGUSR1  10
#define SIGUSR2  12
#define SIGALRM  14
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28
#define SIGSYS   31
#define NSIG_ROZ 64

/* ব্যবহারকারীর sigaction (Linux-এর kernel_sigaction-এর একই বিন্যাস) */
struct k_sigaction {
    uint64_t handler;               /* 0 = SIG_DFL, 1 = SIG_IGN, নইলে ইউজারের ফাংশন */
    uint64_t flags;                 /* SA_* */
    uint64_t restorer;              /* SA_RESTORER: handler-এর ফেরার ঠিকানা (libc-র __restore_rt) */
    uint64_t mask;                  /* handler চলার সময় আর যা বন্ধ */
};

enum proc_state {
    PROC_ALIVE = 1,
    PROC_ZOMBIE
};

struct thread;
struct vmm_space;
struct interrupt_frame;

struct process {
    int pid;
    int ppid;
    char name[PROC_NAME_MAX];
    enum proc_state state;
    int exit_code;                  /* exit() মান (0-255) */
    int term_signal;                /* signal-এ মারা গেলে তার নম্বর, নইলে 0 */
    int killed;                     /* kill করা হয়েছে (signal নম্বর) — পরের ইউজার-ফেরায় শেষ */
    int pgid;                       /* ধাপ ১৪: process group (Ctrl+C পুরো গ্রুপে) */
    int sid;                        /* ধাপ ১৪: session (setsid) */
    uint64_t sigignore;             /* ধাপ ১৪: SIG_IGN করা signal (বিট n-1); SIGKILL কখনো নয় */
    int waiting;                    /* wait4()-এ ঘুমিয়ে আছে */
    int autoreap;                   /* কেউ wait করবে না — শেষ হলে নিজেই মুছে যায় */
    struct thread *kwaiter;         /* কার্নেলের কোনো থ্রেড এই প্রসেসের জন্য অপেক্ষায় */
    struct vmm_space *space;
    struct vfs_fdtable *fdt;
    char cwd[VFS_PATH_MAX];
    uint64_t brk_start, brk_end;    /* heap: [brk_start, brk_end) ম্যাপ করা */
    uint64_t mmap_next;
    uint64_t start_tick;
    struct thread *thread;
    /* ---- ধাপ ১৬খ-১: signal ---- */
    struct k_sigaction sigact[NSIG_ROZ];
    uint64_t sigpending;            /* অপেক্ষমাণ (বিট n-1) */
    uint64_t sigmask;               /* বন্ধ করা (blocked) */
    uint64_t saved_sigmask;         /* sigsuspend-এর আগের mask (handler-এর পরে ফেরে) */
    int      have_saved_mask;
    int      sig_sender[NSIG_ROZ];  /* siginfo.si_pid: কে পাঠাল (0 = কার্নেল) */
    int      sig_code[NSIG_ROZ];    /* siginfo.si_code */
    int      sig_status[NSIG_ROZ];  /* SIGCHLD: সন্তানের exit code/signal */
    uint64_t fault_addr;            /* SIGSEGV/SIGBUS/SIGFPE/SIGILL: si_addr */
    uint64_t altstack_sp, altstack_size;
    int      altstack_flags;        /* SS_DISABLE ইত্যাদি */
    uint64_t alarm_at;              /* ITIMER_REAL: কখন (ms, uptime); 0 = বন্ধ */
    uint64_t alarm_interval;        /* পুনরাবৃত্তি (ms); 0 = একবার */
    /* ---- ধাপ ১৬খ-২: থ্রেড ---- */
    int      nthreads;              /* জীবিত থ্রেড (thread = তাদের একটা, signal পায়) */
    int      exiting;               /* exit_group/শেষ শুরু হয়েছে */
    struct thread *exiter;          /* যে থ্রেড শেষ করছে (বাকিরা শুধু নিজে শেষ হয়) */
    struct thread *wait_thread;     /* wait4-এ ঘুমন্ত থ্রেড */
    struct kmutex mm_lock;          /* brk/mmap/munmap/mprotect — থ্রেডরা একসাথে না বদলায় */
};

void proc_init(void);

/* কার্নেল থেকে নতুন প্রসেস (ppid 0), stdin/stdout/stderr = /dev/console।
 * argv NULL-সমাপ্ত (NULL হলে argv[0] = path)। ফেরত pid বা -ERRNO। */
int proc_spawn(const char *path, const char *const *argv, int autoreap);

/* কার্নেলের কোনো থ্রেড থেকে অপেক্ষা: শেষ হলে status (wait4-এর মতো এনকোড করা) দেয়,
 * timeout_ms = 0 মানে চিরকাল; সময় ফুরালে -ETIMEDOUT */
int proc_wait_kernel(int pid, int *status, uint64_t timeout_ms);

int  proc_kill(int pid, int sig);
/* ধাপ ১৪: গ্রুপের সবাইকে (ফেরত: কয়জন সত্যিই পেল — উপেক্ষা করা বাদে; কেউ না হলে -ESRCH) */
int  proc_kill_group(int pgid, int sig);
/* zombie প্রসেসের struct মুক্ত করা (sched.c-র reaper ও wait ডাকে) */
void proc_free(struct process *p);
struct process *proc_current(void);
struct process *proc_find(int pid);
size_t proc_count(void);
struct process *proc_at(size_t index);

/* syscall-এর জন্য (sys_proc.c) */
int64_t proc_fork(struct interrupt_frame *frame);
int64_t proc_execve(struct interrupt_frame *frame, const char *path,
                    char *const *uargv, char *const *uenvp);
void    proc_exit(int code, int sig);   /* ফেরে না */
int64_t proc_wait4(int pid, int *ustatus, int options);

/* ধাপ ১৬খ-১: signal পাঠানোর বিস্তারিত সহ (siginfo-র জন্য) — kernel/proc/signal.c */
int  proc_send_signal(struct process *p, int sig, int sender, int code, int status);
int  proc_signal_pending(const struct process *p);     /* ঘুম ভাঙার মতো কিছু আছে */
void proc_signal_fork(struct process *child, const struct process *parent);
void proc_signal_exec(struct process *p);
/* ইউজার-মোডে ফেরার ঠিক আগে (sched.c): handler-এর ফ্রেম বসানো বা ডিফল্ট কাজ */
void proc_signal_deliver(struct interrupt_frame *frame);
/* CPU এক্সেপশন থেকে signal: handler থাকলে 1 (ফ্রেম পরে বসবে), নইলে 0 */
int  proc_signal_fault(struct process *p, int sig, uint64_t addr);
void proc_signal_init(void);
/* সংকেত এলে জাগানো: signal-পাওয়া থ্রেড আর sigsuspend/pause-এ থাকা থ্রেডগুলো */
void proc_wake_for_signal(struct process *p);

/* ধাপ ১৬খ-২: থ্রেড — kernel/proc/process.c আর futex.c */
int     proc_alloc_tid(void);
int64_t proc_clone(struct interrupt_frame *frame, uint64_t flags, uint64_t newsp,
                   uint64_t uptid, uint64_t uctid, uint64_t tls);
void    proc_thread_exit(int code);     /* শুধু এই থ্রেড (শেষ থ্রেড হলে পুরো প্রসেস); ফেরে না */
int     futex_wake_addr(struct process *p, uint64_t uaddr, int n);
void    futex_init(void);

/* ইউজার-মোডের এক্সেপশন (idt.c-র hook): 1 = সামলানো হয়েছে */
int proc_user_fault(struct interrupt_frame *frame);

/* পাথ: আপেক্ষিক হলে প্রসেসের cwd যোগ করে পরম পাথ বানায় */
int proc_abs_path(const char *in, char *out, size_t size);

/* wait4 status এনকোডিং (Linux) */
#define W_EXITCODE(code)   (((code) & 0xFF) << 8)
#define W_SIGNALED(sig)    ((sig) & 0x7F)
#define WNOHANG 1

#endif
