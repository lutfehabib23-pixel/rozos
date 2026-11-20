/* rozOS — size-class কার্নেল heap (rozos/heap.h দেখুন)
 *
 * পেজের বিন্যাস (ছোট শ্রেণি):
 *   [16 বাইট হেডার][টুকরো][টুকরো]...   — 16 বাইটের শ্রেণিতে 255টা, 2048-এ 1টা
 * বড় অ্যালোকেশন:
 *   [16 বাইট হেডার][ডেটা ...]           — n টি পাশাপাশি পেজ
 *
 * খালি টুকরোর প্রথম 8 বাইটে free list-এর পরের পয়েন্টার, পরের 8 বাইটে একটা
 * "খালি" চিহ্ন (FREE_MAGIC) — দ্বিতীয়বার kfree করলে এটা দেখে ধরা পড়ে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>
#include <rozos/heap.h>
#include <rozos/pmm.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <arch/x86_64/cpu.h>

#define PAGE_SZ        4096ULL
#define HEADER_SIZE    16
#define PAGE_MAGIC     0x48454150u          /* "HEAP" */
#define LARGE_CLASS    0xFFFF
#define FREE_MAGIC     0xF4EEF4EEF4EEF4EEULL

struct page_header {
    uint32_t magic;
    uint16_t class_index;                   /* LARGE_CLASS = বড় অ্যালোকেশন */
    uint16_t in_use;                        /* ছোট: এই পেজে ব্যবহৃত টুকরো */
    uint64_t pages;                         /* বড়: মোট পেজ */
};

struct free_chunk {
    struct free_chunk *next;
    uint64_t marker;
};

static const uint32_t class_sizes[HEAP_CLASS_COUNT] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
static struct free_chunk *free_lists[HEAP_CLASS_COUNT];
static struct heap_stats stats;
static int ready;

static int class_for(size_t size)
{
    int i;
    for (i = 0; i < HEAP_CLASS_COUNT; i++) {
        if (size <= class_sizes[i]) {
            return i;
        }
    }
    return -1;
}

static struct page_header *header_of(const void *ptr)
{
    return (struct page_header *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SZ - 1));
}

/* শ্রেণির জন্য নতুন পেজ এনে টুকরো করে free list-এ যোগ */
static int grow_class(int c)
{
    uint64_t phys = pmm_alloc_page();
    struct page_header *h;
    uint8_t *p, *end;
    uint32_t sz = class_sizes[c];

    if (!phys) {
        return 0;
    }
    h = (struct page_header *)pmm_phys_to_virt(phys);
    h->magic = PAGE_MAGIC;
    h->class_index = (uint16_t)c;
    h->in_use = 0;
    h->pages = 1;

    p = (uint8_t *)h + HEADER_SIZE;
    end = (uint8_t *)h + PAGE_SZ;
    /* 2048-এর শ্রেণিতে হেডারের পরে একটাই টুকরো ধরে (16 + 2048 ≤ 4096) */
    for (; p + sz <= end; p += sz) {
        struct free_chunk *f = (struct free_chunk *)p;
        f->next = free_lists[c];
        f->marker = FREE_MAGIC;
        free_lists[c] = f;
    }
    stats.class_pages[c]++;
    return 1;
}

int heap_init(void)
{
    int i;
    memset(&stats, 0, sizeof(stats));
    for (i = 0; i < HEAP_CLASS_COUNT; i++) {
        free_lists[i] = NULL;
        stats.class_size[i] = class_sizes[i];
    }
    ready = 1;
    return 1;
}

static void *alloc_large(size_t size)
{
    uint64_t pages = (size + HEADER_SIZE + PAGE_SZ - 1) / PAGE_SZ;
    uint64_t phys = pmm_alloc_contiguous(pages, 0);
    struct page_header *h;

    if (!phys) {
        return NULL;
    }
    h = (struct page_header *)pmm_phys_to_virt(phys);
    h->magic = PAGE_MAGIC;
    h->class_index = LARGE_CLASS;
    h->in_use = 1;
    h->pages = pages;
    stats.large_allocs++;
    stats.large_pages += pages;
    return (uint8_t *)h + HEADER_SIZE;
}

void *kmalloc(size_t size)
{
    uint64_t flags;
    void *result = NULL;
    int c;

    if (!ready) {
        return NULL;
    }
    if (size == 0) {
        size = 1;
    }
    flags = cpu_irq_save();
    c = class_for(size);
    if (c < 0) {
        result = alloc_large(size);
    } else {
        if (!free_lists[c]) {
            grow_class(c);
        }
        if (free_lists[c]) {
            struct free_chunk *f = free_lists[c];
            free_lists[c] = f->next;
            f->marker = 0;
            header_of(f)->in_use++;
            stats.class_in_use[c]++;
            result = f;
        }
    }
    if (result) {
        stats.total_allocs++;
    }
    cpu_irq_restore(flags);
    return result;
}

