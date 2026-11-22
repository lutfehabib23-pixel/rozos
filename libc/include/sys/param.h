/* rozlibc — sys/param.h (ধাপ ১৬ক) */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H
#include <limits.h>
#include <endian.h>

#define MAXPATHLEN  PATH_MAX
#define MAXHOSTNAMELEN 64
#define MAXSYMLINKS 20
#define NOFILE      256
#define NGROUPS     NGROUPS_MAX
#define NBBY        8
#define DEV_BSIZE   512
#define HZ          100
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x)   ((((x) - 1) & (x)) == 0)
#define nitems(a)     (sizeof(a) / sizeof((a)[0]))
#define setbit(a, i)  (((unsigned char *)(a))[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i)  (((unsigned char *)(a))[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i)   (((const unsigned char *)(a))[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i)   (!isset(a, i))

#endif
