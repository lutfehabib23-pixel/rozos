/* rozOS — ELF64 লোডার (ধাপ ৬): স্ট্যাটিক x86-64 এক্সিকিউটেবল (ET_EXEC) */
#ifndef ROZOS_ELF_H
#define ROZOS_ELF_H

#include <stdint.h>

#define ELF_MAX_PHDRS 32

struct vmm_space;

struct elf_image {
    uint64_t entry;
    uint64_t brk_start;      /* সবচেয়ে উঁচু সেগমেন্টের শেষ (পেজ-সীমায়) */
    uint64_t phdr_vaddr;     /* AT_PHDR (জানা গেলে, নইলে 0) */
    uint16_t phnum;
};

/* path-এর ELF space-এ লোড করে; সফল হলে 0, নইলে -ENOEXEC/-ENOENT/-ENOMEM ... */
int elf_load(struct vmm_space *space, const char *path, struct elf_image *out);

#endif
