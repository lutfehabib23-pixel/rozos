/* rozOS — PCI configuration space ও বাস স্ক্যান (rozos/pci.h দেখুন) */

#include <stdint.h>
#include <stddef.h>
#include <rozos/pci.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/timer.h>
#include <arch/x86_64/io.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static size_t device_count;

static uint32_t cfg_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (offset & 0xFC);
}

static uint32_t raw_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static void raw_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t v)
{
    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, v);
}

uint32_t pci_read32(const struct pci_device *d, uint8_t offset)
{
    return raw_read32(d->bus, d->slot, d->func, offset);
}

uint16_t pci_read16(const struct pci_device *d, uint8_t offset)
{
    return (uint16_t)(pci_read32(d, offset) >> ((offset & 2) * 8));
}

uint8_t pci_read8(const struct pci_device *d, uint8_t offset)
{
    return (uint8_t)(pci_read32(d, offset) >> ((offset & 3) * 8));
}

void pci_write32(const struct pci_device *d, uint8_t offset, uint32_t value)
{
    raw_write32(d->bus, d->slot, d->func, offset, value);
}

void pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value)
{
    uint32_t v = pci_read32(d, offset & 0xFC);
    unsigned shift = (offset & 2) * 8;
    v = (v & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_write32(d, offset & 0xFC, v);
}

void pci_enable_device(const struct pci_device *d)
{
    uint16_t cmd = pci_read16(d, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER;
    pci_write16(d, PCI_COMMAND, cmd);
}

/* ধাপ ২২ */
uint8_t pci_find_cap(const struct pci_device *d, uint8_t id)
{
    uint8_t p;
    int guard = 48;
    if (!(pci_read16(d, PCI_STATUS) & 0x0010)) {          /* capability তালিকা নেই */
        return 0;
    }
    p = (uint8_t)(pci_read8(d, 0x34) & 0xFC);
    while (p >= 0x40 && guard-- > 0) {
        if (pci_read8(d, p) == id) {
            return p;
        }
        p = (uint8_t)(pci_read8(d, (uint8_t)(p + 1)) & 0xFC);
    }
    return 0;
}

void pci_set_power_d0(const struct pci_device *d)
{
    uint8_t pm = pci_find_cap(d, 0x01);
    uint16_t csr;
    if (!pm) {
        return;
    }
    csr = pci_read16(d, (uint8_t)(pm + 4));
    if (csr & 3) {
        uint64_t t0;
        kprintf("[pci] %02x:%02x.%u: power state D%u -> D0\n", d->bus, d->slot, d->func, csr & 3);
        pci_write16(d, (uint8_t)(pm + 4), (uint16_t)(csr & ~3u));
        t0 = timer_uptime_ms();
        while (timer_uptime_ms() - t0 < 20) {               /* D3hot → D0: ১০ ms লাগে */
        }
    }
}

void pci_disable_aspm(const struct pci_device *d)
{
    uint8_t pe = pci_find_cap(d, 0x10);
    uint16_t lc;
    if (!pe) {
        return;
    }
    lc = pci_read16(d, (uint8_t)(pe + 0x10));               /* Link Control */
    if (lc & 3) {
        pci_write16(d, (uint8_t)(pe + 0x10), (uint16_t)(lc & ~3u));
    }
}

static void read_bars(struct pci_device *d)
{
    unsigned i;
    if ((d->header_type & 0x7F) != 0) {
        return;                                    /* শুধু সাধারণ ডিভাইসের (type 0) BAR */
    }
    for (i = 0; i < 6; i++) {
        uint32_t b = pci_read32(d, (uint8_t)(PCI_BAR0 + i * 4));
        if (b & 1) {
            d->bar_is_io[i] = 1;
            d->bar[i] = b & ~0x3u;
        } else {
            d->bar[i] = b & ~0xFu;
            if (((b >> 1) & 3) == 2 && i < 5) {    /* 64-বিট BAR: পরেরটায় উপরের অর্ধেক */
                d->bar[i] |= (uint64_t)pci_read32(d, (uint8_t)(PCI_BAR0 + (i + 1) * 4)) << 32;
                d->bar[++i] = 0;
            }
        }
    }
}

static void add_device(uint8_t bus, uint8_t slot, uint8_t func)
{
    struct pci_device *d;
    uint32_t id = raw_read32(bus, slot, func, PCI_VENDOR_ID);
    uint32_t cls;

    if ((id & 0xFFFF) == 0xFFFF || device_count >= PCI_MAX_DEVICES) {
        return;
    }
    d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor_id = (uint16_t)id;
    d->device_id = (uint16_t)(id >> 16);
    cls = raw_read32(bus, slot, func, PCI_CLASS_REV);
    d->revision = (uint8_t)cls;
    d->prog_if = (uint8_t)(cls >> 8);
    d->subclass = (uint8_t)(cls >> 16);
    d->class_code = (uint8_t)(cls >> 24);
    d->header_type = pci_read8(d, PCI_HEADER_TYPE);
    d->irq_line = pci_read8(d, PCI_INTERRUPT_LINE);
    d->irq_pin = pci_read8(d, PCI_INTERRUPT_PIN);
    read_bars(d);
}

int pci_init(void)
{
    unsigned bus, slot, func;

    device_count = 0;
    /* সরল brute-force স্ক্যান: সব বাস × ৩২ slot × (multi-function হলে) ৮ function */
    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            uint32_t id = raw_read32((uint8_t)bus, (uint8_t)slot, 0, PCI_VENDOR_ID);
            uint8_t ht;
            if ((id & 0xFFFF) == 0xFFFF) {
                continue;
            }
            ht = (uint8_t)(raw_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C) >> 16);
            add_device((uint8_t)bus, (uint8_t)slot, 0);
            if (ht & 0x80) {
                for (func = 1; func < 8; func++) {
                    add_device((uint8_t)bus, (uint8_t)slot, (uint8_t)func);
                }
            }
        }
    }
    return (int)device_count;
}

size_t pci_device_count(void) { return device_count; }

struct pci_device *pci_device_at(size_t index)
{
    return index < device_count ? &devices[index] : NULL;
}

struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, size_t n)
{
    size_t i;
    for (i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        if (d->class_code == class_code && d->subclass == subclass &&
            (prog_if == 0xFF || d->prog_if == prog_if)) {
            if (n == 0) {
                return d;
            }
            n--;
        }
    }
    return NULL;
}

struct pci_device *pci_find_id(uint16_t vendor, uint16_t device, size_t n)
{
    size_t i;
    for (i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor && devices[i].device_id == device) {
            if (n == 0) {
                return &devices[i];
            }
            n--;
        }
    }
    return NULL;
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c) {
    case 0x01:
        switch (s) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02: return s == 0x00 ? "Ethernet controller" : "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x05: return "memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-PCI bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x0C:
        switch (s) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    default: return "device";
    }
}
