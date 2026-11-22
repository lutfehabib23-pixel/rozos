/* rozlibc — endian.h (ধাপ ১৬ক): x86-64 = little endian */
#ifndef _ENDIAN_H
#define _ENDIAN_H
#include <stdint.h>

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412
#define __BYTE_ORDER    __LITTLE_ENDIAN
#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#define PDP_ENDIAN      __PDP_ENDIAN
#define BYTE_ORDER      __BYTE_ORDER

static __inline uint16_t __roz_bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static __inline uint32_t __roz_bswap32(uint32_t x)
{ return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24); }
static __inline uint64_t __roz_bswap64(uint64_t x)
{ return ((uint64_t)__roz_bswap32((uint32_t)x) << 32) | __roz_bswap32((uint32_t)(x >> 32)); }

#define htobe16(x) __roz_bswap16(x)
#define htole16(x) (uint16_t)(x)
#define be16toh(x) __roz_bswap16(x)
#define le16toh(x) (uint16_t)(x)
#define htobe32(x) __roz_bswap32(x)
#define htole32(x) (uint32_t)(x)
#define be32toh(x) __roz_bswap32(x)
#define le32toh(x) (uint32_t)(x)
#define htobe64(x) __roz_bswap64(x)
#define htole64(x) (uint64_t)(x)
#define be64toh(x) __roz_bswap64(x)
#define le64toh(x) (uint64_t)(x)

#endif
