/* rozOS — Virtual Memory Manager (rozos/vmm.h দেখুন)
 *
 * পেজ-টেবিলের সব পেজ PMM থেকে আসে এবং HHDM দিয়ে অ্যাক্সেস করা হয়।
 * কার্নেলের HHDM অঞ্চলে যেখানে সম্ভব 2 MiB পেজ ব্যবহার হয় (কম টেবিল, কম TLB miss)।
 * এই huge পেজগুলো কখনো আনম্যাপ করা হয় না; 4 KiB ম্যাপিং চাইলে যে ঠিকানা আগেই
 * huge পেজে ঢাকা, সেখানে vmm_map_page ব্যর্থ হয় (0)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/vmm.h>
#include <rozos/pmm.h>
#include <rozos/heap.h>
#include <rozos/bootinfo.h>
#include <rozos/boot.h>
#include <rozos/early_map.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

#define MSR_EFER  0xC0000080
#define EFER_NXE  (1ULL << 11)

#define PT_ENTRIES 512
#define PTE_PAT_4K   (1ULL << 7)     /* 4 KiB এন্ট্রিতে PAT বিট */
#define PTE_PAT_HUGE (1ULL << 12)    /* 2 MiB/1 GiB এন্ট্রিতে PAT বিট */
#define HUGE_ADDR_MASK 0x000FFFFFFFE00000ULL
#define PTE_SHARED   (1ULL << 9)     /* ধাপ ১৩: সফটওয়্যার বিট — VMM_SHARED */

static void (*shared_hook)(uint64_t phys, int delta);

void vmm_set_shared_hook(void (*fn)(uint64_t phys, int delta))
{
    shared_hook = fn;
}

#ifndef __TINYC__
/* linker.ld */
extern char __kernel_start[], __text_start[], __rodata_start[], __data_start[], __kernel_end[];
#else
/* ধাপ ১১: TCC-বিল্ড কার্নেলে linker.ld-এর প্রতীক নেই। TCC-র লিংকার ELF header ও
 * program header প্রথম PT_LOAD-এর ভেতরে রাখে (ফাইল-অফসেট 0 = kernel_virt), তাই
 * কার্নেল নিজের program header পড়ে প্রতিটি PT_LOAD তার R/W/X অনুযায়ী ম্যাপ করে। */
static int map_kernel_section(char *start, char *end, uint32_t flags);

static int map_kernel_from_phdrs(uint64_t base)
{
    const unsigned char *eh = (const unsigned char *)(uintptr_t)base;
    uint64_t phoff;
    uint16_t phentsize, phnum, k;
    if (eh[0] != 0x7F || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F') {
        return 0;
    }
    phoff = *(const uint64_t *)(eh + 32);
    phentsize = *(const uint16_t *)(eh + 54);
    phnum = *(const uint16_t *)(eh + 56);
    for (k = 0; k < phnum; k++) {
        const unsigned char *ph = eh + phoff + (uint64_t)phentsize * k;
        uint32_t type = *(const uint32_t *)ph, pf = *(const uint32_t *)(ph + 4);
        uint64_t va = *(const uint64_t *)(ph + 16), msz = *(const uint64_t *)(ph + 40);
        if (type != 1 || msz == 0) {                     /* শুধু PT_LOAD */
            continue;
        }
        if (!map_kernel_section((char *)(uintptr_t)va, (char *)(uintptr_t)(va + msz),
                                ((pf & 2) ? VMM_WRITE : 0) | ((pf & 1) ? VMM_EXEC : 0))) {
            return 0;
        }
    }
    return 1;
}
#endif

static struct vmm_space kernel_space;
static struct vmm_space *current_space;
static uint64_t hhdm;
static int nx_ok;
static uint64_t table_pages;

static inline uint64_t *table_virt(uint64_t phys)
{
    return (uint64_t *)(uintptr_t)(phys + hhdm);
}

static inline void invlpg(uint64_t virt)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline unsigned idx(uint64_t virt, unsigned shift)
{
    return (unsigned)((virt >> shift) & 0x1FF);
}

/* VMM_* → পেজ-টেবিল এন্ট্রির ফ্ল্যাগ */
static uint64_t pte_flags(uint32_t flags, int huge)
{
    uint64_t f = PTE_PRESENT;
    if (flags & VMM_WRITE) f |= PTE_WRITABLE;
    if (flags & VMM_USER)  f |= PTE_USER;
    if (!(flags & VMM_EXEC) && nx_ok) f |= PTE_NX;
    if (flags & VMM_NOCACHE) {
        f |= PTE_PCD | PTE_PWT;                       /* PAT index 3 = UC */
    } else if (flags & VMM_WC) {
        f |= PTE_PWT | (huge ? PTE_PAT_HUGE : PTE_PAT_4K);   /* PAT index 5 = WC (Limine-এর PAT) */
    }
    if (flags & VMM_SHARED) f |= PTE_SHARED;
    return f;
}

