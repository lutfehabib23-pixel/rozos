/* rozOS — অপুনরুদ্ধারযোগ্য ত্রুটিতে কার্নেল থামানো */
#ifndef ROZOS_PANIC_H
#define ROZOS_PANIC_H

#include <rozos/compiler.h>

ROZ_NORETURN void kpanic(const char *fmt, ...) ROZ_PRINTF(1, 2);

#define KASSERT(cond) \
    do { \
        if (!(cond)) { \
            kpanic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
        } \
    } while (0)

#endif
