/* rozlibc — exit ও atexit (আলাদা ফাইলে, যাতে `tcc -run`-এর runmain.o নিজের exit/atexit
 * দিতে পারে আর libc.a-র এই অংশ তখন লিংকে না আসে) */

#include <stdlib.h>
#include <unistd.h>

void __stdio_flush_all(void);

extern void (*__fini_array_start[])(void) __attribute__((weak));
extern void (*__fini_array_end[])(void) __attribute__((weak));

#define ATEXIT_MAX 32
static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*fn)(void))
{
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

void exit(int code)
{
    while (atexit_count > 0) {
        atexit_fns[--atexit_count]();
    }
    if (__fini_array_start && __fini_array_end) {
        void (**f)(void) = __fini_array_end;
        while (f > __fini_array_start) {
            (*--f)();                            /* C++ destructor, উল্টো ক্রমে */
        }
    }
    __stdio_flush_all();
    _exit(code);
}
