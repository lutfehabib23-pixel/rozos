/* rozlibc — exit ও atexit (আলাদা ফাইলে, যাতে `tcc -run`-এর runmain.o নিজের exit/atexit
 * দিতে পারে আর libc.a-র এই অংশ তখন লিংকে না আসে) */

#include <stdlib.h>
#include <unistd.h>

void __stdio_flush_all(void);

extern void (*__fini_array_start[])(void) __attribute__((weak));
extern void (*__fini_array_end[])(void) __attribute__((weak));

/* ধাপ ১৭ক: atexit আর __cxa_atexit (C++-এর static object-এর destructor) একই তালিকায়,
 * উল্টো ক্রমে চলে। C++ প্রোগ্রামে অনেক থাকতে পারে, তাই ১০২৪টা। */
#define ATEXIT_MAX 1024
static struct { void (*fn)(void *); void *arg; int cxa; } atexit_tab[ATEXIT_MAX];
static int atexit_count;

int atexit(void (*fn)(void))
{
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_tab[atexit_count].fn = (void (*)(void *))fn;
    atexit_tab[atexit_count].arg = 0;
    atexit_tab[atexit_count].cxa = 0;
    atexit_count++;
    return 0;
}

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_tab[atexit_count].fn = fn;
    atexit_tab[atexit_count].arg = arg;
    atexit_tab[atexit_count].cxa = 1;
    atexit_count++;
    return 0;
}

/* static প্রোগ্রামে একটাই "DSO" — exit-ই সব চালায় */
void __cxa_finalize(void *dso)
{
    (void)dso;
}

void exit(int code)
{
    while (atexit_count > 0) {
        atexit_count--;
        if (atexit_tab[atexit_count].cxa) atexit_tab[atexit_count].fn(atexit_tab[atexit_count].arg);
        else ((void (*)(void))atexit_tab[atexit_count].fn)();
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
