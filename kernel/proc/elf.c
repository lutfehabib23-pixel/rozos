/* rozOS — ELF64 লোডার (rozos/elf.h দেখুন)
 *
 * শুধু স্ট্যাটিক, non-PIE x86-64 এক্সিকিউটেবল (ET_EXEC)। প্রতিটি PT_LOAD সেগমেন্টের
 * পেজগুলো নতুন শূন্য ফ্রেমে ম্যাপ হয় (VMM_USER, PF_W → VMM_WRITE, PF_X → VMM_EXEC),
 * ফাইলের অংশ HHDM দিয়ে সরাসরি ফ্রেমে পড়া হয়, বাকিটা (.bss) শূন্য থাকে।
 * দুটো সেগমেন্ট একই পেজ ভাগ করলে পেজটা একবারই বরাদ্দ হয়, permission মিলিয়ে নেওয়া হয়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/elf.h>
#include <rozos/vmm.h>
#include <rozos/pmm.h>
#include <rozos/vfs.h>
#include <rozos/proc.h>
#include <rozos/errno.h>
#include <rozos/string.h>

#define EI_NIDENT   16
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_PHDR     6
#define PF_X        1
#define PF_W        2

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* কম্পাইল-সময়ের আকার যাচাই (TCC-বান্ধব: _Static_assert নয়) */
typedef char elf_ehdr_size_check[(sizeof(struct elf64_ehdr) == 64) ? 1 : -1];
typedef char elf_phdr_size_check[(sizeof(struct elf64_phdr) == 56) ? 1 : -1];

static int read_at(int fd, uint64_t off, void *buf, size_t len)
{
    int64_t n;
    if (vfs_seek(fd, (int64_t)off, SEEK_SET) < 0) {
        return -EIO;
    }
    while (len > 0) {
        n = vfs_read(fd, buf, len);
        if (n <= 0) {
            return n < 0 ? (int)n : -ENOEXEC;    /* ফাইল ছোট — ভাঙা ELF */
        }
        buf = (uint8_t *)buf + n;
        len -= (size_t)n;
    }
    return 0;
}

/* একটা পেজ নিশ্চিত করা: না থাকলে নতুন শূন্য ফ্রেম; থাকলে permission বাড়ানো */
static int ensure_page(struct vmm_space *sp, uint64_t va, uint32_t flags, uint64_t *phys_out)
{
    uint64_t phys;
    uint32_t old;

    if (vmm_query(sp, va, &phys, &old)) {
        if ((old | flags) != old) {
            if (!vmm_map_page(sp, va, phys, old | flags)) {
                return -ENOMEM;
            }
        }
        *phys_out = phys;
        return 0;
    }
    phys = pmm_alloc_zeroed_page();
    if (!phys) {
        return -ENOMEM;
    }
    if (!vmm_map_page(sp, va, phys, flags)) {
        pmm_free_page(phys);
        return -ENOMEM;
    }
    *phys_out = phys;
    return 0;
}

static int load_segment(int fd, struct vmm_space *sp, const struct elf64_phdr *ph)
{
    uint32_t flags = VMM_USER;
    uint64_t va, start, end, file_end;
    int err;

    if (ph->p_memsz == 0) {
        return 0;
    }
    if (ph->p_filesz > ph->p_memsz || ph->p_vaddr < USER_MIN_ADDR ||
        ph->p_vaddr + ph->p_memsz < ph->p_vaddr ||
        ph->p_vaddr + ph->p_memsz > USER_MMAP_BASE) {
        return -ENOEXEC;
    }
    if (ph->p_flags & PF_W) flags |= VMM_WRITE;
    if (ph->p_flags & PF_X) flags |= VMM_EXEC;

    start = ph->p_vaddr & ~(VMM_PAGE_SIZE - 1);
    end = (ph->p_vaddr + ph->p_memsz + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    file_end = ph->p_vaddr + ph->p_filesz;

    for (va = start; va < end; va += VMM_PAGE_SIZE) {
        uint64_t phys, lo, hi;
        err = ensure_page(sp, va, flags, &phys);
        if (err < 0) {
            return err;
        }
        /* এই পেজে ফাইলের কোন অংশ পড়বে: [max(va, vaddr), min(va+4K, file_end)) */
        lo = va > ph->p_vaddr ? va : ph->p_vaddr;
        hi = va + VMM_PAGE_SIZE < file_end ? va + VMM_PAGE_SIZE : file_end;
        if (lo < hi) {
            uint8_t *dst = (uint8_t *)pmm_phys_to_virt(phys) + (lo - va);
            err = read_at(fd, ph->p_offset + (lo - ph->p_vaddr), dst, (size_t)(hi - lo));
            if (err < 0) {
                return err;
            }
        }
    }
    return 0;
}

int elf_load(struct vmm_space *space, const char *path, struct elf_image *out)
{
    struct elf64_ehdr eh;
    struct elf64_phdr ph[ELF_MAX_PHDRS];
    struct vfs_stat st;
    uint64_t top = 0;
    int fd, err, i;

    memset(out, 0, sizeof(*out));
    fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    err = vfs_fstat(fd, &st);
    if (err == 0 && st.type != VNODE_FILE) {
        err = -EACCES;
    }
    if (err == 0) {
        err = read_at(fd, 0, &eh, sizeof(eh));
    }
    if (err == 0) {
        if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
            eh.e_ident[3] != 'F' || eh.e_ident[4] != ELFCLASS64 || eh.e_ident[5] != ELFDATA2LSB ||
            eh.e_type != ET_EXEC || eh.e_machine != EM_X86_64 ||
            eh.e_phentsize != sizeof(struct elf64_phdr) ||
            eh.e_phnum == 0 || eh.e_phnum > ELF_MAX_PHDRS) {
            err = -ENOEXEC;
        }
    }
    if (err == 0) {
        err = read_at(fd, eh.e_phoff, ph, (size_t)eh.e_phnum * sizeof(ph[0]));
    }
    for (i = 0; err == 0 && i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            err = load_segment(fd, space, &ph[i]);
            if (ph[i].p_vaddr + ph[i].p_memsz > top) {
                top = ph[i].p_vaddr + ph[i].p_memsz;
            }
            /* প্রথম সেগমেন্টে ELF হেডার থাকলে phdr-এর ঠিকানা জানা যায় (AT_PHDR) */
            if (ph[i].p_offset == 0 && eh.e_phoff < ph[i].p_filesz) {
                out->phdr_vaddr = ph[i].p_vaddr + eh.e_phoff;
            }
        } else if (ph[i].p_type == PT_PHDR) {
            out->phdr_vaddr = ph[i].p_vaddr;
        }
    }
    if (err == 0 && top == 0) {
        err = -ENOEXEC;
    }
    if (err == 0 && (eh.e_entry < USER_MIN_ADDR || eh.e_entry >= top)) {
        err = -ENOEXEC;
    }
    vfs_close(fd);
    if (err < 0) {
        return err;
    }
    out->entry = eh.e_entry;
    out->brk_start = (top + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    out->phnum = eh.e_phnum;
    return 0;
}
