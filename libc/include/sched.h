/* rozlibc — sched.h (ধাপ ১৬খ-২) */
#ifndef _SCHED_H
#define _SCHED_H
#ifdef __cplusplus
extern "C" {
#endif
#include <_rozlibc.h>
#include <time.h>

struct sched_param { int sched_priority; };
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5

/* CPU সেট — rozOS এখন একটা CPU চালায় */
#define CPU_SETSIZE 128
typedef struct { unsigned long __bits[CPU_SETSIZE / 64]; } cpu_set_t;
#define CPU_ZERO(s)     do { unsigned __i; for (__i = 0; __i < CPU_SETSIZE / 64; __i++) (s)->__bits[__i] = 0; } while (0)
#define CPU_SET(c, s)   ((s)->__bits[(c) / 64] |= 1UL << ((c) % 64))
#define CPU_CLR(c, s)   ((s)->__bits[(c) / 64] &= ~(1UL << ((c) % 64)))
#define CPU_ISSET(c, s) (((s)->__bits[(c) / 64] >> ((c) % 64)) & 1)
#define CPU_COUNT(s)    __roz_cpu_count(s)
int __roz_cpu_count(const cpu_set_t *s);

int sched_yield(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_getparam(pid_t pid, struct sched_param *p);
int sched_setparam(pid_t pid, const struct sched_param *p);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *p);
int sched_rr_get_interval(pid_t pid, struct timespec *t);
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set);
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set);
int sched_getcpu(void);

#ifdef __cplusplus
}
#endif
#endif
