/* rozlibc — signal.h
 *
 * ধাপ ৭-১৪: signal মানে ছিল প্রসেস শেষ, শুধু SIG_IGN মনে রাখা হত।
 * ধাপ ১৬খ-১: আসল handler (কার্নেল ইউজার-স্ট্যাকে ফ্রেম বসিয়ে ডাকে, rt_sigreturn দিয়ে
 * ফেরে), SA_SIGINFO (siginfo_t + ucontext), SA_RESTART, SA_NODEFER, SA_RESETHAND,
 * SA_ONSTACK + sigaltstack, sigprocmask/sigsuspend/sigpending/sigwait, alarm ও
 * setitimer (sys/time.h), SIGCHLD, SIGWINCH, আর CPU ভুলের signal (SIGSEGV, SIGFPE…)।
 * sigset_t = ৬৪ বিট: signal n মানে বিট n-1 (Linux-এর মতো)। */
#ifndef _SIGNAL_H
#define _SIGNAL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <_rozlibc.h>
#include <time.h>

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
/* ধাপ ১৬ক: বাকি Linux নম্বর */
#define SIGTRAP  5
#define SIGIOT   SIGABRT
#define SIGBUS   7
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCLD  SIGCHLD
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG  23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO   29
#define SIGPOLL SIGIO
#define SIGPWR  30
#define SIGSYS  31
#define SIGUNUSED SIGSYS
#define SIGRTMIN 34
#define SIGRTMAX 64
#define NSIG    65
#define _NSIG   NSIG

typedef int sig_atomic_t;

typedef void (*sighandler_t)(int);
typedef sighandler_t sig_t;
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
#define SIG_HOLD ((sighandler_t)2)

typedef unsigned long sigset_t;

/* ---------- siginfo ---------- */
union sigval {
    int   sival_int;
    void *sival_ptr;
};

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad0;
    union {
        struct { pid_t pid; uid_t uid; int status; } __kill;   /* kill, SIGCHLD */
        void *addr;                                          /* SIGSEGV/BUS/FPE/ILL */
        char __raw[112];
    } __u;
} siginfo_t;
#define si_pid    __u.__kill.pid
#define si_uid    __u.__kill.uid
#define si_status __u.__kill.status
#define si_addr   __u.addr
#define si_value  __u.__kill.status

/* si_code */
#define SI_USER     0
#define SI_KERNEL   0x80
#define SI_QUEUE    (-1)
#define SI_TIMER    (-2)
#define SI_TKILL    (-6)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define FPE_INTDIV  1
#define ILL_ILLOPC  1
#define BUS_ADRERR  2
#define CLD_EXITED  1
#define CLD_KILLED  2
#define CLD_DUMPED  3
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

/* ---------- sigaction ---------- */
struct sigaction {
    union {
        sighandler_t __sa_handler;
        void (*__sa_sigaction)(int, siginfo_t *, void *);
    } __sa_u;
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer)(void);
};
#define sa_handler   __sa_u.__sa_handler
#define sa_sigaction __sa_u.__sa_sigaction

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_NOMASK    SA_NODEFER
#define SA_ONESHOT   SA_RESETHAND

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* ---------- বিকল্প স্ট্যাক ---------- */
typedef struct {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;
#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

/* ---------- ucontext (SA_SIGINFO handler-এর তৃতীয় আর্গুমেন্ট) ---------- */
typedef long greg_t;
#define NGREG 23
typedef greg_t gregset_t[NGREG];
enum { REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RDI, REG_RSI,
       REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP, REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR,
       REG_TRAPNO, REG_OLDMASK, REG_CR2 };
typedef struct {
    gregset_t gregs;
    void *fpregs;                   /* fxsave-এর ৫১২ বাইট */
    unsigned long long __reserved1[8];
} mcontext_t;
typedef struct ucontext {
    unsigned long uc_flags;
    struct ucontext *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
} ucontext_t;

/* ---------- ফাংশন ---------- */
int          kill(pid_t pid, int sig);
int          raise(int sig);
int          killpg(pid_t pgrp, int sig);    /* ধাপ ১৪: kill(-pgrp, sig) */
sighandler_t signal(int sig, sighandler_t handler);   /* ধাপ ১৬খ-১: BSD ধাঁচ (SA_RESTART) */
sighandler_t bsd_signal(int sig, sighandler_t handler);
sighandler_t sysv_signal(int sig, sighandler_t handler);   /* SA_RESETHAND | SA_NODEFER */

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
int sigisemptyset(const sigset_t *set);
int sigandset(sigset_t *d, const sigset_t *a, const sigset_t *b);
int sigorset(sigset_t *d, const sigset_t *a, const sigset_t *b);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int sigsuspend(const sigset_t *mask);
int sigpending(sigset_t *set);
int sigaltstack(const stack_t *ss, stack_t *old);
int sigwait(const sigset_t *set, int *sig);
int sigwaitinfo(const sigset_t *set, siginfo_t *info);
int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout);
int sigqueue(pid_t pid, int sig, const union sigval value);
int siginterrupt(int sig, int flag);
int sighold(int sig);
int sigrelse(int sig);
int sigignore(int sig);
int sigpause(int sig);
void psignal(int sig, const char *msg);
void psiginfo(const siginfo_t *info, const char *msg);
int tgkill(pid_t tgid, pid_t tid, int sig);
extern const char *const sys_siglist[];

#ifdef __cplusplus
}
#endif
#endif
