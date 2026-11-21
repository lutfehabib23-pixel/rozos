/* rozOS — NVMe SSD ড্রাইভার (ধাপ ১৫ক)
 *
 * PCI class 01:08:02 কন্ট্রোলার খুঁজে namespace 1-কে ব্লক ডিভাইস (nvmea, nvmeb ...)
 * হিসেবে নিবন্ধন — পার্টিশন থাকলে nvmea1, nvmea2 ...; তারপর `mount /dev/nvmea1 /mnt/nvme`।
 * একটা admin queue আর একটা I/O queue (প্রতিটা ৬৪ ঘর), polling (ইন্টারাপ্ট ছাড়া)।
 * ডেটা যায় ১২৮ KiB-এর DMA bounce buffer দিয়ে (PRP list), তাই কলারের বাফার যেকোনো
 * কার্নেল ঠিকানায় থাকতে পারে। QEMU: make run NVME=1 (nvme-disk.img জোড়া হয়)। */
#ifndef ROZOS_NVME_H
#define ROZOS_NVME_H

#include <stdint.h>

#define NVME_MAX_CTRL 4

/* পাওয়া ডিস্কের সংখ্যা */
int nvme_init(void);

struct nvme_info {
    int      controllers;
    uint32_t version;              /* 0x00010400 = NVMe 1.4 */
    char     model[41];
};
void nvme_get_info(struct nvme_info *out);

#endif
