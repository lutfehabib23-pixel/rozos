/* rozOS — /bin/crash [ud]: ইচ্ছাকৃত ভুল — কার্নেল যেন শুধু এই প্রসেস শেষ করে
 *   (আর্গুমেন্ট ছাড়া) NULL পয়েন্টারে লেখা → page fault → SIGSEGV (11)
 *   ud                  অবৈধ নির্দেশ (ud2) → SIGILL (4) */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "ud")) {
        printf("crash: executing ud2\n");
        __asm__ __volatile__("ud2");
    } else {
        volatile int *p = (volatile int *)0;
        printf("crash: writing to address 0\n");
        *p = 42;
    }
    printf("crash: still alive?!\n");
    return 1;
}