static uint64_t alloc_table(void)
{
    uint64_t phys = pmm_alloc_zeroed_page();
    if (phys) {
        table_pages++;
    }
    return phys;
}

/* পরের স্তরের টেবিল; create = 1 হলে না থাকলে তৈরি। huge পেজ পেলে NULL */
static uint64_t *next_table(uint64_t *table, unsigned i, int create, int user)
{
    uint64_t e = table[i];
    if (e & PTE_PRESENT) {
        if (e & PTE_HUGE) {
            return NULL;
        }
        if (user && !(e & PTE_USER)) {
            table[i] = e | PTE_USER;                  /* ইউজার পেজের পথে USER বিট লাগে */
        }
        return table_virt(e & PTE_ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }
    {
        uint64_t phys = alloc_table();
        if (!phys) {
            return NULL;
        }
        table[i] = phys | PTE_PRESENT | PTE_WRITABLE | (user ? PTE_USER : 0);
        return table_virt(phys);
    }
}

static void flush_if_current(struct vmm_space *sp, uint64_t virt)
{
    if (sp == current_space || virt >= 0xFFFF800000000000ULL) {
        invlpg(virt);
    }
}

int vmm_map_page(struct vmm_space *sp, uint64_t virt, uint64_t phys, uint32_t flags)
{
    uint64_t *pml4 = table_virt(sp->pml4_phys), *pdpt, *pd, *pt;
    int user = (flags & VMM_USER) != 0;

    if ((virt | phys) & (VMM_PAGE_SIZE - 1)) {
        return 0;
    }
    if (user && virt >= VMM_USER_TOP) {
        return 0;                                     /* কার্নেল অঞ্চলে ইউজার পেজ নয় */
    }
    if (!(pdpt = next_table(pml4, idx(virt, 39), 1, user))) return 0;
    if (!(pd   = next_table(pdpt, idx(virt, 30), 1, user))) return 0;
    if (!(pt   = next_table(pd,   idx(virt, 21), 1, user))) return 0;

    pt[idx(virt, 12)] = (phys & PTE_ADDR_MASK) | pte_flags(flags, 0);
    flush_if_current(sp, virt);
    return 1;
}

static int map_huge(struct vmm_space *sp, uint64_t virt, uint64_t phys, uint32_t flags)
{
    uint64_t *pml4 = table_virt(sp->pml4_phys), *pdpt, *pd;
    uint64_t e;

    if (!(pdpt = next_table(pml4, idx(virt, 39), 1, 0))) return 0;
    if (!(pd   = next_table(pdpt, idx(virt, 30), 1, 0))) return 0;
    e = pd[idx(virt, 21)];
    if ((e & PTE_PRESENT) && !(e & PTE_HUGE)) {
        return 0;                                     /* নিচে আগেই 4 KiB টেবিল আছে */
    }
    pd[idx(virt, 21)] = (phys & HUGE_ADDR_MASK) | pte_flags(flags, 1) | PTE_HUGE;
    flush_if_current(sp, virt);
    return 1;
}

int vmm_map_range(struct vmm_space *sp, uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags)
{
    uint64_t end = virt + size;
    while (virt < end) {
        if (!(flags & VMM_USER) &&
            !(virt & (VMM_HUGE_PAGE_SIZE - 1)) && !(phys & (VMM_HUGE_PAGE_SIZE - 1)) &&
            end - virt >= VMM_HUGE_PAGE_SIZE && map_huge(sp, virt, phys, flags)) {
            virt += VMM_HUGE_PAGE_SIZE;
            phys += VMM_HUGE_PAGE_SIZE;
            continue;
        }
        if (!vmm_map_page(sp, virt, phys, flags)) {
            return 0;
        }
        virt += VMM_PAGE_SIZE;
        phys += VMM_PAGE_SIZE;
    }
    return 1;
}

uint64_t vmm_unmap_page(struct vmm_space *sp, uint64_t virt)
{
    uint64_t *pml4 = table_virt(sp->pml4_phys), *pdpt, *pd, *pt, e;

    if (!(pdpt = next_table(pml4, idx(virt, 39), 0, 0))) return 0;
    if (!(pd   = next_table(pdpt, idx(virt, 30), 0, 0))) return 0;
    if (!(pt   = next_table(pd,   idx(virt, 21), 0, 0))) return 0;
    e = pt[idx(virt, 12)];
    if (!(e & PTE_PRESENT)) {
        return 0;
    }
    pt[idx(virt, 12)] = 0;
    flush_if_current(sp, virt);
    if (e & PTE_SHARED) {                             /* ধাপ ১৩: ফ্রেম আমাদের নয় — মুক্ত নয় */
        if (shared_hook) shared_hook(e & PTE_ADDR_MASK, -1);
        return 0;
    }
    return e & PTE_ADDR_MASK;
}

uint64_t vmm_virt_to_phys(struct vmm_space *sp, uint64_t virt)
{
    uint64_t *pml4 = table_virt(sp->pml4_phys), *pdpt, *pd, *pt, e;

    e = pml4[idx(virt, 39)];
    if (!(e & PTE_PRESENT)) return 0;
    pdpt = table_virt(e & PTE_ADDR_MASK);

    e = pdpt[idx(virt, 30)];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_HUGE) return (e & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFFULL);
    pd = table_virt(e & PTE_ADDR_MASK);

    e = pd[idx(virt, 21)];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_HUGE) return (e & HUGE_ADDR_MASK) | (virt & 0x1FFFFFULL);
    pt = table_virt(e & PTE_ADDR_MASK);

    e = pt[idx(virt, 12)];
    if (!(e & PTE_PRESENT)) return 0;
    return (e & PTE_ADDR_MASK) | (virt & (VMM_PAGE_SIZE - 1));
}

