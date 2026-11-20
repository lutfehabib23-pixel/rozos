/* rozOS — Local APIC, I/O APIC ও LAPIC টাইমার (apic.h দেখুন) */

#include <stdint.h>
#include <stddef.h>
#include <rozos/acpi.h>
#include <rozos/vmm.h>
#include <rozos/string.h>
#include <rozos/compiler.h>
#include "apic.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "cpu.h"

/* ---------- MADT ---------- */

struct madt {
    struct acpi_sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;                      /* বিট 0: PC-AT 8259 আছে */
} ROZ_PACKED;

#define MADT_LAPIC            0
#define MADT_IOAPIC           1
#define MADT_ISO              2          /* interrupt source override */
#define MADT_LAPIC_NMI        4
#define MADT_LAPIC_ADDR_OVR   5
#define MADT_X2APIC           9
#define MADT_X2APIC_NMI       0x0A

/* ---------- LAPIC রেজিস্টার (xAPIC MMIO offset; x2APIC MSR = 0x800 + offset/16) ---------- */

#define LAPIC_ID          0x020
#define LAPIC_VERSION     0x030
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SVR         0x0F0
#define LAPIC_ESR         0x280
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_CUR   0x390
#define LAPIC_TIMER_DIV   0x3E0

#define LVT_MASKED        (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)
#define LVT_DELIVERY_NMI  (4u << 8)
#define LVT_ACTIVE_LOW    (1u << 13)
#define LVT_LEVEL         (1u << 15)
#define SVR_ENABLE        (1u << 8)
#define TIMER_DIV_16      0x3

#define MSR_IA32_APIC_BASE 0x1B
#define APIC_BASE_X2APIC   (1ULL << 10)

/* ---------- I/O APIC ---------- */

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REDIR(n) (0x10 + 2 * (n))
#define IOAPIC_MASKED   (1ULL << 16)

static struct apic_info info;
static volatile uint8_t *lapic_mmio;
static volatile uint8_t *ioapic_mmio[APIC_MAX_IOAPICS];
static int active, timer_active;
static volatile uint64_t error_count, spurious_count;

/* ---------- LAPIC অ্যাক্সেস ---------- */

static uint32_t lapic_read(uint32_t reg)
{
    if (info.x2apic) {
        return (uint32_t)cpu_rdmsr(0x800 + (reg >> 4));
    }
    return *(volatile uint32_t *)(lapic_mmio + reg);
}

static void lapic_write(uint32_t reg, uint32_t val)
{
    if (info.x2apic) {
        cpu_wrmsr(0x800 + (reg >> 4), val);
        return;
    }
    *(volatile uint32_t *)(lapic_mmio + reg) = val;
}

void lapic_eoi(void)
{
    if (active) {
        lapic_write(LAPIC_EOI, 0);
    }
}

uint32_t lapic_id(void)
{
    uint32_t v = lapic_read(LAPIC_ID);
    return info.x2apic ? v : (v >> 24);
}

/* ---------- I/O APIC অ্যাক্সেস ---------- */

static uint32_t ioapic_read(unsigned i, uint32_t reg)
{
    *(volatile uint32_t *)(ioapic_mmio[i] + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t *)(ioapic_mmio[i] + IOAPIC_WIN);
}

static void ioapic_write(unsigned i, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(ioapic_mmio[i] + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t *)(ioapic_mmio[i] + IOAPIC_WIN) = val;
}

/* GSI কোন I/O APIC-এর কোন পিন */
static int ioapic_for_gsi(uint32_t gsi, unsigned *which, uint32_t *pin)
{
    unsigned i;
    for (i = 0; i < info.ioapic_count; i++) {
        if (gsi >= info.ioapics[i].gsi_base &&
            gsi < info.ioapics[i].gsi_base + info.ioapics[i].redirections) {
            *which = i;
            *pin = gsi - info.ioapics[i].gsi_base;
            return 1;
        }
    }
    return 0;
}

static void ioapic_write_entry(unsigned i, uint32_t pin, uint64_t entry)
{
    /* আগে mask করা নিচের অর্ধেক, তারপর উপরের, শেষে আসল নিচের অর্ধেক */
    ioapic_write(i, IOAPIC_REDIR(pin), (uint32_t)IOAPIC_MASKED);
    ioapic_write(i, IOAPIC_REDIR(pin) + 1, (uint32_t)(entry >> 32));
    ioapic_write(i, IOAPIC_REDIR(pin), (uint32_t)entry);
}

/* ISA IRQ-এর redirection entry বানানো (MADT-এর polarity/trigger মেনে) */
static uint64_t isa_entry(uint8_t irq, int masked)
{
    uint16_t f = info.isa_flags[irq];
    uint64_t e = (uint64_t)(PIC_VECTOR_BASE + irq);       /* fixed delivery, physical dest */
    unsigned polarity = f & 0x3, trigger = (f >> 2) & 0x3;

    if (polarity == 0x3) e |= (1ULL << 13);                /* active low */
    if (trigger == 0x3)  e |= (1ULL << 15);                /* level */
    if (masked)          e |= IOAPIC_MASKED;
    e |= (uint64_t)(info.bsp_lapic_id & 0xFF) << 56;
    return e;
}

