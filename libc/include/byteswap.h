/* rozlibc — byteswap.h (ধাপ ১৬ক) */
#ifndef _BYTESWAP_H
#define _BYTESWAP_H
#ifdef __cplusplus
extern "C" {
#endif
#include <endian.h>
#define bswap_16(x) __roz_bswap16(x)
#define bswap_32(x) __roz_bswap32(x)
#define bswap_64(x) __roz_bswap64(x)
#ifdef __cplusplus
}
#endif
#endif