volatile void *vmm_map_mmio(uint64_t phys, uint64_t size)
{
    uint64_t start = phys & ~(VMM_PAGE_SIZE - 1);
    uint64_t end = (phys + size + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    uint64_t p;

    for (p = start; p < end; p += VMM_PAGE_SIZE) {
        uint64_t v = p + hhdm;
        if (vmm_virt_to_phys(&kernel_space, v) == p) {
            continue;
        }
        if (!vmm_map_page(&kernel_space, v, p, VMM_WRITE | VMM_NOCACHE)) {
            return NULL;
        }
    }
    return (volatile void *)(uintptr_t)(phys + hhdm);
}

/* ---------- কার্নেল স্পেস তৈরি ---------- */

static int map_kernel_section(char *start, char *end, uint32_t flags)
{
    const struct bootinfo *bi = bootinfo();
    uint64_t v = (uint64_t)(uintptr_t)start & ~(VMM_PAGE_SIZE - 1);
    uint64_t e = ((uint64_t)(uintptr_t)end + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    for (; v < e; v += VMM_PAGE_SIZE) {
        if (!vmm_map_page(&kernel_space, v, v - bi->kernel_virt + bi->kernel_phys, flags)) {
            return 0;
        }
    }
    return 1;
}

/* PROTOCOL.md (base revision 4+) অনুযায়ী Limine যেসব অঞ্চল HHDM-এ রাখে, ঠিক সেগুলোই */
static int hhdm_type(uint64_t type, uint32_t *flags)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_NVS:
    case LIMINE_MEMMAP_RESERVED_MAPPED:
        *flags = VMM_WRITE;
        return 1;
    case LIMINE_MEMMAP_FRAMEBUFFER:
        *flags = VMM_WRITE | VMM_WC;
        return 1;
    default:
        return 0;
    }
}

