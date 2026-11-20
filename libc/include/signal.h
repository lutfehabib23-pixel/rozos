/* rozlibc — signal.h (ধাপ ৭: signal handler নেই — signal মানে প্রসেস শেষ) */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <_rozlibc.h>

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

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

int          kill(pid_t pid, int sig);
int          raise(int sig);
int          killpg(pid_t pgrp, int sig);    /* ধাপ ১৪: kill(-pgrp, sig) */
sighandler_t signal(int sig, sighandler_t handler);   /* এখন শুধু রেকর্ড রাখে */

/* ধাপ ১১: sigaction (pdpmake ইত্যাদির জন্য) — signal()-এর মতোই শুধু রেকর্ড রাখে */
typedef unsigned long sigset_t;
struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    int          sa_flags;
};
#define SA_RESTART 0x10000000
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);

#endif
