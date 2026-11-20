/* rozOS — kmon: কার্নেল মনিটর (ডিবাগিংয়ের জন্য ছোট কমান্ড-লাইন)
 *
 * ধাপ ৭-এ আসল ইউজার-স্পেস শেল (rozsh) আসার আগ পর্যন্ত এটা দিয়েই কার্নেলের অবস্থা
 * দেখা ও টেস্ট করা হবে। ইনপুট আসে PS/2 কিবোর্ড অথবা serial পোর্ট থেকে। */
#ifndef ROZOS_KMON_H
#define ROZOS_KMON_H

#include <stddef.h>
#include <rozos/compiler.h>

/* কখনো ফেরে না */
ROZ_NORETURN void kmon_run(void);

/* ধাপ ৭: shell-এর /bin/kmon থেকে (কার্নেল-থ্রেডে) — "exit" দিলে ফেরে; একসাথে একটাই, নইলে -1 */
int kmon_session(void);

/* কিবোর্ড বা serial থেকে একটি কী (অপেক্ষা করে) */
int kmon_getkey(void);

/* এক লাইন পড়া (echo, Backspace, Ctrl+C = বাতিল, Ctrl+L = স্ক্রিন মোছা)।
 * ফেরত দেয় দৈর্ঘ্য, অথবা Ctrl+C হলে -1 */
int kmon_readline(char *buf, size_t size);

/* main.c-তে সংজ্ঞায়িত, kmon-এর "mem" কমান্ড ব্যবহার করে */
void print_memory_map(void);

#endif
