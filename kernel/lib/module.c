/* rozOS — কার্নেল মডিউল লোডার (ধাপ ১৫ক), rozos/module.h দেখুন
 *
 * ELF64 relocatable (ET_REL, x86-64): SHF_ALLOC অংশগুলো (.text .data .rodata .bss ...)
 * এক জায়গায় বসানো (MOD_BASE থেকে, পেজে, লেখা+চালানো যায়), প্রতীক মেলানো (মডিউলের নিজের,
 * নইলে কার্নেলের রপ্তানি-তালিকা), তারপর relocation: R_X86_64_64/PC32/PLT32/32/32S/PC64,
 * আর GOTPCREL(X) — তার জন্য ছোট একটা GOT। শেষে rozmod_init()। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/module.h>
#include <rozos/heap.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/errno.h>

#define MOD_BASE   0xFFFFFFFFC0000000ULL
#define MOD_LIMIT  0xFFFFFFFFE0000000ULL     /* ৫১২ MiB — যথেষ্ট */
#define MOD_MAX    32

struct e_hdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct e_shdr {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};
struct e_sym {
    uint32_t name;
    uint8_t  info, other;
    uint16_t shndx;
    uint64_t value, size;
};
struct e_rela {
    uint64_t offset, info;
    int64_t  addend;
};

#define SHT_SYMTAB 2
#define SHT_RELA   4
#define SHT_NOBITS 8
#define SHF_ALLOC  0x2
#define SHN_UNDEF  0
#define SHN_ABS    0xFFF1
#define SHN_COMMON 0xFFF2

struct kmodule {
    int used;
    char name[32], desc[64];
    uint64_t va, pages, bytes;
    int (*init)(void);
    void (*exit)(void);
};

static struct kmodule mods[MOD_MAX];
static uint64_t mod_next = MOD_BASE;

uint64_t ksym_lookup(const char *name)
{
    size_t i;
    for (i = 0; i < ksym_count; i++) {
        if (!strcmp(ksym_table[i].name, name)) return ksym_table[i].addr;
    }
    return 0;
}

static void fail(char *err, size_t n, const char *what, const char *arg)
{
    if (err && n) ksnprintf(err, n, "%s%s%s", what, arg ? ": " : "", arg ? arg : "");
}

static void free_area(uint64_t va, uint64_t pages)
{
    uint64_t i;
    for (i = 0; i < pages; i++) {
        uint64_t phys = vmm_unmap_page(vmm_kernel_space(), va + i * VMM_PAGE_SIZE);
        if (phys) pmm_free_page(phys);
    }
}

static int fits_s32(int64_t v) { return v >= -2147483648LL && v <= 2147483647LL; }

