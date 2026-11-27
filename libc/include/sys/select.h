/* rozlibc — sys/select.h (ধাপ ১৬ক): select() — কার্নেলে poll-এর ওপর বানানো */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#ifdef __cplusplus
extern "C" {
#endif
#include <_rozlibc.h>
#include <time.h>
#include <signal.h>

#define FD_SETSIZE 1024
typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;
#define __ROZ_NFDBITS (8 * sizeof(unsigned long))
#define FD_ZERO(s)  do { unsigned __i; for (__i = 0; __i < FD_SETSIZE / __ROZ_NFDBITS; __i++) \
                         (s)->fds_bits[__i] = 0; } while (0)
#define FD_SET(d, s)   ((s)->fds_bits[(d) / __ROZ_NFDBITS] |= (1UL << ((d) % __ROZ_NFDBITS)))
#define FD_CLR(d, s)   ((s)->fds_bits[(d) / __ROZ_NFDBITS] &= ~(1UL << ((d) % __ROZ_NFDBITS)))
#define FD_ISSET(d, s) (((s)->fds_bits[(d) / __ROZ_NFDBITS] & (1UL << ((d) % __ROZ_NFDBITS))) != 0)

#ifndef _ROZLIBC_TIMEVAL
#define _ROZLIBC_TIMEVAL
struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};
#endif

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);
int pselect(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timespec *ts,
            const sigset_t *mask);

#ifdef __cplusplus
}
#endif
#endif
