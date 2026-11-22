/* rozlibc — sys/cdefs.h (ধাপ ১৬ক, BSD-ধাঁচের কোডের জন্য) */
#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H
#include <features.h>
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS   }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#define __P(x) x
#define __CONCAT(x, y) x ## y
#define __STRING(x) #x
#define __dead2 __attribute__((noreturn))
#define __unused __attribute__((unused))
#define __printflike(f, a) __attribute__((format(printf, f, a)))
#endif
