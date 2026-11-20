/* rozOS — GDT ও TSS তৈরি করে লোড করা
 *
 * Limine-এর দেওয়া GDT bootloader-reclaimable মেমোরিতে থাকে (PROTOCOL.md), তাই
 * PMM সেই মেমোরি ফেরত নেওয়ার আগেই কার্নেলের নিজস্ব GDT বসাতে হয়। */

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

/* flags nibble (byte 6-এর উপরের 4 বিট) */
#define GDT_FLAG_GRAN     0x8    /* limit 4 KiB এককে */
#define GDT_FLAG_SIZE32   0x4    /* data segment-এর জন্য */
#define GDT_FLAG_LONG     0x2    /* 64-bit code */

/* ৫টা সাধারণ descriptor + TSS-এর জন্য ২টা (16 বাইট) */
static uint64_t gdt[7] ROZ_ALIGNED(16);
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
                             GDT_ACC_PRESENT | GDT_ACC_CODE_DATA | GDT_ACC_EXEC | GDT_ACC_RW,
                             GDT_FLAG_GRAN | GDT_FLAG_LONG);
    gdt[2] = make_descriptor(0, 0xFFFFF,
                             GDT_ACC_PRESENT | GDT_ACC_CODE_DATA | GDT_ACC_RW,
                             GDT_FLAG_GRAN | GDT_FLAG_SIZE32);
    gdt[3] = make_descriptor(0, 0xFFFFF,
                             GDT_ACC_PRESENT | GDT_ACC_DPL3 | GDT_ACC_CODE_DATA | GDT_ACC_RW,
                             GDT_FLAG_GRAN | GDT_FLAG_SIZE32);
    gdt[4] = make_descriptor(0, 0xFFFFF,
                             GDT_ACC_PRESENT | GDT_ACC_DPL3 | GDT_ACC_CODE_DATA | GDT_ACC_EXEC | GDT_ACC_RW,
                             GDT_FLAG_GRAN | GDT_FLAG_LONG);

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

void tss_set_kernel_stack(uint64_t rsp0)
{
    tss.rsp[0] = rsp0;
}
