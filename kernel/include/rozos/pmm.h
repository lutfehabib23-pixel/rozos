/* rozOS — Physical Memory Manager (bitmap allocator)
 *
 * প্রতিটি 4 KiB ফিজিক্যাল পেজের জন্য ১ বিট: 1 = ব্যবহৃত/অব্যবহারযোগ্য, 0 = খালি।
 * bitmap নিজে প্রথম যথেষ্ট বড় usable অঞ্চলে থাকে (HHDM দিয়ে অ্যাক্সেস)।
 * সব ঠিকানা ফিজিক্যাল; পেজের ভেতরে লিখতে PMM_PHYS_TO_VIRT() দিয়ে HHDM ঠিকানা নিতে হয়। */
#ifndef ROZOS_PMM_H
#define ROZOS_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PMM_PAGE_SIZE 4096ULL

struct pmm_stats {
    uint64_t total_pages;         /* bitmap যতগুলো পেজ ঢাকে */
    uint64_t usable_pages;        /* কখনো খালি ছিল এমন পেজ (usable + reclaim করা) */
    uint64_t free_pages;
    uint64_t bitmap_bytes;
    uint64_t reclaimed_bytes;     /* bootloader-reclaimable থেকে ফেরত পাওয়া */
    uint64_t double_frees;        /* ভুল: আগেই খালি পেজ আবার free */
};

/* bootinfo-র memmap থেকে তৈরি; সফল হলে 1 */
int      pmm_init(void);

/* একটি পেজ; না পেলে 0 */
uint64_t pmm_alloc_page(void);
/* একটি শূন্য-করা পেজ (পেজ-টেবিল ইত্যাদির জন্য) */
uint64_t pmm_alloc_zeroed_page(void);
/* count টি পাশাপাশি পেজ, শেষ ঠিকানা max_phys-এর নিচে (0 = সীমা নেই) — DMA-র জন্য */
uint64_t pmm_alloc_contiguous(uint64_t count, uint64_t max_phys);

void     pmm_free_page(uint64_t phys);
void     pmm_free_pages(uint64_t phys, uint64_t count);

/* bootloader-reclaimable অঞ্চলগুলো খালি তালিকায় যোগ; ফেরত দেয় বাইট সংখ্যা।
 * শর্ত: bootinfo কপি হয়ে গেছে এবং কার্নেল নিজের পেজ টেবিলে চলছে (vmm_init)। */
uint64_t pmm_reclaim_bootloader_memory(void);

int      pmm_is_free(uint64_t phys);
void     pmm_get_stats(struct pmm_stats *out);

/* HHDM সহায়ক */
void    *pmm_phys_to_virt(uint64_t phys);

#endif