int vmm_init(void)
{
    const struct bootinfo *bi = bootinfo();
    uint64_t *pml4, i;
    size_t m;

    hhdm = bi->hhdm_offset;
    nx_ok = (cpu_rdmsr(MSR_EFER) & EFER_NXE) != 0;
    table_pages = 0;

    kernel_space.pml4_phys = alloc_table();
    if (!kernel_space.pml4_phys) {
        return 0;
    }
    pml4 = table_virt(kernel_space.pml4_phys);

    /* ১. কার্নেল-অর্ধেকের সব PDPT আগেই — সব প্রসেস এগুলো ভাগ করবে */
    for (i = 256; i < PT_ENTRIES; i++) {
        uint64_t phys = alloc_table();
        if (!phys) {
            return 0;
        }
        pml4[i] = phys | PTE_PRESENT | PTE_WRITABLE;
    }

    /* ২. কার্নেল ইমেজ, প্রতিটি অংশ নিজের permission-এ (linker.ld-এর PHDRS অনুযায়ী) */
#ifdef __TINYC__
    if (!map_kernel_from_phdrs(bootinfo()->kernel_virt)) {
        return 0;
    }
#else
    if (!map_kernel_section(__kernel_start, __text_start, VMM_WRITE) ||        /* Limine requests */
        !map_kernel_section(__text_start, __rodata_start, VMM_EXEC) ||         /* .text */
        !map_kernel_section(__rodata_start, __data_start, 0) ||                /* .rodata, .init_array */
        !map_kernel_section(__data_start, __kernel_end, VMM_WRITE)) {          /* .data, .bss */
        return 0;
    }
#endif

    /* ৩. HHDM */
    for (i = 0; i < bi->memmap_count; i++) {
        const struct boot_memmap_entry *e = &bi->memmap[i];
        uint32_t flags;
        uint64_t start, end;
        if (!hhdm_type(e->type, &flags)) {
            continue;
        }
        start = e->base & ~(VMM_PAGE_SIZE - 1);
        end = (e->base + e->length + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
        if (!vmm_map_range(&kernel_space, start + hhdm, start, end - start, flags)) {
            return 0;
        }
    }

    /* ৪. early_map দিয়ে আগে ম্যাপ করা ডিভাইস মেমোরি (যেমন LAPIC) */
    for (m = 0; m < early_map_mmio_count(); m++) {
        uint64_t phys, size;
        early_map_mmio_range(m, &phys, &size);
        if (!vmm_map_mmio(phys, size)) {
            return 0;
        }
    }

    /* ৫. নতুন টেবিলে চলে যাওয়া */
    vmm_switch(&kernel_space);

    /* এখন থেকে early_map-ও নতুন টেবিলে কাজ করবে, টেবিল নেবে PMM থেকে */
    early_map_set_table_allocator(pmm_alloc_zeroed_page);
    return 1;
}

struct vmm_space *vmm_kernel_space(void)  { return &kernel_space; }
struct vmm_space *vmm_current_space(void) { return current_space; }
uint64_t vmm_table_pages(void)            { return table_pages; }
int vmm_nx_enabled(void)                  { return nx_ok; }

void vmm_switch(struct vmm_space *sp)
{
    current_space = sp;
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(sp->pml4_phys) : "memory");
}

/* ---------- ইউজার স্পেস (ধাপ ৬-এ ব্যবহৃত হবে) ---------- */

struct vmm_space *vmm_create_space(void)
{
    struct vmm_space *sp = (struct vmm_space *)kmalloc(sizeof(*sp));
    uint64_t *dst, *src;

    if (!sp) {
        return NULL;
    }
    sp->pml4_phys = alloc_table();
    if (!sp->pml4_phys) {
        kfree(sp);
        return NULL;
    }
    dst = table_virt(sp->pml4_phys);
    src = table_virt(kernel_space.pml4_phys);
    memcpy(&dst[256], &src[256], 256 * sizeof(uint64_t));
    return sp;
}

static void free_level(uint64_t phys, int level, int free_frames)
{
    uint64_t *t = table_virt(phys);
    unsigned i;
    for (i = 0; i < PT_ENTRIES; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_PRESENT)) {
            continue;
        }
        if (level > 1 && !(e & PTE_HUGE)) {
            free_level(e & PTE_ADDR_MASK, level - 1, free_frames);
        } else if (level == 1 && (e & PTE_SHARED)) {
            if (shared_hook) shared_hook(e & PTE_ADDR_MASK, -1);   /* ধাপ ১৩ */
        } else if (level == 1 && free_frames) {
            pmm_free_page(e & PTE_ADDR_MASK);
        }
    }
    pmm_free_page(phys);
    table_pages--;
}

void vmm_destroy_space(struct vmm_space *sp, int free_frames)
{
    uint64_t *pml4;
    unsigned i;

    if (!sp || sp == &kernel_space) {
        return;
    }
    if (current_space == sp) {
        vmm_switch(&kernel_space);
    }
    pml4 = table_virt(sp->pml4_phys);
    for (i = 0; i < 256; i++) {                     /* শুধু ইউজার-অর্ধেক */
        if (pml4[i] & PTE_PRESENT) {
            free_level(pml4[i] & PTE_ADDR_MASK, 3, free_frames);
        }
    }
    pmm_free_page(sp->pml4_phys);
    table_pages--;
    kfree(sp);
}

/* ---------- ধাপ ৬: প্রসেসের জন্য সহায়ক ---------- */

