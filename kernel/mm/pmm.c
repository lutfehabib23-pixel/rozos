/* rozOS — bitmap ভিত্তিক Physical Memory Manager (rozos/pmm.h দেখুন)
 *
 * PROTOCOL.md: usable ও bootloader-reclaimable entry-গুলো 4 KiB aligned এবং অন্য
 * কোনো entry-র সাথে overlap করে না — তাই এগুলোকে নিরাপদে পেজ হিসেবে ভাগ করা যায়।
 * কার্নেল ইমেজ (executable_and_modules) কখনো খালি ধরা হয় না, তাই early_map-এর
 * স্ট্যাটিক পুল বা কার্নেলের .bss কখনো অন্য কাউকে দেওয়া হয় না।
 *
 * সব অপারেশন ইন্টারাপ্ট বন্ধ রেখে হয় (একক CPU-তে এটাই lock)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/pmm.h>
#include <rozos/bootinfo.h>
#include <rozos/boot.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

static uint8_t *bitmap;
static uint64_t bitmap_bytes;
static uint64_t total_pages;
static uint64_t usable_pages;
static uint64_t free_pages;
static uint64_t next_hint;          /* পরের খোঁজ এখান থেকে শুরু (next-fit) */
static uint64_t reclaimed_bytes;
static uint64_t double_frees;
static uint64_t hhdm;

static inline int bit_test(uint64_t page)  { return bitmap[page >> 3] & (1u << (page & 7)); }
static inline void bit_set(uint64_t page)  { bitmap[page >> 3] |= (uint8_t)(1u << (page & 7)); }
static inline void bit_clear(uint64_t page){ bitmap[page >> 3] &= (uint8_t)~(1u << (page & 7)); }

void *pmm_phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + hhdm);
}

static void free_range(uint64_t base, uint64_t length, uint64_t *counter)
{
    uint64_t page = base / PMM_PAGE_SIZE;
    uint64_t end  = (base + length) / PMM_PAGE_SIZE;
    for (; page < end && page < total_pages; page++) {
        if (bit_test(page)) {
            bit_clear(page);
            free_pages++;
            if (counter) {
                (*counter)++;
            }
        }
    }
}

static void reserve_range(uint64_t base, uint64_t length)
{
    uint64_t page = base / PMM_PAGE_SIZE;
    uint64_t end  = (base + length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (; page < end && page < total_pages; page++) {
        if (!bit_test(page)) {
            bit_set(page);
            free_pages--;
        }
    }
}

int pmm_init(void)
{
    const struct bootinfo *bi = bootinfo();
    uint64_t highest = 0, i, bitmap_phys = 0;

    hhdm = bi->hhdm_offset;

    /* ১. bitmap কত বড় হবে: usable ও bootloader-reclaimable-এর সর্বোচ্চ ঠিকানা পর্যন্ত */
    for (i = 0; i < bi->memmap_count; i++) {
        const struct boot_memmap_entry *e = &bi->memmap[i];
        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            if (e->base + e->length > highest) {
                highest = e->base + e->length;
            }
        }
    }
    if (highest == 0) {
        return 0;
    }
    total_pages  = highest / PMM_PAGE_SIZE;
    bitmap_bytes = (total_pages + 7) / 8;
    bitmap_bytes = (bitmap_bytes + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);

    /* ২. bitmap রাখার জায়গা: 1 MiB-এর উপরের প্রথম যথেষ্ট বড় usable অঞ্চল */
    for (i = 0; i < bi->memmap_count && !bitmap_phys; i++) {
        const struct boot_memmap_entry *e = &bi->memmap[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->base >= 0x100000 && e->length >= bitmap_bytes) {
            bitmap_phys = e->base;
        }
    }
    for (i = 0; i < bi->memmap_count && !bitmap_phys; i++) {
        const struct boot_memmap_entry *e = &bi->memmap[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes && e->base != 0) {
            bitmap_phys = e->base;
        }
    }
    if (!bitmap_phys) {
        return 0;
    }
    bitmap = (uint8_t *)pmm_phys_to_virt(bitmap_phys);

    /* ৩. সব ব্যবহৃত ধরে শুরু, তারপর usable অঞ্চল খালি করা */
    memset(bitmap, 0xFF, bitmap_bytes);
    free_pages = 0;
    for (i = 0; i < bi->memmap_count; i++) {
        const struct boot_memmap_entry *e = &bi->memmap[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            free_range(e->base, e->length, NULL);
        }
    }
    usable_pages = free_pages;

    /* ৪. bitmap-এর নিজের পেজ আর পেজ 0 (NULL ধরার জন্য) কখনো দেওয়া হবে না */
    reserve_range(bitmap_phys, bitmap_bytes);
    reserve_range(0, PMM_PAGE_SIZE);

    next_hint = 0x100000 / PMM_PAGE_SIZE;
    reclaimed_bytes = 0;
    double_frees = 0;
    return 1;
}

