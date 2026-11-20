/* rozOS — Local APIC, I/O APIC ও LAPIC টাইমার
 *
 * ACPI MADT থেকে LAPIC/I-O APIC-এর ঠিকানা, ISA IRQ override আর NMI তথ্য নেওয়া হয়।
 * APIC মোডে ISA IRQ 0-15 একই ভেক্টরে (0x20-0x2F) আসে, তাই pic.c-এর
 * irq_register() API বদলায় না — শুধু mask/unmask আর EOI এখানে চলে আসে।
 * MADT বা I/O APIC না পেলে apic_init() 0 দেয় এবং 8259 PIC আগের মতোই চলে। */
#ifndef ROZOS_ARCH_X86_64_APIC_H
#define ROZOS_ARCH_X86_64_APIC_H

#include <stdint.h>

#define APIC_VECTOR_TIMER     0x30
#define APIC_VECTOR_ERROR     0x31
#define APIC_VECTOR_SPURIOUS  0xFF

#define APIC_MAX_CPUS     64
#define APIC_MAX_IOAPICS  8

struct apic_info {
    uint64_t lapic_phys;
    uint32_t bsp_lapic_id;
    int      x2apic;
    uint32_t cpu_count;                  /* MADT-এ চালুযোগ্য CPU */
    uint32_t cpu_lapic_ids[APIC_MAX_CPUS];
    uint32_t ioapic_count;
    struct {
        uint8_t  id;
        uint64_t phys;
        uint32_t gsi_base;
        uint32_t redirections;           /* কতগুলো ইনপুট পিন */
    } ioapics[APIC_MAX_IOAPICS];
    uint32_t isa_gsi[16];                /* ISA IRQ → GSI (override সহ) */
    uint16_t isa_flags[16];              /* MPS INTI flags (polarity/trigger) */
    uint32_t lapic_timer_ticks_per_ms;   /* PIT দিয়ে মাপা (divide 16) */
    uint32_t lapic_version;
};

/* MADT পার্স, LAPIC ও I/O APIC চালু, PIC বন্ধ। শর্ত: vmm_init ও acpi_init হয়ে গেছে।
 * সফল হলে 1, APIC ব্যবহার করা না গেলে 0 (তখন PIC চালু থাকে)। */
int  apic_init(void);
int  apic_active(void);

/* PIT দিয়ে মেপে LAPIC টাইমারকে hz-এ periodic চালু করে টিকের উৎস বানায়।
 * ইন্টারাপ্ট চালু থাকতে হবে। সফল হলে 1। */
int  apic_timer_init(uint32_t hz);
int  apic_timer_active(void);

void lapic_eoi(void);
uint32_t lapic_id(void);

/* ISA IRQ-এর I/O APIC এন্ট্রি mask/unmask (pic.c থেকে) */
void ioapic_set_irq_masked(uint8_t isa_irq, int masked);

const struct apic_info *apic_get_info(void);
uint64_t apic_error_count(void);
uint64_t apic_spurious_count(void);

#endif
