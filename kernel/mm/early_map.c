/* rozOS — early page mapper (rozos/early_map.h দেখুন)
 *
 * x86_64 ৪-লেভেল পেজিং (Limine-এর paging mode request অনুযায়ী নিশ্চিত):
 *   PML4 (বিট 47-39) → PDPT (38-30) → PD (29-21) → PT (20-12) → 4 KiB পেজ
 * Limine-এর পেজ টেবিল bootloader-reclaimable মেমোরিতে থাকে, যা HHDM-এ ম্যাপ করা —
 * তাই ফিজিক্যাল ঠিকানায় HHDM offset যোগ করেই সেগুলো পড়া/লেখা যায়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>
#include <rozos/early_map.h>
#include <rozos/boot.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

#define EARLY_POOL_PAGES 16      /* 16 টেবিল = 64 KiB; কয়েকটা MMIO অঞ্চলের জন্য যথেষ্ট */
#define MSR_EFER         0xC0000080
#define EFER_NXE         (1ULL << 11)

static uint8_t table_pool[EARLY_POOL_PAGES][PAGE_SIZE] ROZ_ALIGNED(4096);
static size_t  pool_used;
static uint64_t hhdm;                   /* HHDM offset */
static uint64_t kernel_phys_base, kernel_virt_base;
static int ready;
static early_table_alloc_t table_alloc;
static int nx_enabled;                  /* EFER.NXE বন্ধ থাকলে বিট 63 reserved — দিলে #PF */

struct mmio_record {
    uint64_t phys;
    uint64_t size;
};
static struct mmio_record mmio_records[EARLY_MAP_MAX_MMIO];
static size_t mmio_record_count;

static void record_mmio(uint64_t phys, uint64_t size)
{
    size_t i;
    for (i = 0; i < mmio_record_count; i++) {
        if (mmio_records[i].phys == phys && mmio_records[i].size >= size) {
            return;
        }
    }
    if (mmio_record_count < EARLY_MAP_MAX_MMIO) {
        mmio_records[mmio_record_count].phys = phys;
        mmio_records[mmio_record_count].size = size;
        mmio_record_count++;
    }
}

size_t early_map_mmio_count(void)
{
    return mmio_record_count;
}

int early_map_mmio_range(size_t index, uint64_t *phys, uint64_t *size)
{
    if (index >= mmio_record_count) {
        return 0;
    }
    *phys = mmio_records[index].phys;
    *size = mmio_records[index].size;
    return 1;
}

static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + hhdm);
}

static inline void invlpg(uint64_t virt)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

int early_map_init(void)
{
    struct limine_executable_address_response *ea = limine_exec_addr_req.response;

    ready = 0;
    hhdm = boot_hhdm_offset();
    if (!hhdm || !ea) {
        return 0;
    }
    kernel_phys_base = ea->physical_base;
    kernel_virt_base = ea->virtual_base;
    nx_enabled = (cpu_rdmsr(MSR_EFER) & EFER_NXE) != 0;
    pool_used = 0;
    ready = 1;
    return 1;
}

uint64_t early_kernel_virt_to_phys(const void *ptr)
{
    /* PROTOCOL.md: কার্নেল ইমেজ ফিজিক্যালি একটানা, একটাই offset */
    return (uint64_t)(uintptr_t)ptr - kernel_virt_base + kernel_phys_base;
}

void early_map_set_table_allocator(early_table_alloc_t alloc)
{
    table_alloc = alloc;
}

size_t early_map_pool_used(void) { return pool_used; }
size_t early_map_pool_size(void) { return EARLY_POOL_PAGES; }

/* নতুন শূন্য পেজ-টেবিলের ফিজিক্যাল ঠিকানা */
static uint64_t new_table(void)
{
    uint64_t phys;

    if (table_alloc) {
        phys = table_alloc();
        if (phys) {
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
        }
        return phys;
    }
    if (pool_used >= EARLY_POOL_PAGES) {
        return 0;
    }
    memset(table_pool[pool_used], 0, PAGE_SIZE);
    phys = early_kernel_virt_to_phys(table_pool[pool_used]);
    pool_used++;
    return phys;
}

