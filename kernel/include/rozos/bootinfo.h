/* rozOS — বুটলোডারের তথ্যের কার্নেল-নিজস্ব কপি
 *
 * Limine-এর সব response (এবং সেগুলোর স্ট্রিং/অ্যারে) bootloader-reclaimable মেমোরিতে
 * থাকে (PROTOCOL.md)। ধাপ ৩-এর শেষে PMM ওই মেমোরি ফেরত নেয়, তাই তার আগেই
 * bootinfo_save() দরকারি সবকিছু এখানে কপি করে রাখে। এরপর কার্নেলের কোথাও সরাসরি
 * limine_*_req.response পড়া যাবে না — এই struct ব্যবহার করতে হবে। */
#ifndef ROZOS_BOOTINFO_H
#define ROZOS_BOOTINFO_H

#include <stdint.h>
#include <stddef.h>

#define BOOTINFO_MAX_MEMMAP   256
#define BOOTINFO_MAX_MODULES  16
#define BOOTINFO_STR_MAX      128

/* memmap type — Limine-এর LIMINE_MEMMAP_* মানগুলোই রাখা হয় */
struct boot_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct boot_module {
    uint64_t phys;                       /* মডিউলের ফিজিক্যাল ঠিকানা */
    uint64_t virt;                       /* HHDM ভার্চুয়াল ঠিকানা */
    uint64_t size;
    char     path[BOOTINFO_STR_MAX];
    char     cmdline[BOOTINFO_STR_MAX];
};

struct bootinfo {
    char     bootloader_name[32];
    char     bootloader_version[32];
    uint64_t firmware_type;              /* LIMINE_FIRMWARE_TYPE_* */
    int      firmware_known;
    uint64_t paging_mode;                /* LIMINE_PAGING_MODE_X86_64_* */
    int      paging_mode_known;
    uint64_t hhdm_offset;
    uint64_t kernel_phys;
    uint64_t kernel_virt;
    char     cmdline[256];
    uint64_t rsdp_phys;                  /* 0 = ACPI নেই */
    int64_t  boot_unix_time;             /* বুটের মুহূর্ত (UNIX সেকেন্ড, UTC); 0 = অজানা */

    uint64_t memmap_count;
    struct boot_memmap_entry memmap[BOOTINFO_MAX_MEMMAP];
    uint64_t memmap_truncated;           /* জায়গা না থাকায় বাদ পড়া entry */

    uint64_t module_count;
    struct boot_module modules[BOOTINFO_MAX_MODULES];
};

/* Limine response থেকে কপি; HHDM ও memmap না থাকলে 0 */
int bootinfo_save(void);
const struct bootinfo *bootinfo(void);

#endif