static uint64_t find_free_page(void)
{
    uint64_t start = next_hint, page;

    for (page = start; page < total_pages; page++) {
        if ((page & 7) == 0 && page + 8 <= total_pages && bitmap[page >> 3] == 0xFF) {
            page += 7;                    /* পুরো বাইট ব্যবহৃত — একবারে ৮ পেজ বাদ */
            continue;
        }
        if (!bit_test(page)) {
            return page;
        }
    }
    for (page = 1; page < start && page < total_pages; page++) {
        if (!bit_test(page)) {
            return page;
        }
    }
    return 0;
}

uint64_t pmm_alloc_page(void)
{
    uint64_t flags = cpu_irq_save();
    uint64_t page = free_pages ? find_free_page() : 0;

    if (page) {
        bit_set(page);
        free_pages--;
        next_hint = page + 1;
    }
    cpu_irq_restore(flags);
    return page * PMM_PAGE_SIZE;
}

uint64_t pmm_alloc_zeroed_page(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys) {
        memset(pmm_phys_to_virt(phys), 0, PMM_PAGE_SIZE);
    }
    return phys;
}

uint64_t pmm_alloc_contiguous(uint64_t count, uint64_t max_phys)
{
    uint64_t flags, page, run = 0, limit = total_pages, i, result = 0;

    if (count == 0) {
        return 0;
    }
    if (max_phys && max_phys / PMM_PAGE_SIZE < limit) {
        limit = max_phys / PMM_PAGE_SIZE;
    }
    flags = cpu_irq_save();
    for (page = 1; page < limit; page++) {
        if (bit_test(page)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint64_t first = page + 1 - count;
            for (i = first; i <= page; i++) {
                bit_set(i);
            }
            free_pages -= count;
            result = first * PMM_PAGE_SIZE;
            break;
        }
    }
    cpu_irq_restore(flags);
    return result;
}

void pmm_free_page(uint64_t phys)
{
    pmm_free_pages(phys, 1);
}

void pmm_free_pages(uint64_t phys, uint64_t count)
{
    uint64_t flags = cpu_irq_save();
    uint64_t page = phys / PMM_PAGE_SIZE, i;

    for (i = 0; i < count; i++, page++) {
        if (page == 0 || page >= total_pages) {
            continue;
        }
        if (!bit_test(page)) {
            double_frees++;
            continue;
        }
        bit_clear(page);
        free_pages++;
    }
    if (phys / PMM_PAGE_SIZE < next_hint) {
        next_hint = phys / PMM_PAGE_SIZE;
    }
    cpu_irq_restore(flags);
}

uint64_t pmm_reclaim_bootloader_memory(void)
{
    const struct bootinfo *bi = bootinfo();
    uint64_t flags = cpu_irq_save();
    uint64_t pages = 0, i;

    for (i = 0; i < bi->memmap_count; i++) {
        const struct boot_memmap_entry *e = &bi->memmap[i];
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            free_range(e->base, e->length, &pages);
        }
    }
    usable_pages += pages;
    reclaimed_bytes += pages * PMM_PAGE_SIZE;
    cpu_irq_restore(flags);
    return pages * PMM_PAGE_SIZE;
}

int pmm_is_free(uint64_t phys)
{
    uint64_t page = phys / PMM_PAGE_SIZE;
    return page < total_pages && !bit_test(page);
}

void pmm_get_stats(struct pmm_stats *out)
{
    out->total_pages     = total_pages;
    out->usable_pages    = usable_pages;
    out->free_pages      = free_pages;
    out->bitmap_bytes    = bitmap_bytes;
    out->reclaimed_bytes = reclaimed_bytes;
    out->double_frees    = double_frees;
}