/* এক স্তর নিচে নামা; দরকার হলে নতুন টেবিল বানানো। huge পেজ পেলে NULL আর *huge = 1 */
static uint64_t *next_level(uint64_t *table, unsigned index, int create, int *huge)
{
    uint64_t e = table[index];

    if (e & PTE_PRESENT) {
        if (e & PTE_HUGE) {
            *huge = 1;
            return NULL;
        }
        return (uint64_t *)phys_to_virt(e & PTE_ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }
    {
        uint64_t phys = new_table();
        if (!phys) {
            return NULL;
        }
        /* মধ্যবর্তী স্তরে উদার permission; আসল নিয়ন্ত্রণ শেষ স্তরের এন্ট্রিতে */
        table[index] = phys | PTE_PRESENT | PTE_WRITABLE;
        return (uint64_t *)phys_to_virt(phys);
    }
}

int early_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4, *pdpt, *pd, *pt;
    int huge = 0;

    if (!ready || (virt & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1))) {
        return 0;
    }
    if (!nx_enabled) {
        flags &= ~PTE_NX;
    }
    pml4 = (uint64_t *)phys_to_virt(cpu_read_cr3() & PTE_ADDR_MASK);

    pdpt = next_level(pml4, (unsigned)((virt >> 39) & 0x1FF), 1, &huge);
    if (!pdpt) return 0;
    pd = next_level(pdpt, (unsigned)((virt >> 30) & 0x1FF), 1, &huge);
    if (!pd) return huge;          /* 1 GiB পেজ আগেই আছে — ঠিকানা ম্যাপ করা */
    pt = next_level(pd, (unsigned)((virt >> 21) & 0x1FF), 1, &huge);
    if (!pt) return huge;          /* 2 MiB পেজ আগেই আছে */

    pt[(virt >> 12) & 0x1FF] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    invlpg(virt);
    return 1;
}

uint64_t early_virt_to_phys(uint64_t virt)
{
    uint64_t *pml4, *pdpt, *pd, *pt, e;
    int huge = 0;

    if (!ready) {
        return 0;
    }
    pml4 = (uint64_t *)phys_to_virt(cpu_read_cr3() & PTE_ADDR_MASK);

    pdpt = next_level(pml4, (unsigned)((virt >> 39) & 0x1FF), 0, &huge);
    if (!pdpt) return 0;
    pd = next_level(pdpt, (unsigned)((virt >> 30) & 0x1FF), 0, &huge);
    if (!pd) {
        if (!huge) return 0;
        e = pdpt[(virt >> 30) & 0x1FF];
        return (e & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFFULL);
    }
    pt = next_level(pd, (unsigned)((virt >> 21) & 0x1FF), 0, &huge);
    if (!pt) {
        if (!huge) return 0;
        e = pd[(virt >> 21) & 0x1FF];
        return (e & 0x000FFFFFFFE00000ULL) | (virt & 0x1FFFFFULL);
    }
    e = pt[(virt >> 12) & 0x1FF];
    if (!(e & PTE_PRESENT)) {
        return 0;
    }
    return (e & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

volatile void *early_map_mmio(uint64_t phys, uint64_t size)
{
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t p;

    if (!ready || size == 0) {
        return NULL;
    }
    record_mmio(start, end - start);
    for (p = start; p < end; p += PAGE_SIZE) {
        uint64_t virt = p + hhdm;
        if (early_virt_to_phys(virt) == p) {
            continue;              /* আগেই ঠিকমতো ম্যাপ করা */
        }
        /* ডিভাইস মেমোরি: uncached (PCD|PWT → PAT এন্ট্রি 3 = UC), executable নয় */
        if (!early_map_page(virt, p, PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX)) {
            return NULL;
        }
    }
    return (volatile void *)(uintptr_t)(phys + hhdm);
}
