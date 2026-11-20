/* rozOS — lwIP-এর "arch" স্তর: কম্পাইলার/প্ল্যাটফর্ম সংজ্ঞা */
#ifndef ROZOS_LWIP_ARCH_CC_H
#define ROZOS_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/printf.h>
#include <rozos/panic.h>

#define BYTE_ORDER LITTLE_ENDIAN

/* কার্নেলে libc নেই — lwIP নিজের বিকল্প ব্যবহার করে */
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H    1
#define LWIP_NO_LIMITS_H   1
#define LWIP_NO_UNISTD_H   1
#define SSIZE_MAX          0x7FFFFFFFFFFFFFFFL
typedef long ssize_t;
#define LWIP_SSIZE_T_DEFINED 1

#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define LWIP_PLATFORM_DIAG(x)   do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) kpanic("lwIP assertion: %s (%s:%d)", (x), __FILE__, __LINE__)

/* ধাপ ১১: TCC __GNUC__ দেয় না, তাই lwIP-এর arch.h packed struct বন্ধ রাখত
 * (init.c-র "Struct packing not implemented" assert) — TCC attribute বোঝে */
#ifdef __TINYC__
#define PACK_STRUCT_STRUCT __attribute__((packed))
#endif

#endif
