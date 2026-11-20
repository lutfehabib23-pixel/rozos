/* rozOS — early page mapper (PMM/VMM তৈরির আগে MMIO ম্যাপ করার জন্য)
 *
 * PROTOCOL.md (base revision 3+): HHDM শুধু usable/bootloader/kernel/framebuffer
 * (rev 4+ এ ACPI-ও) অঞ্চল ম্যাপ করে — LAPIC (0xFEE00000), I/O APIC (0xFEC00000)
 * বা VGA (0xB8000)-এর মতো ডিভাইস মেমোরি থাকে না। এই মডিউল বর্তমান (Limine-এর)
 * PML4-এ 4 KiB পেজ যোগ করে সেগুলো HHDM-এর একই নিয়মে (ভার্চুয়াল = ফিজিক্যাল +
 * HHDM offset) ম্যাপ করে।
 *
 * নতুন পেজ-টেবিল আসে কার্নেলের .bss-এর একটা ছোট স্ট্যাটিক পুল থেকে, কারণ এখনো
 * PMM নেই। ধাপ ৩-এ VMM এই একই map ফাংশন ব্যবহার করবে, শুধু পুলের জায়গায় PMM
 * থেকে পেজ নেবে (early_map_set_table_allocator)। */
#ifndef ROZOS_EARLY_MAP_H
#define ROZOS_EARLY_MAP_H

#include <stdint.h>
#include <stddef.h>

/* পেজ-টেবিল এন্ট্রির ফ্ল্যাগ */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_HUGE      (1ULL << 7)    /* PDPT/PD স্তরে 1 GiB / 2 MiB পেজ */
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PAGE_SIZE 4096ULL

/* ফিজিক্যাল পেজ দেয় (শূন্য করা), না পারলে 0 — ধাপ ৩-এ PMM এটা সরবরাহ করবে */
typedef uint64_t (*early_table_alloc_t)(void);

/* Limine-এর HHDM ও executable address response থেকে offset হিসাব করে; সফল হলে 1 */
int early_map_init(void);

/* phys থেকে size বাইট ডিভাইস মেমোরি (uncached) ম্যাপ করে ভার্চুয়াল ঠিকানা দেয়;
 * ব্যর্থ হলে NULL। আগেই ম্যাপ করা থাকলে সেই ঠিকানাই দেয়। */
volatile void *early_map_mmio(uint64_t phys, uint64_t size);

/* সাধারণ ম্যাপিং: virt → phys, flags = PTE_* (PRESENT নিজে যোগ হয়); সফল হলে 1 */
int early_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* ভার্চুয়াল ঠিকানা বর্তমান পেজ টেবিলে কোন ফিজিক্যাল ঠিকানায় যায়; না থাকলে 0 */
uint64_t early_virt_to_phys(uint64_t virt);

/* কার্নেল ইমেজের ভেতরের (.text/.data/.bss) ঠিকানার ফিজিক্যাল ঠিকানা */
uint64_t early_kernel_virt_to_phys(const void *ptr);

/* early_map_mmio() দিয়ে ম্যাপ করা অঞ্চলের তালিকা — vmm_init() নিজের নতুন পেজ
 * টেবিলে এগুলো আবার ম্যাপ করে, যাতে CR3 বদলানোর পরেও ডিভাইস (যেমন LAPIC) পাওয়া যায় */
#define EARLY_MAP_MAX_MMIO 16
size_t early_map_mmio_count(void);
int    early_map_mmio_range(size_t index, uint64_t *phys, uint64_t *size);

void   early_map_set_table_allocator(early_table_alloc_t alloc);
size_t early_map_pool_used(void);    /* স্ট্যাটিক পুল থেকে কতগুলো টেবিল নেওয়া হয়েছে */
size_t early_map_pool_size(void);

#endif