int module_load(const void *image, size_t len, const char *name, char *err, size_t errlen)
{
    const uint8_t *img = (const uint8_t *)image;
    const struct e_hdr *eh = (const struct e_hdr *)image;
    const struct e_shdr *sh;
    uint64_t *secaddr = NULL, total = 0, common_off, common_size = 0, got_off, ngot = 0, gotused = 0;
    uint64_t va, pages, i;
    struct kmodule *m = NULL;
    const struct e_sym *syms = NULL;
    const char *strtab = NULL;
    uint64_t nsyms = 0, *symval = NULL;
    int rc = -ENOEXEC, slot;

    if (len < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 ||
        eh->type != 1 || eh->machine != 62) {
        fail(err, errlen, "not an x86-64 relocatable ELF file (compile with -c)", NULL);
        return -ENOEXEC;
    }
    if (eh->shoff == 0 || eh->shentsize != sizeof(struct e_shdr) ||
        eh->shoff + (uint64_t)eh->shnum * sizeof(struct e_shdr) > len) {
        fail(err, errlen, "bad section table", NULL);
        return -ENOEXEC;
    }
    for (slot = 0; slot < MOD_MAX && mods[slot].used; slot++) { }
    if (slot == MOD_MAX) { fail(err, errlen, "too many modules", NULL); return -ENOMEM; }
    for (i = 0; i < MOD_MAX; i++) {
        if (mods[i].used && name && !strcmp(mods[i].name, name)) {
            fail(err, errlen, "already loaded", name);
            return -EEXIST;
        }
    }
    sh = (const struct e_shdr *)(img + eh->shoff);
    secaddr = (uint64_t *)kzalloc(sizeof(uint64_t) * eh->shnum);
    if (!secaddr) return -ENOMEM;

    /* ১) প্রতীক-টেবিল, COMMON আর GOT-এর মাপ */
    for (i = 0; i < eh->shnum; i++) {
        if (sh[i].type == SHT_SYMTAB) {
            if (sh[i].offset + sh[i].size > len || sh[i].link >= eh->shnum ||
                sh[sh[i].link].offset + sh[sh[i].link].size > len) {
                fail(err, errlen, "bad symbol table", NULL);
                goto out;
            }
            syms = (const struct e_sym *)(img + sh[i].offset);
            nsyms = sh[i].size / sizeof(struct e_sym);
            strtab = (const char *)(img + sh[sh[i].link].offset);
        }
    }
    if (!syms) { fail(err, errlen, "no symbol table", NULL); goto out; }
    for (i = 0; i < nsyms; i++) {
        if (syms[i].shndx == SHN_COMMON) {
            uint64_t al = syms[i].value ? syms[i].value : 8;
            common_size = (common_size + al - 1) & ~(al - 1);
            common_size += syms[i].size;
        }
    }
    for (i = 0; i < eh->shnum; i++) {
        if (sh[i].type == SHT_RELA && sh[i].info < eh->shnum && (sh[sh[i].info].flags & SHF_ALLOC)) {
            const struct e_rela *r = (const struct e_rela *)(img + sh[i].offset);
            uint64_t k, n = sh[i].size / sizeof(struct e_rela);
            if (sh[i].offset + sh[i].size > len) { fail(err, errlen, "bad relocation table", NULL); goto out; }
            for (k = 0; k < n; k++) {
                uint32_t t = (uint32_t)r[k].info;
                if (t == 9 || t == 41 || t == 42) ngot++;
            }
        }
    }

    /* ২) জায়গার হিসাব: SHF_ALLOC অংশ, তারপর COMMON, তারপর GOT */
    for (i = 0; i < eh->shnum; i++) {
        if (!(sh[i].flags & SHF_ALLOC) || sh[i].size == 0) continue;
        {
            uint64_t al = sh[i].addralign ? sh[i].addralign : 1;
            total = (total + al - 1) & ~(al - 1);
            secaddr[i] = total;                    /* এখন offset; পরে ঠিকানা */
            total += sh[i].size;
        }
    }
    total = (total + 15) & ~15ULL;
    common_off = total;
    total += common_size;
    total = (total + 7) & ~7ULL;
    got_off = total;
    total += ngot * 8;
    pages = (total + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    if (pages == 0) pages = 1;
    if (mod_next + pages * VMM_PAGE_SIZE > MOD_LIMIT) { fail(err, errlen, "module area full", NULL); rc = -ENOMEM; goto out; }

    /* ৩) পেজ বসানো (লেখা + চালানো যায়) আর অংশ কপি */
    va = mod_next;
    for (i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_zeroed_page();
        if (!phys || !vmm_map_page(vmm_kernel_space(), va + i * VMM_PAGE_SIZE, phys, VMM_WRITE | VMM_EXEC)) {
            if (phys) pmm_free_page(phys);
            free_area(va, i);
            fail(err, errlen, "out of memory", NULL);
            rc = -ENOMEM;
            goto out;
        }
    }
    mod_next += (pages + 1) * VMM_PAGE_SIZE;     /* এক পেজ ফাঁক: ভুলে উপচালে ধরা পড়ে */
    for (i = 0; i < eh->shnum; i++) {
        if (!(sh[i].flags & SHF_ALLOC) || sh[i].size == 0) continue;
        secaddr[i] += va;
        if (sh[i].type != SHT_NOBITS) {
            if (sh[i].offset + sh[i].size > len) { fail(err, errlen, "section outside the file", NULL); goto bad; }
            memcpy((void *)(uintptr_t)secaddr[i], img + sh[i].offset, sh[i].size);
        }
    }

    /* ৪) প্রতীকের ঠিকানা */
    symval = (uint64_t *)kzalloc(sizeof(uint64_t) * (nsyms ? nsyms : 1));
    if (!symval) { rc = -ENOMEM; goto bad; }
    {
        uint64_t cpos = 0;
        for (i = 1; i < nsyms; i++) {
            const struct e_sym *s = &syms[i];
            const char *nm = strtab + s->name;
            if (s->shndx == SHN_UNDEF) {
                if (!nm[0]) continue;
                symval[i] = ksym_lookup(nm);
                if (!symval[i] && (s->info >> 4) != 2) {          /* weak (2) হলে 0 চলবে */
                    fail(err, errlen, "unknown kernel symbol (not exported)", nm);
                    goto bad;
                }
            } else if (s->shndx == SHN_ABS) {
                symval[i] = s->value;
            } else if (s->shndx == SHN_COMMON) {
                uint64_t al = s->value ? s->value : 8;
                cpos = (cpos + al - 1) & ~(al - 1);
                symval[i] = va + common_off + cpos;
                cpos += s->size;
            } else if (s->shndx < eh->shnum) {
                symval[i] = secaddr[s->shndx] + s->value;
            }
        }
    }

    /* ৫) relocation */
    for (i = 0; i < eh->shnum; i++) {
        const struct e_rela *r;
        uint64_t k, n, tgt;
        if (sh[i].type != SHT_RELA || sh[i].info >= eh->shnum || !(sh[sh[i].info].flags & SHF_ALLOC)) continue;
        r = (const struct e_rela *)(img + sh[i].offset);
        n = sh[i].size / sizeof(struct e_rela);
        tgt = secaddr[sh[i].info];
        for (k = 0; k < n; k++) {
            uint32_t type = (uint32_t)r[k].info, si = (uint32_t)(r[k].info >> 32);
            uint64_t P = tgt + r[k].offset, S = si < nsyms ? symval[si] : 0;
            int64_t A = r[k].addend, v;
            if (r[k].offset + 4 > sh[sh[i].info].size) { fail(err, errlen, "relocation outside its section", NULL); goto bad; }
            switch (type) {
            case 0: break;
            case 1:  *(uint64_t *)(uintptr_t)P = S + (uint64_t)A; break;                  /* R_X86_64_64 */
            case 24: *(uint64_t *)(uintptr_t)P = S + (uint64_t)A - P; break;              /* PC64 */
            case 2: case 4:                                                                /* PC32, PLT32 */
                v = (int64_t)(S + (uint64_t)A - P);
                if (!fits_s32(v)) { fail(err, errlen, "PC32 relocation out of range", strtab + syms[si].name); goto bad; }
                *(int32_t *)(uintptr_t)P = (int32_t)v;
                break;
            case 10:                                                                       /* 32 */
                if (S + (uint64_t)A > 0xFFFFFFFFULL) { fail(err, errlen, "R_X86_64_32 out of range (use -mcmodel=kernel)", NULL); goto bad; }
                *(uint32_t *)(uintptr_t)P = (uint32_t)(S + (uint64_t)A);
                break;
            case 11:                                                                       /* 32S */
                v = (int64_t)(S + (uint64_t)A);
                if (!fits_s32(v)) { fail(err, errlen, "R_X86_64_32S out of range", NULL); goto bad; }
                *(int32_t *)(uintptr_t)P = (int32_t)v;
                break;
            case 9: case 41: case 42: {                                                    /* GOTPCREL(X) */
                uint64_t G = va + got_off + gotused * 8;
                *(uint64_t *)(uintptr_t)G = S;
                gotused++;
                v = (int64_t)(G + (uint64_t)A - P);
                if (!fits_s32(v)) { fail(err, errlen, "GOTPCREL out of range", NULL); goto bad; }
                *(int32_t *)(uintptr_t)P = (int32_t)v;
                break;
            }
            default: {
                char t[24];
                ksnprintf(t, sizeof(t), "%u", type);
                fail(err, errlen, "unsupported relocation type", t);
                goto bad;
            }
            }
        }
    }

    /* ৬) নাম, init/exit */
    m = &mods[slot];
    memset(m, 0, sizeof(*m));
    for (i = 1; i < nsyms; i++) {
        const char *nm = strtab + syms[i].name;
        if (syms[i].shndx == SHN_UNDEF) continue;
        if (!strcmp(nm, "rozmod_init")) m->init = (int (*)(void))(uintptr_t)symval[i];
        else if (!strcmp(nm, "rozmod_exit")) m->exit = (void (*)(void))(uintptr_t)symval[i];
        else if (!strcmp(nm, "rozmod_name")) strncpy(m->name, (const char *)(uintptr_t)symval[i], sizeof(m->name) - 1);
        else if (!strcmp(nm, "rozmod_desc")) strncpy(m->desc, (const char *)(uintptr_t)symval[i], sizeof(m->desc) - 1);
    }
    if (!m->name[0] && name) strncpy(m->name, name, sizeof(m->name) - 1);
    if (!m->name[0]) strcpy(m->name, "module");
    for (i = 0; i < MOD_MAX; i++) {
        if ((int)i != slot && mods[i].used && !strcmp(mods[i].name, m->name)) {
            fail(err, errlen, "already loaded", m->name);
            rc = -EEXIST;
            goto bad;
        }
    }
    if (!m->init) { fail(err, errlen, "no rozmod_init() in the module", NULL); goto bad; }
    m->va = va;
    m->pages = pages;
    m->bytes = total;
    m->used = 1;
    rc = m->init();
    if (rc < 0) {
        m->used = 0;
        fail(err, errlen, "rozmod_init() refused", NULL);
        free_area(va, pages);
        goto out;
    }
    kprintf("[module] %s loaded at 0x%llx (%llu bytes)%s%s\n", m->name, (unsigned long long)va,
            (unsigned long long)total, m->desc[0] ? " - " : "", m->desc);
    rc = 0;
    goto out;
bad:
    if (rc == -ENOEXEC || rc == 0) rc = -ENOEXEC;
    if (m) m->used = 0;
    free_area(va, pages);
out:
    kfree(symval);
    kfree(secaddr);
    return rc;
}

int module_unload(const char *name)
{
    int i;
    for (i = 0; i < MOD_MAX; i++) {
        if (mods[i].used && !strcmp(mods[i].name, name)) {
            if (mods[i].exit) mods[i].exit();
            free_area(mods[i].va, mods[i].pages);
            mods[i].used = 0;
            kprintf("[module] %s removed\n", name);
            return 0;
        }
    }
    return -ENOENT;
}

int64_t module_list(char *buf, size_t len, int kernel_symbols)
{
    size_t o = 0, i;
    if (len == 0) return 0;
    buf[0] = '\0';
    if (kernel_symbols) {
        for (i = 0; i < ksym_count && o + 1 < len; i++)
            o += (size_t)ksnprintf(buf + o, len - o, "%s\n", ksym_table[i].name);
    } else {
        for (i = 0; i < MOD_MAX && o + 1 < len; i++) {
            if (!mods[i].used) continue;
            o += (size_t)ksnprintf(buf + o, len - o, "%s %llu %s\n", mods[i].name,
                                   (unsigned long long)mods[i].bytes, mods[i].desc);
        }
    }
    if (o >= len) o = len - 1;
    return (int64_t)o;
}
