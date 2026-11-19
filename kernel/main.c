/* rozOS — কার্নেলের মূল প্রবেশ বিন্দু (entry.S থেকে ডাকা হয়)
 *
 * ধাপ ১: serial → base revision যাচাই → ফন্ট → ফ্রেমবাফার → tty → বুট তথ্য ও
 * মেমোরি ম্যাপ দেখানো → (ঐচ্ছিক) C++ সাপোর্ট পরীক্ষা → থামা।
 * পরের ধাপগুলো (GDT/IDT, PMM, VMM ...) এখানে ক্রমানুসারে যোগ হবে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>
#include <rozos/boot.h>
#include <rozos/serial.h>
#include <rozos/console.h>
#include <rozos/printf.h>
#include <rozos/panic.h>
#ifdef ROZOS_KCXX
#include <rozos/kcxx.h>
#endif
#include <arch/x86_64/cpu.h>

#define ROZOS_VERSION "0.1.0-phase1"

/* linker.ld থেকে */
extern char __kernel_start[], __kernel_end[];

static void print_banner(void)
{
    tty_set_color(TTY_COLOR_LIGHT_CYAN, TTY_COLOR_BLACK);
    kprintf("rozOS %s (x86_64)\n", ROZOS_VERSION);
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    kprintf("--------------------------------------------\n");
}

static void print_ok(const char *what)
{
    tty_set_color(TTY_COLOR_LIGHT_GREEN, TTY_COLOR_BLACK);
    kprintf("[ OK ] ");
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    kprintf("%s\n", what);
}

static void print_warn(const char *what)
{
    tty_set_color(TTY_COLOR_YELLOW, TTY_COLOR_BLACK);
    kprintf("[WARN] ");
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    kprintf("%s\n", what);
}

static void print_boot_info(int revision)
{
    struct limine_bootloader_info_response *bi = limine_bootloader_info_req.response;
    struct limine_firmware_type_response *fw = limine_firmware_type_req.response;
    struct limine_paging_mode_response *pm = limine_paging_mode_req.response;
    struct limine_executable_address_response *ea = limine_exec_addr_req.response;
    struct limine_executable_cmdline_response *cl = limine_cmdline_req.response;
    struct limine_module_response *mods = limine_module_req.response;
    struct limine_rsdp_response *rsdp = limine_rsdp_req.response;

    kprintf("\nBoot information\n");
    kprintf("  Bootloader     : %s %s\n",
            bi ? bi->name : "?", bi ? bi->version : "");
    kprintf("  Base revision  : %d (requested %d)\n", revision, ROZOS_LIMINE_BASE_REVISION);
    kprintf("  Firmware       : %s\n", fw ? boot_firmware_name(fw->firmware_type) : "?");
    kprintf("  Paging mode    : %s\n",
            !pm ? "?" : (pm->mode == LIMINE_PAGING_MODE_X86_64_4LVL ? "4-level (PML4)" : "5-level"));
    kprintf("  HHDM offset    : 0x%016llx\n", (unsigned long long)boot_hhdm_offset());
    if (ea) {
        kprintf("  Kernel phys    : 0x%016llx\n", (unsigned long long)ea->physical_base);
        kprintf("  Kernel virt    : 0x%016llx\n", (unsigned long long)ea->virtual_base);
    }
    kprintf("  Kernel size    : %llu KiB\n",
            (unsigned long long)((uintptr_t)__kernel_end - (uintptr_t)__kernel_start) / 1024);
    kprintf("  Command line   : \"%s\"\n", (cl && cl->cmdline) ? cl->cmdline : "");
    kprintf("  Modules        : %llu\n", mods ? (unsigned long long)mods->module_count : 0ULL);
    kprintf("  ACPI RSDP      : %p\n", rsdp ? rsdp->address : NULL);
    kprintf("  Console        : %s", tty_backend_name());
    if (tty_backend() == TTY_BACKEND_FB) {
        kprintf(" %llux%llu@%u, %ux%u chars",
                (unsigned long long)fb_width(), (unsigned long long)fb_height(),
                (unsigned)fb_bpp(), (unsigned)tty_cols(), (unsigned)tty_rows());
    }
    kprintf("\n");
}

static void print_memory_map(void)
{
    struct limine_memmap_response *mm = limine_memmap_req.response;
    uint64_t usable = 0, reclaimable = 0, i;

    if (!mm) {
        print_warn("no memory map from the bootloader");
        return;
    }

    kprintf("\nMemory map (%llu entries)\n", (unsigned long long)mm->entry_count);
    kprintf("  %-18s %-18s %10s  %s\n", "Base", "End", "Size", "Type");
    for (i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t kib = e->length / 1024;

        kprintf("  0x%016llx 0x%016llx %8llu %s  %s\n",
                (unsigned long long)e->base,
                (unsigned long long)(e->base + e->length),
                (unsigned long long)(kib >= 10240 ? kib / 1024 : kib),
                kib >= 10240 ? "MiB" : "KiB",
                boot_memmap_type_name(e->type));

        if (e->type == LIMINE_MEMMAP_USABLE) {
            usable += e->length;
        } else if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            reclaimable += e->length;
        }
    }
    kprintf("  Usable RAM: %llu MiB, bootloader reclaimable: %llu KiB\n",
            (unsigned long long)(usable / (1024 * 1024)),
            (unsigned long long)(reclaimable / 1024));
}

#ifdef ROZOS_KCXX
static void check_kcxx(void)
{
    size_t ctors = kcxx_init();
    if (kcxx_selftest()) {
        char msg[64];
        ksnprintf(msg, sizeof(msg), "C++ runtime: %zu global constructor(s), virtual calls OK", ctors);
        print_ok(msg);
    } else {
        print_warn("C++ runtime self-test FAILED (check .init_array in linker.ld)");
    }
}
#endif

void kmain(void)
{
    int revision, revision_ok;
    int have_serial, have_font;

    /* ১. serial — সবার আগে, যাতে পরের যেকোনো সমস্যা লগে ধরা পড়ে */
    have_serial = serial_init(SERIAL_COM1);

    /* ২. বুটলোডার আমাদের প্রোটোকল মেনে লোড করেছে কিনা */
    revision = boot_base_revision(&revision_ok);

    /* ৩. স্ক্রিন */
    have_font = font_init();
    fb_init();
    tty_init();

    print_banner();
    if (have_serial) {
        print_ok("Serial console on COM1 (115200 8N1)");
    }
    if (tty_backend() == TTY_BACKEND_FB) {
        print_ok("Framebuffer console");
    } else if (!have_font) {
        print_warn("No bootloader font: screen output disabled, serial only");
    } else {
        print_warn("No usable 32bpp framebuffer: serial only");
    }

    if (!revision_ok) {
        kpanic("Limine base revision %d is too old (need >= %d). Update Limine.",
               revision, ROZOS_LIMINE_MIN_REVISION);
    }
    print_ok("Limine boot protocol");

    if (!limine_hhdm_req.response) {
        kpanic("bootloader did not provide the HHDM response");
    }
    if (!limine_memmap_req.response) {
        kpanic("bootloader did not provide a memory map");
    }

#ifdef ROZOS_KCXX
    check_kcxx();
#endif

    print_boot_info(revision);
    print_memory_map();

    tty_set_color(TTY_COLOR_LIGHT_GREEN, TTY_COLOR_BLACK);
    kprintf("\nrozOS booted. Phase 1 complete - halting.\n");
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);

    cpu_halt_forever();
}
