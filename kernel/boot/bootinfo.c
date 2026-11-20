/* rozOS — Limine response-গুলো কার্নেলের নিজস্ব মেমোরিতে কপি করা (bootinfo.h দেখুন) */

#include <stdint.h>
#include <stddef.h>
#include <rozos/bootinfo.h>
#include <rozos/boot.h>
#include <rozos/string.h>

static struct bootinfo info;

static void copy_str(char *dst, size_t size, const char *src)
{
    size_t n;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strnlen(src, size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int bootinfo_save(void)
{
    struct limine_bootloader_info_response *bi = limine_bootloader_info_req.response;
    struct limine_firmware_type_response *fw = limine_firmware_type_req.response;
    struct limine_paging_mode_response *pm = limine_paging_mode_req.response;
    struct limine_hhdm_response *hh = limine_hhdm_req.response;
    struct limine_executable_address_response *ea = limine_exec_addr_req.response;
    struct limine_executable_cmdline_response *cl = limine_cmdline_req.response;
    struct limine_memmap_response *mm = limine_memmap_req.response;
    struct limine_module_response *mods = limine_module_req.response;
    struct limine_rsdp_response *rs = limine_rsdp_req.response;
    uint64_t i;

    memset(&info, 0, sizeof(info));
    if (!hh || !mm) {
        return 0;
    }
    info.hhdm_offset = hh->offset;

    if (bi) {
        copy_str(info.bootloader_name, sizeof(info.bootloader_name), bi->name);
        copy_str(info.bootloader_version, sizeof(info.bootloader_version), bi->version);
    }
    if (fw) {
        info.firmware_type = fw->firmware_type;
        info.firmware_known = 1;
    }
    if (pm) {
        info.paging_mode = pm->mode;
        info.paging_mode_known = 1;
    }
    if (ea) {
        info.kernel_phys = ea->physical_base;
        info.kernel_virt = ea->virtual_base;
    }
    if (cl) {
        copy_str(info.cmdline, sizeof(info.cmdline), cl->cmdline);
    }
    if (rs && rs->address) {
        /* PROTOCOL.md: base revision 3-এ ফিজিক্যাল, অন্য সবগুলোতে HHDM ভার্চুয়াল */
        uint64_t a = (uint64_t)(uintptr_t)rs->address;
        info.rsdp_phys = (a >= info.hhdm_offset) ? a - info.hhdm_offset : a;
    }

    for (i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (info.memmap_count >= BOOTINFO_MAX_MEMMAP) {
            info.memmap_truncated++;
            continue;
        }
        info.memmap[info.memmap_count].base   = e->base;
        info.memmap[info.memmap_count].length = e->length;
        info.memmap[info.memmap_count].type   = e->type;
        info.memmap_count++;
    }

    if (mods) {
        for (i = 0; i < mods->module_count && i < BOOTINFO_MAX_MODULES; i++) {
            struct limine_file *f = mods->modules[i];
            struct boot_module *m = &info.modules[info.module_count++];
            m->virt = (uint64_t)(uintptr_t)f->address;
            m->phys = m->virt - info.hhdm_offset;
            m->size = f->size;
            copy_str(m->path, sizeof(m->path), f->path);
            copy_str(m->cmdline, sizeof(m->cmdline), f->string);
        }
    }
    return 1;
}

const struct bootinfo *bootinfo(void)
{
    return &info;
}
