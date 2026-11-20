/* rozOS — সিস্টেম টাইমার ইন্টারফেস
 *
 * কার্নেলের বাকি অংশ শুধু এই ফাংশনগুলো ব্যবহার করবে। এখন উৎস PIT (pit.c,
 * 1000 Hz); ধাপ ৩-এ APIC টাইমার এলে বাস্তবায়ন বদলাবে, ইন্টারফেস নয়।
 * ধাপ ৬-এ শিডিউলার timer_set_tick_hook() দিয়ে প্রতিটি টিকে ডাক পাবে। */
#ifndef ROZOS_TIMER_H
#define ROZOS_TIMER_H

#include <stdint.h>

#define TIMER_DEFAULT_HZ 1000

typedef void (*timer_tick_hook_t)(uint64_t ticks);

void     timer_init(uint32_t hz);
uint32_t timer_frequency(void);       /* অনুরোধ করা Hz */
uint64_t timer_ticks(void);           /* বুট থেকে মোট টিক */
uint64_t timer_uptime_ns(void);       /* হার্ডওয়্যারের আসল ফ্রিকোয়েন্সি অনুযায়ী */
uint64_t timer_uptime_ms(void);
void     timer_sleep_ms(uint64_t ms); /* ইন্টারাপ্ট চালু থাকতে হবে */
void     timer_set_tick_hook(timer_tick_hook_t hook);
int      timer_add_tick_hook(timer_tick_hook_t hook);   /* ধাপ ১৪: আরও (সর্বোচ্চ ৪টা) */

#endif
