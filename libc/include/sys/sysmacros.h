/* rozlibc — sys/sysmacros.h (ধাপ ১৯ছ): ডিভাইস নম্বর ভাঙা/জোড়া (Linux/glibc-এর মতো)
 * rozOS-এ st_rdev প্রায় সবসময় 0; sbase-এর ls/tar এগুলো ডাকে। */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <sys/types.h>

#define major(d) ((unsigned)((((d) >> 31 >> 1) & 0xfffff000u) | (((d) >> 8) & 0x00000fffu)))
#define minor(d) ((unsigned)((((d) >> 12) & 0xffffff00u) | ((d) & 0x000000ffu)))
#define makedev(x, y) ((dev_t)((((unsigned long long)(x) & 0xfffff000ull) << 32) | \
                               (((unsigned long long)(x) & 0x00000fffull) << 8) | \
                               (((unsigned long long)(y) & 0xffffff00ull) << 12) | \
                               ((unsigned long long)(y) & 0x000000ffull)))

#endif
