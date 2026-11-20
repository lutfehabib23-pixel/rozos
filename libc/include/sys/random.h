/* rozlibc — sys/random.h (ধাপ ১০) */
#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <_rozlibc.h>

#define GRND_NONBLOCK 1
#define GRND_RANDOM   2

ssize_t getrandom(void *buf, size_t len, unsigned flags);
int     getentropy(void *buf, size_t len);       /* len ≤ 256 */

#endif
