/* rozOS — Virtual Memory Manager (x86_64 ৪-লেভেল পেজিং, PML4)
 *
 * ভার্চুয়াল ঠিকানার বিন্যাস:
 *   0x0000000000000000 - 0x00007FFFFFFFFFFF  ইউজার স্পেস (প্রতিটি প্রসেসের আলাদা, PML4 0-255)
 *   0xFFFF800000000000 - ...                 HHDM: ফিজিক্যাল মেমোরির সরাসরি ম্যাপ
 *                                            (Limine-এর একই offset; ডিভাইস MMIO-ও এখানে)
 *   0xFFFFFFFF80000000 - ...                 কার্নেল ইমেজ (.text RX, .rodata R, .data/.bss RW)
 *
 * কার্নেল-অর্ধেকের (PML4 256-511) সব PDPT শুরুতেই তৈরি করা থাকে, তাই নতুন প্রসেসের
 * PML4-এ শুধু এই ২৫৬টা এন্ট্রি কপি করলেই কার্নেলের সব ম্যাপিং (পরে যোগ হওয়াগুলোও)
 * সব প্রসেসে দেখা যায়। ইউজার পেজে VMM_USER না দিলে Ring 3 থেকে অ্যাক্সেস করা যায় না —
 * এটাই কার্নেল/ইউজার মেমোরি সুরক্ষা। */
#ifndef ROZOS_VMM_H
#define ROZOS_VMM_H

#include <stdint.h>
#include <stddef.h>

#define VMM_PAGE_SIZE      4096ULL
#define VMM_HUGE_PAGE_SIZE (2ULL * 1024 * 1024)

#define VMM_USER_TOP       0x0000800000000000ULL
#define VMM_KERNEL_IMAGE   0xFFFFFFFF80000000ULL

/* ম্যাপিং ফ্ল্যাগ */
#define VMM_WRITE    0x01
#define VMM_USER     0x02     /* Ring 3 থেকে অ্যাক্সেসযোগ্য */
#define VMM_EXEC     0x04     /* না দিলে NX (CPU সাপোর্ট করলে) */
#define VMM_NOCACHE  0x08     /* ডিভাইস মেমোরি: uncached (PAT 3) */
#define VMM_WC       0x10     /* write-combining (PAT 5, ফ্রেমবাফার) */

struct vmm_space {
    uint64_t pml4_phys;
};

/* কার্নেলের নিজস্ব পেজ টেবিল বানিয়ে CR3-এ বসায়। শর্ত: pmm_init() হয়ে গেছে।
 * early_map দিয়ে আগে ম্যাপ করা MMIO নতুন টেবিলেও ম্যাপ করা হয়। */
int vmm_init(void);

struct vmm_space *vmm_kernel_space(void);
struct vmm_space *vmm_current_space(void);

int      vmm_map_page(struct vmm_space *sp, uint64_t virt, uint64_t phys, uint32_t flags);
/* ম্যাপিং সরিয়ে আগের ফিজিক্যাল ঠিকানা দেয় (0 = ম্যাপ ছিল না) */
uint64_t vmm_unmap_page(struct vmm_space *sp, uint64_t virt);
int      vmm_map_range(struct vmm_space *sp, uint64_t virt, uint64_t phys,
                       uint64_t size, uint32_t flags);
uint64_t vmm_virt_to_phys(struct vmm_space *sp, uint64_t virt);   /* 0 = ম্যাপ নেই */

/* ডিভাইস মেমোরি HHDM-এ uncached ম্যাপ করে ভার্চুয়াল ঠিকানা দেয় (NULL = ব্যর্থ) */
volatile void *vmm_map_mmio(uint64_t phys, uint64_t size);

/* ইউজার প্রসেসের জন্য (ধাপ ৬): নতুন PML4, কার্নেল-অর্ধেক কপি করা */
struct vmm_space *vmm_create_space(void);
/* ইউজার-অর্ধেকের পেজ টেবিল ফেরত দেয়; free_frames = 1 হলে ম্যাপ করা পেজগুলোও */
void vmm_destroy_space(struct vmm_space *sp, int free_frames);
void vmm_switch(struct vmm_space *sp);

/* ধাপ ৬: 4 KiB পেজ ম্যাপ থাকলে 1; *phys ও *flags (VMM_WRITE/VMM_USER/VMM_EXEC) ভরে
 * (দুটোই NULL হতে পারে) */
int vmm_query(struct vmm_space *sp, uint64_t virt, uint64_t *phys, uint32_t *flags);
/* ধাপ ৬ (fork): src-এর সব ইউজার পেজ নতুন ফ্রেমে কপি করে dst-এ; ব্যর্থ হলে 0 */
int vmm_clone_user(struct vmm_space *dst, struct vmm_space *src);

/* পরিসংখ্যান */
uint64_t vmm_table_pages(void);       /* কার্নেল পেজ-টেবিলে মোট কত পেজ */
int      vmm_nx_enabled(void);

#endif
