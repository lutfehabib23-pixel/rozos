/* rozOS — /bin/spin <অক্ষর> [বার]: CPU ব্যস্ত রেখে (yield ছাড়া) মাঝে মাঝে অক্ষর ছাপে।
 * দুটো একসাথে চালালে অক্ষরগুলো মিশে আসা মানে টাইমার preemption কাজ করছে।
 * double দিয়ে হিসাবও করে — শেষে মান না মিললে FPU/SSE অবস্থা সুইচে নষ্ট হয়েছে। */

#include <stdio.h>
#include <rozos.h>

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "*";
    int rounds = 6, r;
    volatile double acc = 0.0;
    double expect = 0.0;
    long i;

    if (argc > 2) {
        rounds = 0;
        for (i = 0; argv[2][i]; i++) rounds = rounds * 10 + (argv[2][i] - '0');
    }
    for (r = 0; r < rounds; r++) {
        long start = rozos_now_ms();
        while (rozos_now_ms() - start < 40) {        /* ~40 ms ব্যস্ত — 10 ms-এর slice-এর চেয়ে বেশি */
            for (i = 0; i < 2000; i++) {
                acc = acc + 0.5;
            }
            expect += 1000.0;
        }
        printf("%s%d ", tag, r);
        fflush(stdout);                         /* লাইন শেষ হওয়ার অপেক্ষা নয় */
    }
    if (acc != expect) {
        printf("\nspin %s: FPU state corrupted (%ld != %ld)\n", tag, (long)acc, (long)expect);
        return 3;
    }
    return 0;
}