void kfree(void *ptr)
{
    struct page_header *h;
    uint64_t flags;

    if (!ptr) {
        return;
    }
    flags = cpu_irq_save();
    h = header_of(ptr);

    if (h->magic != PAGE_MAGIC) {
        stats.bad_frees++;
    } else if (h->class_index == LARGE_CLASS) {
        if ((uint8_t *)ptr != (uint8_t *)h + HEADER_SIZE || !h->in_use) {
            stats.bad_frees++;
        } else {
            uint64_t pages = h->pages;
            h->in_use = 0;
            h->magic = 0;
            stats.large_allocs--;
            stats.large_pages -= pages;
            stats.total_frees++;
            pmm_free_pages((uint64_t)(uintptr_t)h - (uint64_t)(uintptr_t)pmm_phys_to_virt(0), pages);
        }
    } else if (h->class_index < HEAP_CLASS_COUNT) {
        int c = h->class_index;
        uintptr_t off = (uintptr_t)ptr - ((uintptr_t)h + HEADER_SIZE);
        struct free_chunk *f = (struct free_chunk *)ptr;

        if ((uintptr_t)ptr < (uintptr_t)h + HEADER_SIZE || off % class_sizes[c] != 0 ||
            f->marker == FREE_MAGIC) {
            stats.bad_frees++;                 /* টুকরোর শুরু নয়, অথবা double free */
        } else {
            f->marker = FREE_MAGIC;
            f->next = free_lists[c];
            free_lists[c] = f;
            h->in_use--;
            stats.class_in_use[c]--;
            stats.total_frees++;
        }
    } else {
        stats.bad_frees++;
    }
    cpu_irq_restore(flags);
}

size_t kmalloc_usable_size(const void *ptr)
{
    struct page_header *h;
    if (!ptr) {
        return 0;
    }
    h = header_of(ptr);
    if (h->magic != PAGE_MAGIC) {
        return 0;
    }
    if (h->class_index == LARGE_CLASS) {
        return (size_t)(h->pages * PAGE_SZ - HEADER_SIZE);
    }
    return h->class_index < HEAP_CLASS_COUNT ? class_sizes[h->class_index] : 0;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void *kcalloc(size_t count, size_t size)
{
    if (size && count > (size_t)-1 / size) {
        return NULL;                           /* গুণফল ওভারফ্লো */
    }
    return kzalloc(count * size);
}

void *krealloc(void *ptr, size_t size)
{
    size_t old;
    void *n;

    if (!ptr) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    old = kmalloc_usable_size(ptr);
    if (size <= old) {
        return ptr;                            /* বর্তমান টুকরোতেই আঁটে */
    }
    n = kmalloc(size);
    if (!n) {
        return NULL;
    }
    memcpy(n, ptr, old);
    kfree(ptr);
    return n;
}

void heap_get_stats(struct heap_stats *out)
{
    uint64_t flags = cpu_irq_save();
    *out = stats;
    cpu_irq_restore(flags);
}

int heap_selftest(void)
{
    static const size_t sizes[] = { 1, 15, 16, 17, 100, 500, 1000, 2048, 2049, 5000, 20000 };
    void *ptrs[ROZ_ARRAY_SIZE(sizes)];
    size_t i, j;
    uint64_t bad_before = stats.bad_frees;
    int ok = 1;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i] || ((uintptr_t)ptrs[i] & 15)) {
            return 0;
        }
        memset(ptrs[i], (int)(i + 1), sizes[i]);
    }
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const uint8_t *p = (const uint8_t *)ptrs[i];
        for (j = 0; j < sizes[i]; j++) {
            if (p[j] != (uint8_t)(i + 1)) {
                ok = 0;
            }
        }
    }
    /* krealloc বিষয়বস্তু রাখে কিনা */
    ptrs[0] = krealloc(ptrs[0], 3000);
    if (!ptrs[0] || ((uint8_t *)ptrs[0])[0] != 1) {
        ok = 0;
    }
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        kfree(ptrs[i]);
    }
    /* double free ধরা পড়ে কিনা (ইচ্ছাকৃত) */
    {
        void *p = kmalloc(64);
        kfree(p);
        kfree(p);
        if (stats.bad_frees != bad_before + 1) {
            ok = 0;
        }
        stats.bad_frees = bad_before;          /* ইচ্ছাকৃত ভুলটা পরিসংখ্যানে না রাখি */
    }
    return ok;
}