/* 4 KiB পেজ ম্যাপ থাকলে 1; *phys ও *flags (VMM_WRITE/VMM_USER/VMM_EXEC) ভরে */
int vmm_query(struct vmm_space *sp, uint64_t virt, uint64_t *phys, uint32_t *flags)
{
    uint64_t *pml4 = table_virt(sp->pml4_phys), *pdpt, *pd, *pt, e;
    uint32_t f = 0;

    if (!(pdpt = next_table(pml4, idx(virt, 39), 0, 0))) return 0;
    if (!(pd   = next_table(pdpt, idx(virt, 30), 0, 0))) return 0;
    if (!(pt   = next_table(pd,   idx(virt, 21), 0, 0))) return 0;
    e = pt[idx(virt, 12)];
    if (!(e & PTE_PRESENT)) {
        return 0;
    }
    if (e & PTE_WRITABLE) f |= VMM_WRITE;
    if (e & PTE_USER)     f |= VMM_USER;
    if (!(e & PTE_NX))    f |= VMM_EXEC;
    if (e & PTE_SHARED)   f |= VMM_SHARED;              /* ধাপ ১৩ */
    if ((e & PTE_PCD) && (e & PTE_PWT)) f |= VMM_NOCACHE;
    else if ((e & PTE_PWT) && (e & PTE_PAT_4K)) f |= VMM_WC;
    if (phys)  *phys = e & PTE_ADDR_MASK;
    if (flags) *flags = f;
    return 1;
}

/* fork: src-এর ইউজার-অর্ধেকের প্রতিটি পেজ নতুন ফ্রেমে কপি করে dst-এ একই ঠিকানায়,
 * একই permission-এ ম্যাপ করে। ব্যর্থ হলে 0 (যা কপি হয়েছে তা dst-এই থাকে —
 * ডাকা ফাংশন vmm_destroy_space(dst, 1) দিয়ে ফেরত দেয়)। */
int vmm_clone_user(struct vmm_space *dst, struct vmm_space *src)
{
    uint64_t *pml4 = table_virt(src->pml4_phys);
    unsigned a, b, c, d;

    for (a = 0; a < 256; a++) {
        uint64_t *pdpt;
        if (!(pml4[a] & PTE_PRESENT)) continue;
        pdpt = table_virt(pml4[a] & PTE_ADDR_MASK);
        for (b = 0; b < PT_ENTRIES; b++) {
            uint64_t *pd;
            if (!(pdpt[b] & PTE_PRESENT) || (pdpt[b] & PTE_HUGE)) continue;
            pd = table_virt(pdpt[b] & PTE_ADDR_MASK);
            for (c = 0; c < PT_ENTRIES; c++) {
                uint64_t *pt;
                if (!(pd[c] & PTE_PRESENT) || (pd[c] & PTE_HUGE)) continue;
                pt = table_virt(pd[c] & PTE_ADDR_MASK);
                for (d = 0; d < PT_ENTRIES; d++) {
                    uint64_t e = pt[d], virt, np;
                    uint32_t f = VMM_USER;
                    if (!(e & PTE_PRESENT) || !(e & PTE_USER)) continue;
                    virt = ((uint64_t)a << 39) | ((uint64_t)b << 30) | ((uint64_t)c << 21) | ((uint64_t)d << 12);
                    if (e & PTE_SHARED) {
                        /* ধাপ ১৩: ভাগ করা পেজ — একই ফ্রেম, একই ফ্ল্যাগ (WC সহ) */
                        uint32_t sf = VMM_USER | VMM_SHARED;
                        if (e & PTE_WRITABLE) sf |= VMM_WRITE;
                        if (!(e & PTE_NX))    sf |= VMM_EXEC;
                        if ((e & PTE_PCD) && (e & PTE_PWT)) sf |= VMM_NOCACHE;
                        else if ((e & PTE_PWT) && (e & PTE_PAT_4K)) sf |= VMM_WC;
                        if (!vmm_map_page(dst, virt, e & PTE_ADDR_MASK, sf)) {
                            return 0;
                        }
                        if (shared_hook) shared_hook(e & PTE_ADDR_MASK, +1);
                        continue;
                    }
                    np = pmm_alloc_page();
                    if (!np) {
                        return 0;
                    }
                    memcpy(table_virt(np), table_virt(e & PTE_ADDR_MASK), VMM_PAGE_SIZE);
                    if (e & PTE_WRITABLE) f |= VMM_WRITE;
                    if (!(e & PTE_NX))    f |= VMM_EXEC;
                    if (!vmm_map_page(dst, virt, np, f)) {
                        pmm_free_page(np);
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}
