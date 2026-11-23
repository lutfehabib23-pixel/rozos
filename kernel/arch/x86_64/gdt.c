/* rozOS — GDT ও TSS তৈরি করে লোড করা
 *
 * Limine-এর দেওয়া GDT bootloader-reclaimable মেমোরিতে থাকে (PROTOCOL.md), তাই
 * PMM সেই মেমোরি ফেরত নেওয়ার আগেই কার্নেলের নিজস্ব GDT বসাতে হয়। */

#include <rozos/printf.h>
#include <stdint.h>
#include <stddef.h>
#include <rozos/string.h>
#include "gdt.h"

#define IST_STACK_SIZE 16384

/* access byte */
#define GDT_ACC_PRESENT   0x80
#define GDT_ACC_DPL3      0x60
#define GDT_ACC_CODE_DATA 0x10   /* S bit: code/data (system নয়) */
#define GDT_ACC_EXEC      0x08
#define GDT_ACC_RW        0x02   /* code: readable, data: writable */
#define GDT_ACC_TSS_AVAIL 0x09   /* 64-bit TSS (available) */
#define GDT_ACC_ACCESSED  0x01   /* ধাপ ১৯ঞ: আগেই বসানো — CPU যেন segment লোডের সময় GDT-তে
                                  * নিজে লিখতে না যায় (Linux-ও তাই করে; AMD-তে আসল PC-তে
                                  * iretq #GP 0x18-এর সন্দেহভাজন) */

/* flags nibble (byte 6-এর উপরের 4 বিট) */
#define GDT_FLAG_GRAN     0x8    /* limit 4 KiB এককে */
#define GDT_FLAG_SIZE32   0x4    /* data segment-এর জন্য */
#define GDT_FLAG_LONG     0x2    /* 64-bit code */

/* ৫টা সাধারণ descriptor + TSS-এর জন্য ২টা (16 বাইট) */
static uint64_t gdt[7] ROZ_ALIGNED(16);
static uint64_t gdt_expected[5];         /* ধাপ ১৫ক: শুরুর মান — নষ্ট হলে ধরা ও ঠিক করা */
static struct tss tss ROZ_ALIGNED(16);

static uint8_t ist_df_stack[IST_STACK_SIZE] ROZ_ALIGNED(16);
static uint8_t ist_nmi_stack[IST_STACK_SIZE] ROZ_ALIGNED(16);
static uint8_t ist_mc_stack[IST_STACK_SIZE] ROZ_ALIGNED(16);

static uint64_t make_descriptor(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFF);
    d |= (uint64_t)(base & 0xFFFFFF) << 16;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d |= (uint64_t)(flags & 0xF) << 52;
    d |= (uint64_t)((base >> 24) & 0xFF) << 56;
    return d;
}

static void set_tss_descriptor(int index, uint64_t base, uint32_t limit)
{
    gdt[index] = make_descriptor((uint32_t)(base & 0xFFFFFFFF), limit,
                                 GDT_ACC_PRESENT | GDT_ACC_TSS_AVAIL, 0);
    /* উপরের 8 বাইট: base-এর 63:32 */
    gdt[index + 1] = base >> 32;
}

void gdt_init(void)
{
    struct gdt_pointer ptr;

    gdt[0] = 0;
    gdt[1] = make_descriptor(0, 0xFFFFF,
                             GDT_ACC_PRESENT | GDT_ACC_CODE_DATA | GDT_ACC_EXEC | GDT_ACC_RW | GDT_ACC_ACCESSED,
                             GDT_FLAG_GRAN | GDT_FLAG_LONG);
    gdt[2] = make_descriptor(0, 0xFFFFF,
                             GDT_ACC_PRESENT | GDT_ACC_CODE_DATA | GDT_ACC_RW | GDT_ACC_ACCESSED,
                             GDT_FLAG_GRAN | GDT_FLAG_SIZE32);
    gdt[3] = make_descriptor(0, 0xFFFFF,
                             GDT_ACC_PRESENT | GDT_ACC_DPL3 | GDT_ACC_CODE_DATA | GDT_ACC_RW | GDT_ACC_ACCESSED,
                             GDT_FLAG_GRAN | GDT_FLAG_SIZE32);
    gdt[4] = make_descriptor(0, 0xFFFFF,
                             GDT_ACC_PRESENT | GDT_ACC_DPL3 | GDT_ACC_CODE_DATA | GDT_ACC_EXEC | GDT_ACC_RW | GDT_ACC_ACCESSED,
                             GDT_FLAG_GRAN | GDT_FLAG_LONG);

    memcpy(gdt_expected, gdt, sizeof(gdt_expected));

    memset(&tss, 0, sizeof(tss));
    tss.ist[IST_DOUBLE_FAULT - 1]  = (uint64_t)(uintptr_t)(ist_df_stack + IST_STACK_SIZE);
    tss.ist[IST_NMI - 1]           = (uint64_t)(uintptr_t)(ist_nmi_stack + IST_STACK_SIZE);
    tss.ist[IST_MACHINE_CHECK - 1] = (uint64_t)(uintptr_t)(ist_mc_stack + IST_STACK_SIZE);
    tss.iopb_offset = sizeof(struct tss);
    set_tss_descriptor(GDT_TSS / 8, (uint64_t)(uintptr_t)&tss, sizeof(struct tss) - 1);

    ptr.limit = sizeof(gdt) - 1;
    ptr.base  = (uint64_t)(uintptr_t)gdt;
    gdt_load(&ptr, GDT_KERNEL_CODE, GDT_KERNEL_DATA);
    tss_load(GDT_TSS);
}

int gdt_check_repair(void)
{
    struct gdt_pointer cur;
    int i, bad = 0;
    const uint64_t acc = 1ULL << 40;              /* "accessed" বিট — CPU নিজে বসায়, উপেক্ষা */
    __asm__ __volatile__("sgdt %0" : "=m"(cur));
    for (i = 0; i < 5; i++) {
        if ((gdt[i] | acc) != (gdt_expected[i] | acc)) {
            kprintf("  GDT[%d] = %016llx, expected %016llx - repaired\n", i,
                    (unsigned long long)gdt[i], (unsigned long long)gdt_expected[i]);
            gdt[i] = gdt_expected[i];
            bad = 1;
        }
    }
    if (cur.base != (uint64_t)(uintptr_t)gdt || cur.limit != sizeof(gdt) - 1) {
        struct gdt_pointer ptr;
        kprintf("  GDTR = %016llx/%x, expected %016llx/%x - reloaded\n", (unsigned long long)cur.base,
                cur.limit, (unsigned long long)(uintptr_t)gdt, (unsigned)(sizeof(gdt) - 1));
        ptr.limit = sizeof(gdt) - 1;
        ptr.base = (uint64_t)(uintptr_t)gdt;
        __asm__ __volatile__("lgdt %0" : : "m"(ptr) : "memory");
        bad = 1;
    }
    return bad;
}

void gdt_debug_break_user_data(void)
{
    gdt[3] &= ~(1ULL << 41);                      /* writable বিট মোছা — SS হিসেবে অচল (#GP 0x18) */
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    tss.rsp[0] = rsp0;
}
