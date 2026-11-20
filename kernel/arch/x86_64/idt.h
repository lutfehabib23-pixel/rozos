/* rozOS — IDT (Interrupt Descriptor Table) ও ইন্টারাপ্ট ডিসপ্যাচ
 *
 * ভেক্টর বিন্যাস:
 *   0x00-0x1F  CPU এক্সেপশন
 *   0x20-0x2F  লিগ্যাসি PIC-এর IRQ 0-15 (pic.c)
 *   0x30-0xFE  ভবিষ্যৎ: APIC টাইমার, IPI, MSI, syscall (int 0x80) ইত্যাদি
 *   0xFF       APIC spurious (ধাপ ৩) */
#ifndef ROZOS_ARCH_X86_64_IDT_H
#define ROZOS_ARCH_X86_64_IDT_H

#include <stdint.h>

#define IDT_ENTRIES         256
#define IDT_EXCEPTION_COUNT 32

/* এক্সেপশন ভেক্টর */
#define EXC_DIVIDE_ERROR        0
#define EXC_DEBUG               1
#define EXC_NMI                 2
#define EXC_BREAKPOINT          3
#define EXC_OVERFLOW            4
#define EXC_BOUND_RANGE         5
#define EXC_INVALID_OPCODE      6
#define EXC_DEVICE_NOT_AVAIL    7
#define EXC_DOUBLE_FAULT        8
#define EXC_INVALID_TSS         10
#define EXC_SEGMENT_NOT_PRESENT 11
#define EXC_STACK_FAULT         12
#define EXC_GENERAL_PROTECTION  13
#define EXC_PAGE_FAULT          14
#define EXC_X87_FP              16
#define EXC_ALIGNMENT_CHECK     17
#define EXC_MACHINE_CHECK       18
#define EXC_SIMD_FP             19
#define EXC_VIRTUALIZATION      20
#define EXC_CONTROL_PROTECTION  21
#define EXC_HYPERVISOR_INJECT   28
#define EXC_VMM_COMMUNICATION   29
#define EXC_SECURITY            30

/* isr.S যেভাবে স্ট্যাকে রাখে ঠিক সেই ক্রমে (নিচের ঠিকানা থেকে উপরে) */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;    /* CPU দেয়নি এমন ভেক্টরে 0 */
    /* CPU নিজে যা push করে */
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void idt_init(void);

/* নির্দিষ্ট ভেক্টরের জন্য C হ্যান্ডলার নিবন্ধন (এক্সেপশন বা নিজস্ব ভেক্টর) */
void idt_register_handler(uint8_t vector, interrupt_handler_t handler);

/* gate-এর ধরন বদলানো: ist = 0-7, dpl = 0 বা 3 (৩ হলে ইউজার-মোড থেকে int দেওয়া যায়) */
void idt_set_gate_options(uint8_t vector, uint8_t ist, uint8_t dpl);

/* রেজিস্টার ডাম্প (এক্সেপশন ও ডিবাগিং) */
void interrupt_dump_frame(const struct interrupt_frame *frame);

const char *exception_name(uint64_t vector);

/* isr.S থেকে ডাকা হয় */
void interrupt_dispatch(struct interrupt_frame *frame);

#endif