void ioapic_set_irq_masked(uint8_t isa_irq, int masked)
{
    unsigned which;
    uint32_t pin;
    if (!active || isa_irq >= 16 || !ioapic_for_gsi(info.isa_gsi[isa_irq], &which, &pin)) {
        return;
    }
    ioapic_write_entry(which, pin, isa_entry(isa_irq, masked));
}

/* ---------- হ্যান্ডলার ---------- */

static void spurious_handler(struct interrupt_frame *f)
{
    (void)f;
    spurious_count++;                    /* spurious-এ EOI দিতে হয় না */
}

static void error_handler(struct interrupt_frame *f)
{
    (void)f;
    lapic_write(LAPIC_ESR, 0);           /* ESR পড়ার আগে লেখা লাগে */
    (void)lapic_read(LAPIC_ESR);
    error_count++;
    lapic_eoi();
}

static void timer_handler(struct interrupt_frame *f)
{
    (void)f;
    timer_core_tick();
    lapic_eoi();
}

/* ---------- MADT পার্স ---------- */

static int parse_madt(void)
{
    const struct madt *m = (const struct madt *)acpi_find_table("APIC", 0);
    const uint8_t *p, *end;
    unsigned i;

    if (!m) {
        return 0;
    }
    info.lapic_phys = m->lapic_address;
    for (i = 0; i < 16; i++) {
        info.isa_gsi[i] = i;             /* override না থাকলে ISA IRQ = GSI */
        info.isa_flags[i] = 0;
    }

    p = (const uint8_t *)m + sizeof(*m);
    end = (const uint8_t *)m + m->header.length;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        uint8_t type = p[0];
        switch (type) {
        case MADT_LAPIC: {
            uint32_t flags;
            memcpy(&flags, p + 4, 4);
            if ((flags & 0x3) && info.cpu_count < APIC_MAX_CPUS) {   /* enabled বা online-capable */
                info.cpu_lapic_ids[info.cpu_count++] = p[3];
            }
            break;
        }
        case MADT_X2APIC: {
            uint32_t id, flags;
            memcpy(&id, p + 4, 4);
            memcpy(&flags, p + 8, 4);
            if ((flags & 0x3) && info.cpu_count < APIC_MAX_CPUS) {
                info.cpu_lapic_ids[info.cpu_count++] = id;
            }
            break;
        }
        case MADT_IOAPIC:
            if (info.ioapic_count < APIC_MAX_IOAPICS) {
                uint32_t addr, gsi;
                memcpy(&addr, p + 4, 4);
                memcpy(&gsi, p + 8, 4);
                info.ioapics[info.ioapic_count].id = p[2];
                info.ioapics[info.ioapic_count].phys = addr;
                info.ioapics[info.ioapic_count].gsi_base = gsi;
                info.ioapic_count++;
            }
            break;
        case MADT_ISO:
            if (p[2] == 0 && p[3] < 16) {                     /* bus 0 = ISA */
                uint32_t gsi;
                uint16_t flags;
                memcpy(&gsi, p + 4, 4);
                memcpy(&flags, p + 8, 2);
                info.isa_gsi[p[3]] = gsi;
                info.isa_flags[p[3]] = flags;
            }
            break;
        case MADT_LAPIC_ADDR_OVR: {
            uint64_t addr;
            memcpy(&addr, p + 4, 8);
            info.lapic_phys = addr;
            break;
        }
        default:
            break;                                            /* NMI এন্ট্রি নিচে আলাদা */
        }
        p += p[1];
    }
    return info.ioapic_count > 0;
}

/* MADT-এর LAPIC NMI এন্ট্রি অনুযায়ী LINT0/LINT1 কনফিগার */
static void setup_lint_nmi(const struct madt *m)
{
    const uint8_t *p = (const uint8_t *)m + sizeof(*m);
    const uint8_t *end = (const uint8_t *)m + m->header.length;
    uint32_t me = info.bsp_lapic_id;

    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        uint32_t target = 0;
        uint16_t flags = 0;
        uint8_t lint = 0;
        int match = 0;

        if (p[0] == MADT_LAPIC_NMI) {
            memcpy(&flags, p + 3, 2);
            lint = p[5];
            match = (p[2] == 0xFF);                           /* সব প্রসেসর */
            if (!match) {
                /* ACPI processor UID → এখানে সরলতার জন্য একক CPU ধরে নেওয়া */
                match = 1;
            }
        } else if (p[0] == MADT_X2APIC_NMI) {
            memcpy(&flags, p + 2, 2);
            memcpy(&target, p + 4, 4);
            lint = p[8];
            match = (target == 0xFFFFFFFFu || target == me);
        }
        if (match && (p[0] == MADT_LAPIC_NMI || p[0] == MADT_X2APIC_NMI)) {
            uint32_t v = LVT_DELIVERY_NMI;
            if ((flags & 0x3) == 0x3) v |= LVT_ACTIVE_LOW;
            if (((flags >> 2) & 0x3) == 0x3) v |= LVT_LEVEL;
            lapic_write(lint ? LAPIC_LVT_LINT1 : LAPIC_LVT_LINT0, v);
        }
        p += p[1];
    }
}

