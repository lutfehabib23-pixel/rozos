/* rozOS — টাইমার কোরের অভ্যন্তরীণ ইন্টারফেস (শুধু টাইমার ড্রাইভারদের জন্য)
 *
 * কার্নেলের বাকি অংশ rozos/timer.h ব্যবহার করে, এটা নয়। এখানে আছে টিকের উৎস
 * (PIT বা LAPIC টাইমার) বদলানোর ব্যবস্থা — উৎস বদলালেও uptime একটানা থাকে। */
#ifndef ROZOS_ARCH_X86_64_PIT_H
#define ROZOS_ARCH_X86_64_PIT_H

#include <stdint.h>

/* টিকের উৎস প্রতিটি টাইমার ইন্টারাপ্টে এটা ডাকে */
void timer_core_tick(void);

/* নতুন উৎসে যাওয়া: ns_per_tick_x1000 = প্রতি টিকে ন্যানোসেকেন্ড × 1000।
 * ইন্টারাপ্ট বন্ধ রেখে ডাকতে হবে। */
void timer_core_switch_source(const char *name, uint64_t ns_per_tick_x1000);
const char *timer_source_name(void);

/* PIT-এর IRQ0 বন্ধ (LAPIC টাইমার দায়িত্ব নিলে) */
void pit_stop(void);

/* PIT দিয়ে নির্দিষ্ট সময় অপেক্ষা (অন্য টাইমার মাপার জন্য; ইন্টারাপ্ট চালু থাকতে হবে) */
void pit_wait_ms(uint64_t ms);

#endif
