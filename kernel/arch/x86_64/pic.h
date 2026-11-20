/* rozOS — লিগ্যাসি 8259 PIC (দুটো, cascade) ও IRQ হ্যান্ডলার নিবন্ধন
 *
 * PROTOCOL.md: Limine সব PIC IRQ mask করে দেয়। আমরা IRQ 0-15-কে ভেক্টর
 * 0x20-0x2F-এ সরাই (ডিফল্ট 0x08-0x0F CPU এক্সেপশনের সাথে মিলে যায়)।
 * ধাপ ৩-এ APIC চালু হলে PIC পুরো mask করে দেওয়া হবে; irq_register() API একই থাকবে। */
#ifndef ROZOS_ARCH_X86_64_PIC_H
#define ROZOS_ARCH_X86_64_PIC_H

#include <stdint.h>
#include "idt.h"

#define PIC_VECTOR_BASE 0x20
#define PIC_IRQ_COUNT   16

/* প্রচলিত IRQ নম্বর */
#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1
#define IRQ_CASCADE   2
#define IRQ_COM2      3
#define IRQ_COM1      4
#define IRQ_RTC       8
#define IRQ_MOUSE     12
#define IRQ_ATA_PRIMARY   14
#define IRQ_ATA_SECONDARY 15

typedef void (*irq_handler_t)(uint8_t irq, struct interrupt_frame *frame);

/* PROTOCOL.md (base revision 5+): Limine Local APIC চালু রাখে কিন্তু তার LINT0
 * mask করে দেয়। লিগ্যাসি PIC-এর ইন্টারাপ্ট CPU-তে আসে LINT0 (ExtINT মোড) দিয়েই।
 * তাই pic_init() early_map দিয়ে LAPIC-এর MMIO পেজ ম্যাপ করে LINT0-কে
 * ExtINT, unmasked করে দেয় (x2APIC মোডে থাকলে MSR দিয়ে)।
 * ExtINT-এ আসা ইন্টারাপ্টে LAPIC-এর EOI লাগে না — শুধু PIC-এর EOI।
 * পুরো APIC ড্রাইভার (টাইমার, I/O APIC, IPI) আসবে ধাপ ৩-এ। */
int      pic_lapic_routed(void);      /* LINT0 সফলভাবে খোলা হয়েছে কিনা */
uint64_t pic_lapic_phys_base(void);   /* LAPIC-এর ফিজিক্যাল ঠিকানা (0 = নেই) */

void pic_init(void);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_disable(void);              /* সব mask (APIC-এ যাওয়ার সময়) */
void pic_send_eoi(uint8_t irq);
uint16_t pic_get_isr(void);

/* হ্যান্ডলার নিবন্ধন করে IRQ unmask করে (handler = NULL হলে mask)।
 * APIC মোডে mask/unmask I/O APIC-এ হয় — ডাকার নিয়ম একই থাকে। */
void irq_register(uint8_t irq, irq_handler_t handler);
int  irq_has_handler(uint8_t irq);

/* apic.c ডাকে: 1 = এখন থেকে EOI LAPIC-এ, mask/unmask I/O APIC-এ; 8259 বন্ধ */
void pic_set_apic_mode(int on);
int  pic_apic_mode(void);
uint64_t irq_count(uint8_t irq);     /* প্রতিটি IRQ কতবার এসেছে */
uint64_t irq_spurious_count(void);

/* idt.c থেকে */
void pic_dispatch(struct interrupt_frame *frame);

#endif
