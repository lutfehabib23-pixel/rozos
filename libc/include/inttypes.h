/* rozlibc — inttypes.h */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi64 "li"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"

intmax_t  strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);

#endif
