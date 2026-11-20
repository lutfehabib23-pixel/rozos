/* rozOS — AHCI (SATA) ডিস্ক ড্রাইভার
 *
 * PCI class 01:06:01 কন্ট্রোলার খুঁজে প্রতিটি চালু SATA পোর্টের ডিস্ককে ব্লক
 * ডিভাইস (sda, sdb ...) হিসেবে নিবন্ধন করে। কমান্ড: IDENTIFY, READ/WRITE DMA EXT
 * (LBA48), FLUSH CACHE EXT। এখন polling (ইন্টারাপ্ট ছাড়া) — শিডিউলার এলে (ধাপ ৬)
 * ইন্টারাপ্ট-চালিত করা হবে। ডেটা যায় প্রতি পোর্টের একটা DMA bounce buffer দিয়ে,
 * তাই কলারের বাফার যেকোনো কার্নেল ঠিকানায় থাকতে পারে। */
#ifndef ROZOS_AHCI_H
#define ROZOS_AHCI_H

#include <stdint.h>

#define AHCI_MAX_PORTS 32

/* পাওয়া ডিস্কের সংখ্যা ফেরত দেয় (কন্ট্রোলার না থাকলে 0) */
int ahci_init(void);

struct ahci_info {
    int      present;
    uint32_t version;              /* যেমন 0x00010300 = AHCI 1.3 */
    uint32_t ports_implemented;
    uint32_t command_slots;
    int      supports_64bit;
    int      disks;
};
void ahci_get_info(struct ahci_info *out);

#endif
