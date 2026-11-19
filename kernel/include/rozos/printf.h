/* rozOS — কার্নেলের ফরম্যাটেড আউটপুট।
 * সাপোর্টেড: %d %i %u %x %X %o %p %s %c %%
 * modifier: l, ll, z  |  flag: '-', '0'  |  width (সংখ্যা বা '*')  |  precision (%.Ns) */
#ifndef ROZOS_PRINTF_H
#define ROZOS_PRINTF_H

#include <stddef.h>
#include <stdarg.h>
#include <rozos/compiler.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) ROZ_PRINTF(3, 4);

/* tty (স্ক্রিন + serial)-এ লেখে */
int kvprintf(const char *fmt, va_list ap);
int kprintf(const char *fmt, ...) ROZ_PRINTF(1, 2);

#endif
