/* rozlibc — signal (ধাপ ১৬খ-১; আগে syscall.c-তে signal(), posix.c-তে sigaction ছিল)
 *
 * কার্নেলের kernel_sigaction-এ বিন্যাস: handler, flags, restorer, mask। ফেরার জন্য
 * সবসময় SA_RESTORER + __restore_rt (sigrestore.S)। signal() BSD/glibc-র মতো:
 * SA_RESTART সহ, তাই read() ইত্যাদি handler-এর পরে আবার চলে। */

#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <rozos.h>

#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_pause           34
#define SYS_getitimer       36
#define SYS_alarm           37
#define SYS_setitimer       38
#define SYS_rt_sigpending   127
#define SYS_rt_sigtimedwait 128
#define SYS_rt_sigsuspend   130
#define SYS_sigaltstack     131
#define SYS_tgkill          234

#define SC(n, a, b, c, d) __syscall_ret(__syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0))

extern void __restore_rt(void);

struct k_sigaction {
    unsigned long handler, flags, restorer, mask;
};

/* ---------- sigset (signal n = বিট n-1) ---------- */

static int bad_sig(int sig)
{
    if (sig <= 0 || sig > 64) {
        errno = EINVAL;
        return 1;
    }
    return 0;
}

int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set) { *set = ~0ul; return 0; }

int sigaddset(sigset_t *set, int sig)
{
    if (bad_sig(sig)) return -1;
    *set |= 1ul << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (bad_sig(sig)) return -1;
    *set &= ~(1ul << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (bad_sig(sig)) return -1;
    return (*set >> (sig - 1)) & 1;
}

int sigisemptyset(const sigset_t *set) { return *set == 0; }
int sigandset(sigset_t *d, const sigset_t *a, const sigset_t *b) { *d = *a & *b; return 0; }
int sigorset(sigset_t *d, const sigset_t *a, const sigset_t *b) { *d = *a | *b; return 0; }

/* ---------- sigaction / signal ---------- */

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct k_sigaction ka, ko;
    if (bad_sig(sig)) return -1;
    if (act) {
        ka.handler = (unsigned long)act->sa_handler;
        ka.flags = (unsigned long)(unsigned)act->sa_flags | SA_RESTORER;
        ka.restorer = (unsigned long)(act->sa_restorer && (act->sa_flags & SA_RESTORER) ?
                                      act->sa_restorer : __restore_rt);
        ka.mask = act->sa_mask;
    }
    if (SC(SYS_rt_sigaction, sig, act ? &ka : NULL, old ? &ko : NULL, 8) < 0) return -1;
    if (old) {
        memset(old, 0, sizeof(*old));
        old->sa_handler = (sighandler_t)ko.handler;
        old->sa_flags = (int)(ko.flags & ~(unsigned long)SA_RESTORER);
        old->sa_mask = ko.mask;
    }
    return 0;
}

