/* rozOS — PCI বাস (configuration space, ডিভাইস খোঁজা)
 *
 * লিগ্যাসি I/O পোর্ট পদ্ধতি (0xCF8 ঠিকানা, 0xCFC ডেটা) — সব x86 মেশিনে আছে।
 * বুটে একবার সব বাস স্ক্যান করে ডিভাইসের তালিকা রাখা হয়; ড্রাইভাররা
 * class/vendor দিয়ে খুঁজে নেয় (AHCI: 01:06:01, পরে নেটওয়ার্ক: 02:00)। */
#ifndef ROZOS_PCI_H
#define ROZOS_PCI_H

#include <stdint.h>
#include <stddef.h>

#define PCI_MAX_DEVICES 128   /* ধাপ ২২: আসল PC-তে ৬৪-র বেশি function থাকতে পারে */

/* configuration space-এর কিছু রেজিস্টার */
#define PCI_VENDOR_ID     0x00
#define PCI_DEVICE_ID     0x02
#define PCI_COMMAND       0x04
#define PCI_STATUS        0x06
#define PCI_CLASS_REV     0x08
#define PCI_HEADER_TYPE   0x0E
#define PCI_BAR0          0x10
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN 0x3D

#define PCI_CMD_IO        0x0001
#define PCI_CMD_MEMORY    0x0002
#define PCI_CMD_BUS_MASTER 0x0004
#define PCI_CMD_INTX_DISABLE 0x0400

struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;
    uint8_t  irq_line, irq_pin;
    uint64_t bar[6];          /* ঠিকানা (টাইপ বিট বাদে); 64-বিট BAR হলে পরেরটা 0 */
    uint8_t  bar_is_io[6];
};

int    pci_init(void);                      /* স্ক্যান; পাওয়া ডিভাইসের সংখ্যা */
size_t pci_device_count(void);
struct pci_device *pci_device_at(size_t index);
/* class/subclass/prog_if মেলে এমন n-তম ডিভাইস (prog_if = 0xFF হলে যেকোনো) */
struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, size_t n);
struct pci_device *pci_find_id(uint16_t vendor, uint16_t device, size_t n);

uint32_t pci_read32(const struct pci_device *d, uint8_t offset);
uint16_t pci_read16(const struct pci_device *d, uint8_t offset);
uint8_t  pci_read8(const struct pci_device *d, uint8_t offset);
void     pci_write32(const struct pci_device *d, uint8_t offset, uint32_t value);
void     pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value);

/* memory decoding + bus mastering (DMA) চালু */
void pci_enable_device(const struct pci_device *d);

/* ধাপ ২২: capability তালিকায় id খোঁজা (0 = নেই); PM দিয়ে D0-তে আনা; PCIe ASPM বন্ধ */
uint8_t pci_find_cap(const struct pci_device *d, uint8_t id);
void    pci_set_power_d0(const struct pci_device *d);
void    pci_disable_aspm(const struct pci_device *d);

const char *pci_class_name(uint8_t class_code, uint8_t subclass);

#endif
