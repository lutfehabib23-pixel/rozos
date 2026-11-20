/* rozOS — 8259 PIC ড্রাইভার */

#include <stdint.h>
#include <stddef.h>
#include <rozos/early_map.h>
#include "pic.h"
#include "apic.h"
#include "io.h"
#include "cpu.h"

#define MSR_IA32_APIC_BASE      0x1B
#define APIC_BASE_X2APIC        (1ULL << 10)
#define APIC_BASE_GLOBAL_ENABLE (1ULL << 11)
#define APIC_BASE_ADDR_MASK     0x000FFFFFFFFFF000ULL

#define LAPIC_REG_LVT_LINT0     0x350       /* xAPIC MMIO offset */
#define X2APIC_MSR_LVT_LINT0    0x835       /* x2APIC MSR */
#define LVT_MASKED              (1u << 16)
#define LVT_LEVEL_TRIGGER       (1u << 15)
#define LVT_DELIVERY_MASK       (7u << 8)
#define LVT_DELIVERY_EXTINT     (7u << 8)

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_ICW4  0x01
#define ICW1_INIT  0x10
#define ICW4_8086  0x01
#define OCW3_READ_ISR 0x0B
#define PIC_EOI    0x20

static irq_handler_t irq_handlers[PIC_IRQ_COUNT];
static volatile uint64_t irq_counts[PIC_IRQ_COUNT];
static volatile uint64_t spurious_count;
static int apic_mode;
static int lapic_routed;
static uint64_t lapic_phys;

static uint32_t lint0_extint(uint32_t old)
{
    /* delivery mode ExtINT, edge, unmasked; বাকি বিট (polarity ইত্যাদি) যেমন ছিল */
    old &= ~(LVT_MASKED | LVT_LEVEL_TRIGGER | LVT_DELIVERY_MASK);
    return old | LVT_DELIVERY_EXTINT;
}

/* LAPIC-এর LINT0 খুলে PIC-কে CPU পর্যন্ত পৌঁছানোর পথ করে দেওয়া (pic.h দেখুন) */
static void route_pic_through_lapic(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint64_t base;

    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (!(edx & (1u << 9))) {
        lapic_routed = 1;               /* Local APIC নেই — PIC সরাসরি CPU-র INTR পিনে */
        return;
    }
    base = cpu_rdmsr(MSR_IA32_APIC_BASE);
    lapic_phys = base & APIC_BASE_ADDR_MASK;
    if (!(base & APIC_BASE_GLOBAL_ENABLE)) {
        lapic_routed = 1;               /* APIC বন্ধ — LINT0 নিজেই INTR হিসেবে কাজ করে */
        return;
    }

    if (base & APIC_BASE_X2APIC) {
        uint64_t lvt = cpu_rdmsr(X2APIC_MSR_LVT_LINT0);
        cpu_wrmsr(X2APIC_MSR_LVT_LINT0, lint0_extint((uint32_t)lvt));
        lapic_routed = 1;
        return;
    }

    {
        volatile uint8_t *regs = (volatile uint8_t *)early_map_mmio(lapic_phys, PAGE_SIZE);
        volatile uint32_t *lint0;
        if (!regs) {
            return;                     /* ম্যাপ করা যায়নি — kmain রিপোর্ট করবে */
        }
        lint0 = (volatile uint32_t *)(regs + LAPIC_REG_LVT_LINT0);
        *lint0 = lint0_extint(*lint0);
        lapic_routed = 1;
    }
}

int pic_lapic_routed(void)
{
    return lapic_routed;
}

uint64_t pic_lapic_phys_base(void)
{
    return lapic_phys;
}

void pic_init(void)
{
    /* ICW1: initialization শুরু, ICW4 লাগবে */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    /* ICW2: ভেক্টর offset */
    outb(PIC1_DATA, PIC_VECTOR_BASE);       io_wait();
    outb(PIC2_DATA, PIC_VECTOR_BASE + 8);   io_wait();
    /* ICW3: master-এর IRQ2-তে slave; slave-এর cascade identity = 2 */
    outb(PIC1_DATA, 0x04);                  io_wait();
    outb(PIC2_DATA, 0x02);                  io_wait();
    /* ICW4: 8086 মোড */
    outb(PIC1_DATA, ICW4_8086);             io_wait();
    outb(PIC2_DATA, ICW4_8086);             io_wait();

    /* সব mask; শুধু cascade (IRQ2) খোলা যাতে slave-এর IRQ পৌঁছাতে পারে */
    outb(PIC1_DATA, (uint8_t)~(1u << IRQ_CASCADE));
    outb(PIC2_DATA, 0xFF);

    route_pic_through_lapic();
}

void pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1u << (irq & 7));
    outb(port, inb(port) | bit);
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1u << (irq & 7));
    outb(port, inb(port) & (uint8_t)~bit);
}

void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

uint16_t pic_get_isr(void)
{
    outb(PIC1_CMD, OCW3_READ_ISR);
    outb(PIC2_CMD, OCW3_READ_ISR);
    return (uint16_t)((inb(PIC2_CMD) << 8) | inb(PIC1_CMD));
}

void irq_register(uint8_t irq, irq_handler_t handler)
{
    if (irq >= PIC_IRQ_COUNT) {
        return;
    }
    irq_handlers[irq] = handler;
    if (apic_mode) {
        ioapic_set_irq_masked(irq, handler == NULL);
    } else if (handler) {
        pic_unmask(irq);
    } else {
        pic_mask(irq);
    }
}

int irq_has_handler(uint8_t irq)
{
    return irq < PIC_IRQ_COUNT && irq_handlers[irq] != NULL;
}

void pic_set_apic_mode(int on)
{
    apic_mode = on;
}

int pic_apic_mode(void)
{
    return apic_mode;
}

uint64_t irq_count(uint8_t irq)
{
    return irq < PIC_IRQ_COUNT ? irq_counts[irq] : 0;
}

uint64_t irq_spurious_count(void)
{
    return spurious_count;
}

void pic_dispatch(struct interrupt_frame *frame)
{
    uint8_t irq = (uint8_t)(frame->vector - PIC_VECTOR_BASE);

    /* APIC মোড: I/O APIC থেকে আসে — 8259-এর spurious নিয়ম খাটে না, EOI LAPIC-এ */
    if (apic_mode) {
        irq_counts[irq]++;
        if (irq_handlers[irq]) {
            irq_handlers[irq](irq, frame);
        }
        lapic_eoi();
        return;
    }

    /* spurious IRQ 7/15: ISR-এ আসলে বিট সেট না থাকলে এটা ভুয়া ইন্টারাপ্ট */
    if (irq == 7 && !(pic_get_isr() & (1u << 7))) {
        spurious_count++;
        return;                                   /* EOI নয় */
    }
    if (irq == 15 && !(pic_get_isr() & (1u << 15))) {
        spurious_count++;
        outb(PIC1_CMD, PIC_EOI);                  /* শুধু master-কে EOI */
        return;
    }

    irq_counts[irq]++;
    if (irq_handlers[irq]) {
        irq_handlers[irq](irq, frame);
    }
    pic_send_eoi(irq);
}
