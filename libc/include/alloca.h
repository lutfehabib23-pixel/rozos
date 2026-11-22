/* rozlibc — alloca.h (ধাপ ১৬ক): gcc-তে builtin, TCC-তে libtcc1-এর alloca */
#ifndef _ALLOCA_H
#define _ALLOCA_H
#include <stddef.h>
#if defined(__GNUC__) && !defined(__TINYC__)
#define alloca(n) __builtin_alloca(n)
#else
void *alloca(size_t n);
#endif
#endif