static sighandler_t set_handler(int sig, sighandler_t h, int flags)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;
    sa.sa_flags = flags;
    if (sig == SIGKILL || sig == SIGSTOP) {
        errno = EINVAL;
        return SIG_ERR;
    }
    if (sigaction(sig, &sa, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

sighandler_t signal(int sig, sighandler_t h) { return set_handler(sig, h, SA_RESTART); }
sighandler_t bsd_signal(int sig, sighandler_t h) { return set_handler(sig, h, SA_RESTART); }
sighandler_t sysv_signal(int sig, sighandler_t h) { return set_handler(sig, h, SA_RESETHAND | SA_NODEFER); }

int siginterrupt(int sig, int flag)
{
    struct sigaction sa;
    if (sigaction(sig, NULL, &sa) < 0) return -1;
    if (flag) sa.sa_flags &= ~SA_RESTART;
    else sa.sa_flags |= SA_RESTART;
    return sigaction(sig, &sa, NULL);
}

/* ---------- mask ---------- */

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return (int)SC(SYS_rt_sigprocmask, how, set, old, 8);
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    /* ধাপ ১৬খ-২-এর আগে এক প্রসেস = এক থ্রেড; pthread-এর নিয়মে ভুল সংখ্যা ফেরত দেয় */
    int r = sigprocmask(how, set, old);
    return r < 0 ? errno : 0;
}

int sigsuspend(const sigset_t *mask) { return (int)SC(SYS_rt_sigsuspend, mask, 8, 0, 0); }
int sigpending(sigset_t *set) { return (int)SC(SYS_rt_sigpending, set, 8, 0, 0); }

int sighold(int sig)
{
    sigset_t s;
    sigemptyset(&s);
    if (sigaddset(&s, sig) < 0) return -1;
    return sigprocmask(SIG_BLOCK, &s, NULL);
}

int sigrelse(int sig)
{
    sigset_t s;
    sigemptyset(&s);
    if (sigaddset(&s, sig) < 0) return -1;
    return sigprocmask(SIG_UNBLOCK, &s, NULL);
}

int sigignore(int sig) { return signal(sig, SIG_IGN) == SIG_ERR ? -1 : 0; }

int sigpause(int sig)
{
    sigset_t s;
    sigprocmask(SIG_BLOCK, NULL, &s);
    sigdelset(&s, sig);
    return sigsuspend(&s);
}

int sigaltstack(const stack_t *ss, stack_t *old)
{
    /* stack_t-এর বিন্যাস কার্নেলের সাথে মেলে: sp, flags(int)+ফাঁক, size */
    return (int)SC(SYS_sigaltstack, ss, old, 0, 0);
}

/* ---------- অপেক্ষা ---------- */

int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout)
{
    return (int)SC(SYS_rt_sigtimedwait, set, info, timeout, 8);
}

int sigwaitinfo(const sigset_t *set, siginfo_t *info) { return sigtimedwait(set, info, NULL); }

int sigwait(const sigset_t *set, int *sig)
{
    int r;
    do {
        r = sigtimedwait(set, NULL, NULL);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return errno;
    *sig = r;
    return 0;
}

int pause(void) { return (int)SC(SYS_pause, 0, 0, 0, 0); }

/* ---------- টাইমার ---------- */

unsigned alarm(unsigned seconds) { return (unsigned)SC(SYS_alarm, seconds, 0, 0, 0); }

int setitimer(int which, const struct itimerval *nv, struct itimerval *old)
{
    return (int)SC(SYS_setitimer, which, nv, old, 0);
}

int getitimer(int which, struct itimerval *v) { return (int)SC(SYS_getitimer, which, v, 0, 0); }

/* ---------- পাঠানো ---------- */

int tgkill(pid_t tgid, pid_t tid, int sig) { return (int)SC(SYS_tgkill, tgid, tid, sig, 0); }

int sigqueue(pid_t pid, int sig, const union sigval value)
{
    (void)value;                     /* মান পাঠানো এখনো নেই — সাধারণ kill */
    return kill(pid, sig);
}

/* ---------- নাম ---------- */

const char *const sys_siglist[65] = {
    "Unknown signal 0", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap",
    "Aborted", "Bus error", "Floating point exception", "Killed", "User defined signal 1",
    "Segmentation fault", "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated",
    "Stack fault", "Child exited", "Continued", "Stopped (signal)", "Stopped",
    "Stopped (tty input)", "Stopped (tty output)", "Urgent I/O condition", "CPU time limit exceeded",
    "File size limit exceeded", "Virtual timer expired", "Profiling timer expired",
    "Window changed", "I/O possible", "Power failure", "Bad system call",
};

void psignal(int sig, const char *msg)
{
    if (msg && *msg) fprintf(stderr, "%s: %s\n", msg, strsignal(sig));
    else fprintf(stderr, "%s\n", strsignal(sig));
}

void psiginfo(const siginfo_t *info, const char *msg)
{
    psignal(info->si_signo, msg);
}