/* ---------- চালু করা ---------- */

int apic_init(void)
{
    uint64_t flags;
    unsigned i, irq;

    memset(&info, 0, sizeof(info));
    active = 0;
    if (!acpi_available() || !parse_madt()) {
        return 0;
    }

    info.x2apic = (cpu_rdmsr(MSR_IA32_APIC_BASE) & APIC_BASE_X2APIC) != 0;
    if (!info.x2apic) {
        lapic_mmio = (volatile uint8_t *)vmm_map_mmio(info.lapic_phys, 0x1000);
        if (!lapic_mmio) {
            return 0;
        }
    }
    for (i = 0; i < info.ioapic_count; i++) {
        ioapic_mmio[i] = (volatile uint8_t *)vmm_map_mmio(info.ioapics[i].phys, 0x1000);
        if (!ioapic_mmio[i]) {
            return 0;
        }
        info.ioapics[i].redirections = ((ioapic_read(i, IOAPIC_REG_VER) >> 16) & 0xFF) + 1;
    }

    flags = cpu_irq_save();

    info.bsp_lapic_id = lapic_id();
    info.lapic_version = lapic_read(LAPIC_VERSION) & 0xFF;

    /* IDT হ্যান্ডলার */
    idt_register_handler(APIC_VECTOR_SPURIOUS, spurious_handler);
    idt_register_handler(APIC_VECTOR_ERROR, error_handler);
    idt_register_handler(APIC_VECTOR_TIMER, timer_handler);

    /* LAPIC: সফটওয়্যার-চালু, সব priority গ্রহণ, LINT0 (PIC ExtINT) বন্ধ */
    lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_VECTOR_SPURIOUS);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED | LVT_DELIVERY_NMI);
    setup_lint_nmi((const struct madt *)acpi_find_table("APIC", 0));
    lapic_write(LAPIC_LVT_ERROR, APIC_VECTOR_ERROR);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* I/O APIC: সব পিন mask */
    for (i = 0; i < info.ioapic_count; i++) {
        uint32_t pin;
        for (pin = 0; pin < info.ioapics[i].redirections; pin++) {
            ioapic_write_entry(i, pin, IOAPIC_MASKED | (APIC_VECTOR_SPURIOUS - 1));
        }
    }

    /* 8259 সম্পূর্ণ বন্ধ, তারপর যেসব ISA IRQ-এর হ্যান্ডলার আছে সেগুলো I/O APIC-এ খোলা */
    pic_disable();
    active = 1;
    for (irq = 0; irq < 16; irq++) {
        if (irq == IRQ_CASCADE) {
            continue;
        }
        ioapic_set_irq_masked((uint8_t)irq, !irq_has_handler((uint8_t)irq));
    }
    pic_set_apic_mode(1);

    cpu_irq_restore(flags);
    return 1;
}

int apic_timer_init(uint32_t hz)
{
    uint32_t elapsed, initial;
    uint64_t flags;
    const uint32_t calib_ms = 50;

    if (!active || hz == 0) {
        return 0;
    }

    /* ১. PIT দিয়ে মাপা: divide 16, এক-শট, masked, সর্বোচ্চ মান থেকে কতটা কমে */
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    pit_wait_ms(calib_ms);
    elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);

    info.lapic_timer_ticks_per_ms = elapsed / calib_ms;
    if (info.lapic_timer_ticks_per_ms == 0) {
        return 0;
    }
    initial = (uint32_t)((uint64_t)info.lapic_timer_ticks_per_ms * 1000 / hz);
    if (initial == 0) {
        return 0;
    }

    /* ২. periodic চালু করে টিকের উৎস বদলানো; PIT থামানো */
    flags = cpu_irq_save();
    pit_stop();
    timer_core_switch_source("LAPIC", 1000000000000ULL / hz);
    lapic_write(LAPIC_LVT_TIMER, APIC_VECTOR_TIMER | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, initial);
    timer_active = 1;
    cpu_irq_restore(flags);
    return 1;
}

int apic_active(void)       { return active; }
int apic_timer_active(void) { return timer_active; }
const struct apic_info *apic_get_info(void) { return &info; }
uint64_t apic_error_count(void)    { return error_count; }
uint64_t apic_spurious_count(void) { return spurious_count; }
