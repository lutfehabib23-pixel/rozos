/* rozOS — CPU-নিয়ন্ত্রণের ছোট সহায়ক ফাংশন */
#ifndef ROZOS_ARCH_X86_64_CPU_H
#define ROZOS_ARCH_X86_64_CPU_H

#include <stdint.h>
#include <rozos/compiler.h>

static inline void cpu_cli(void) { __asm__ __volatile__("cli" : : : "memory"); }
static inline void cpu_sti(void) { __asm__ __volatile__("sti" : : : "memory"); }
static inline void cpu_hlt(void) { __asm__ __volatile__("hlt" : : : "memory"); }
static inline void cpu_pause(void) { __asm__ __volatile__("pause" : : : "memory"); }

/* ইন্টারাপ্ট বন্ধ করে চিরতরে থামিয়ে রাখে */
static inline ROZ_NORETURN void cpu_halt_forever(void)
{
    for (;;) {
        __asm__ __volatile__("cli; hlt" : : : "memory");
    }
}

static inline uint64_t cpu_read_cr0(void)
{
    uint64_t v;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline uint64_t cpu_read_cr2(void)
{
    uint64_t v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t cpu_read_cr3(void)
{
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint64_t cpu_read_cr4(void)
{
    uint64_t v;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline uint64_t cpu_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t cpu_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif
