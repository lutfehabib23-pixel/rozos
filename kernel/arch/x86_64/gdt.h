/* rozOS — GDT (Global Descriptor Table) ও TSS (Task State Segment)
 *
 * সেগমেন্ট বিন্যাস — ক্রমটা ইচ্ছাকৃত: ধাপ ৬-এর syscall/sysret নির্দেশ ধরে নেয়
 * user data ঠিক user code-এর আগে থাকে (STAR MSR: sysret CS = base+16, SS = base+8)।
 *
 *   0x00  null
 *   0x08  kernel code (64-bit, DPL 0)
 *   0x10  kernel data (DPL 0)
 *   0x18  user data   (DPL 3)  → selector 0x1B
 *   0x20  user code   (64-bit, DPL 3) → selector 0x23
 *   0x28  TSS (16 বাইটের descriptor)
 */
#ifndef ROZOS_ARCH_X86_64_GDT_H
#define ROZOS_ARCH_X86_64_GDT_H

#include <stdint.h>
#include <rozos/compiler.h>

#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x18
#define GDT_USER_CODE    0x20
#define GDT_TSS          0x28

#define GDT_RPL_USER     3
#define GDT_USER_DATA_SEL (GDT_USER_DATA | GDT_RPL_USER)
#define GDT_USER_CODE_SEL (GDT_USER_CODE | GDT_RPL_USER)

/* IST (Interrupt Stack Table) নম্বর — এই এক্সেপশনগুলো সবসময় আলাদা, নিশ্চিতভাবে
 * ভালো স্ট্যাকে চলে, যাতে কার্নেল স্ট্যাক নষ্ট হলেও হ্যান্ডলার কাজ করে */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MACHINE_CHECK 3

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];        /* Ring 0-2-এ ঢোকার সময় স্ট্যাক (rsp0 = ইউজার→কার্নেল) */
    uint64_t reserved1;
    uint64_t ist[7];        /* IST1..IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   /* I/O permission bitmap নেই → sizeof(struct tss) */
} ROZ_PACKED;

void gdt_init(void);

/* ধাপ ৬: প্রতিটি ইউজার প্রসেসে সুইচ করার সময় তার কার্নেল স্ট্যাক বসানো */
void tss_set_kernel_stack(uint64_t rsp0);

/* gdt_flush.S */
struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} ROZ_PACKED;

void gdt_load(const struct gdt_pointer *ptr, uint16_t code_sel, uint16_t data_sel);
void tss_load(uint16_t sel);

#endif
