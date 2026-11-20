/* rozOS kernel — TCC বিল্ডের জন্য freestanding inttypes.h (ধাপ ১১) */
#ifndef ROZOS_TCC_INTTYPES_H
#define ROZOS_TCC_INTTYPES_H

#include <stdint.h>

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIuPTR "lu"
#define PRIxPTR "lx"

#endif
