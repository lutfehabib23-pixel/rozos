/* rozOS — 8253/8254 PIT (Programmable Interval Timer), IRQ0, এবং টাইমার কোর
 *
 * PIT-এর মূল ঘড়ি 1193182 Hz। divisor = 1193182 / hz; 1000 Hz-এ divisor 1193,
 * আসল ফ্রিকোয়েন্সি ≈ 1000.15 Hz। তাই uptime হিসাব হয় প্রতি টিকের আসল ন্যানোসেকেন্ড
 * ধরে, অনুরোধ করা Hz ধরে নয় — এতে দীর্ঘ সময়ে ঘড়ি পিছিয়ে পড়ে না।
 *
 * ধাপ ৩: টিকের উৎস বদলানো যায় (pit.h) — LAPIC টাইমার চালু হলে সে
 * timer_core_tick() ডাকে আর PIT বন্ধ হয়। উৎস বদলানোর মুহূর্তের uptime
 * (base_ns) রেখে দেওয়া হয়, তাই সময় একটানা থাকে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/sched.h>
#include <rozos/random.h>
#include <rozos/timer.h>
#include "pit.h"
#include "pic.h"
#include "io.h"
#include "cpu.h"

#define PIT_BASE_HZ   1193182ULL
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
/* channel 0, lobyte/hibyte, mode 2 (rate generator), binary */
#define PIT_CMD_CH0_RATE 0x34

static volatile uint64_t ticks;
static uint32_t requested_hz;
static uint64_t ns_per_tick_x1000;     /* প্রতি টিকে ন্যানোসেকেন্ড × 1000 (ভগ্নাংশ ধরে রাখতে) */
static timer_tick_hook_t tick_hook;
static timer_tick_hook_t extra_hooks[4];   /* ধাপ ১৪: অন্য অংশের টিক (যেমন serial-এ Ctrl+C) */
static uint64_t pit_ns_per_tick_x1000; /* PIT-এর নিজের মান (pit_wait_ms-এর জন্য) */
static volatile uint64_t pit_ticks;    /* শুধু PIT-এর টিক */
static uint64_t base_ns;               /* উৎস বদলানোর মুহূর্তের uptime */
static uint64_t base_ticks;
static const char *source_name = "PIT";

void timer_core_tick(void)
{
    ticks++;
    random_add_entropy(cpu_rdtsc());     /* ধাপ ১০: প্রতি টিকের TSC jitter → entropy */
    {
        int i;
        for (i = 0; i < 4; i++) if (extra_hooks[i]) extra_hooks[i](ticks);
    }
    if (tick_hook) {
        tick_hook(ticks);
    }
}

int timer_add_tick_hook(timer_tick_hook_t hook)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (!extra_hooks[i]) {
            extra_hooks[i] = hook;
            return 0;
        }
    }
    return -1;
}

static void pit_irq(uint8_t irq, struct interrupt_frame *frame)
{
    (void)irq;
    (void)frame;
    pit_ticks++;
    timer_core_tick();
}

void timer_init(uint32_t hz)
{
    uint32_t divisor;

    if (hz < 19) {
        hz = 19;                       /* divisor 16 বিটে আঁটতে হবে */
    }
    if (hz > 100000) {
        hz = 100000;
    }
    divisor = (uint32_t)((PIT_BASE_HZ + hz / 2) / hz);
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }
    requested_hz = hz;
    ns_per_tick_x1000 = (uint64_t)divisor * 1000000000000ULL / PIT_BASE_HZ;
    pit_ns_per_tick_x1000 = ns_per_tick_x1000;
    ticks = 0;
    pit_ticks = 0;
    base_ns = 0;
    base_ticks = 0;
    source_name = "PIT";

    outb(PIT_COMMAND, PIT_CMD_CH0_RATE);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    irq_register(IRQ_TIMER, pit_irq);
}

uint32_t timer_frequency(void)
{
    return requested_hz;
}

uint64_t timer_ticks(void)
{
    return ticks;
}

/* t টিক × (npt/1000) ন্যানোসেকেন্ড, 64-বিট ওভারফ্লো ছাড়া */
static uint64_t ticks_to_ns(uint64_t t, uint64_t npt)
{
    uint64_t per_sec = 1000000000000ULL / npt;                 /* প্রায় Hz */
    uint64_t secs, rem;
    if (per_sec == 0) {
        per_sec = 1;
    }
    secs = t / per_sec;
    rem  = t % per_sec;
    return secs * (per_sec * npt / 1000) + rem * npt / 1000;
}

uint64_t timer_uptime_ns(void)
{
    uint64_t flags = cpu_irq_save();
    uint64_t ns = base_ns + ticks_to_ns(ticks - base_ticks, ns_per_tick_x1000);
    cpu_irq_restore(flags);
    return ns;
}

uint64_t timer_uptime_ms(void)
{
    return timer_uptime_ns() / 1000000ULL;
}

void timer_sleep_ms(uint64_t ms)
{
    uint64_t target = timer_uptime_ms() + ms;

    /* ধাপ ৬: শিডিউলার চালু থাকলে থ্রেড ঘুমায়, CPU অন্যরা পায় */
    if (sched_can_sleep()) {
        sched_sleep_ms(ms);
        return;
    }
    for (;;) {
        cpu_cli();
        if (timer_uptime_ms() >= target) {
            cpu_sti();
            return;
        }
        cpu_sti_hlt();                 /* পরের টিক পর্যন্ত ঘুম */
    }
}

void timer_set_tick_hook(timer_tick_hook_t hook)
{
    tick_hook = hook;
}

/* ---------- টাইমার কোর (pit.h) ---------- */

void timer_core_switch_source(const char *name, uint64_t new_ns_per_tick_x1000)
{
    base_ns = base_ns + ticks_to_ns(ticks - base_ticks, ns_per_tick_x1000);
    base_ticks = ticks;
    ns_per_tick_x1000 = new_ns_per_tick_x1000;
    source_name = name;
}

const char *timer_source_name(void)
{
    return source_name;
}

void pit_stop(void)
{
    irq_register(IRQ_TIMER, NULL);         /* IRQ0 mask */
}

void pit_wait_ms(uint64_t ms)
{
    uint64_t needed = ms * 1000000000ULL / pit_ns_per_tick_x1000;   /* ms → PIT টিক */
    uint64_t start = pit_ticks;
    if (needed == 0) {
        needed = 1;
    }
    for (;;) {
        cpu_cli();
        if (pit_ticks - start >= needed) {
            cpu_sti();
            return;
        }
        cpu_sti_hlt();
    }
}
