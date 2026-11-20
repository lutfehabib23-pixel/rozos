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
sighandler_t signal(int sig, sighandler_t handler);   /* এখন শুধু রেকর্ড রাখে */

#endif
