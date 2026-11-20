/* rozOS — কার্নেলের মূল প্রবেশ বিন্দু (entry.S থেকে ডাকা হয়)
 *
 * ধাপ ১: serial → base revision যাচাই → ফন্ট → ফ্রেমবাফার → tty → বুট তথ্য ও
 * মেমোরি ম্যাপ দেখানো → (ঐচ্ছিক) C++ সাপোর্ট পরীক্ষা।
 * ধাপ ২: GDT/TSS → IDT → early page mapper → PIC (LAPIC LINT0 দিয়ে) → PIT টাইমার
 * (1000 Hz) → PS/2 কিবোর্ড ও মাউস → ইন্টারাপ্ট চালু → kmon (কার্নেল মনিটর)।
 * ধাপ ৩: bootinfo কপি → PMM → VMM (নিজস্ব PML4) → heap (kmalloc, C++ new/delete) →
 * ACPI → LAPIC/I-O APIC → LAPIC টাইমার → bootloader-reclaimable মেমোরি ফেরত।
 * ধাপ ৪: VFS → tmpfs "/" → devfs "/dev" → initrd (tar) "/"-এ খোলা।
 * পরের ধাপগুলো (ডিস্ক, প্রসেস ...) এখানে ক্রমানুসারে যোগ হবে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>
#include <rozos/boot.h>
#include <rozos/serial.h>
#include <rozos/console.h>
#include <rozos/printf.h>
#include <rozos/panic.h>
#include <rozos/timer.h>
#include <rozos/ps2.h>
#include <rozos/keyboard.h>
#include <rozos/mouse.h>
#include <rozos/kmon.h>
#include <rozos/early_map.h>
#include <rozos/bootinfo.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/acpi.h>
#include <rozos/vfs.h>
#include <rozos/devfs.h>
#include <rozos/initrd.h>
#include <rozos/errno.h>
#ifdef ROZOS_KCXX
#include <rozos/kcxx.h>
#endif
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/pic.h>
#include <arch/x86_64/pit.h>
#include <arch/x86_64/apic.h>

#define ROZOS_VERSION "0.4.0-phase4"

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

/* ধাপ ৩ থেকে Limine response সরাসরি নয়, bootinfo কপি থেকে পড়া হয় — reclaim-এর
 * পরেও (যেমন kmon থেকে) এটা নিরাপদে ডাকা যায় */
static void print_boot_info(int revision)
{
    const struct bootinfo *bi = bootinfo();

    kprintf("\nBoot information\n");
    kprintf("  Bootloader     : %s %s\n",
            bi->bootloader_name[0] ? bi->bootloader_name : "?", bi->bootloader_version);
    kprintf("  Base revision  : %d (requested %d)\n", revision, ROZOS_LIMINE_BASE_REVISION);
    kprintf("  Firmware       : %s\n", bi->firmware_known ? boot_firmware_name(bi->firmware_type) : "?");
    kprintf("  Paging mode    : %s\n",
            !bi->paging_mode_known ? "?" :
            (bi->paging_mode == LIMINE_PAGING_MODE_X86_64_4LVL ? "4-level (PML4)" : "5-level"));
    kprintf("  HHDM offset    : 0x%016llx\n", (unsigned long long)bi->hhdm_offset);
    if (bi->kernel_virt) {
        kprintf("  Kernel phys    : 0x%016llx\n", (unsigned long long)bi->kernel_phys);
        kprintf("  Kernel virt    : 0x%016llx\n", (unsigned long long)bi->kernel_virt);
    }
    kprintf("  Kernel size    : %llu KiB\n",
            (unsigned long long)((uintptr_t)__kernel_end - (uintptr_t)__kernel_start) / 1024);
    kprintf("  Command line   : \"%s\"\n", bi->cmdline);
    kprintf("  Modules        : %llu\n", (unsigned long long)bi->module_count);
    kprintf("  ACPI RSDP      : 0x%llx (phys)\n", (unsigned long long)bi->rsdp_phys);
    kprintf("  Console        : %s", tty_backend_name());
    if (tty_backend() == TTY_BACKEND_FB) {
        kprintf(" %llux%llu@%u, %ux%u chars",
                (unsigned long long)fb_width(), (unsigned long long)fb_height(),
                (unsigned)fb_bpp(), (unsigned)tty_cols(), (unsigned)tty_rows());
    }
    kprintf("\n");
}

/* kmon-এর "mem" কমান্ডও এটা ব্যবহার করে (rozos/kmon.h) */
void print_memory_map(void)
{
    const struct bootinfo *bi = bootinfo();
    uint64_t usable = 0, reclaimable = 0, i;

    if (!bi->memmap_count) {
        print_warn("no memory map from the bootloader");
        return;
    }

    kprintf("\nMemory map (%llu entries)\n", (unsigned long long)bi->memmap_count);
    kprintf("  %-18s %-18s %10s  %s\n", "Base", "End", "Size", "Type");
    for (i = 0; i < bi->memmap_count; i++) {
        const struct boot_memmap_entry *e = &bi->memmap[i];
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
    if (bi->memmap_truncated) {
        kprintf("  (%llu entries did not fit and were ignored)\n",
                (unsigned long long)bi->memmap_truncated);
    }
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

/* ধাপ ২: CPU টেবিল, ইন্টারাপ্ট কন্ট্রোলার, টাইমার ও ইনপুট ডিভাইস */
static void init_interrupts_and_devices(void)
{
    char msg[80];

    gdt_init();
    print_ok("GDT and TSS (IST stacks for #DF, NMI, #MC)");

    idt_init();
    print_ok("IDT: 256 vectors, exception handlers");

    if (!early_map_init()) {
        kpanic("early page mapper: missing HHDM or executable address response");
    }
    print_ok("Early page mapper (MMIO)");

    pic_init();
    if (!pic_lapic_routed()) {
        kpanic("could not map the local APIC at 0x%llx to unmask LINT0 for the PIC",
               (unsigned long long)pic_lapic_phys_base());
    }
    print_ok("8259 PIC remapped to vectors 0x20-0x2F (via LAPIC LINT0)");

    timer_init(TIMER_DEFAULT_HZ);
    ksnprintf(msg, sizeof(msg), "PIT timer at %u Hz (IRQ0)", (unsigned)timer_frequency());
    print_ok(msg);

    if (ps2_init()) {
        print_ok("PS/2 controller");
        if (keyboard_init()) {
            print_ok("PS/2 keyboard (US QWERTY, IRQ1)");
        } else {
            print_warn("PS/2 keyboard did not respond");
        }
        if (mouse_init()) {
            if (fb_is_ready()) {
                mouse_set_bounds((int32_t)fb_width(), (int32_t)fb_height());
            }
            print_ok("PS/2 mouse (IRQ12)");
        } else {
            print_warn("no PS/2 mouse");
        }
    } else {
        print_warn("no PS/2 controller: keyboard input via serial only");
    }

    /* এখান থেকে হার্ডওয়্যার ইন্টারাপ্ট চালু। টাইমার না এলে timer_sleep_ms()
     * চিরকাল ঘুমিয়ে থাকত, তাই প্রথম টিকের জন্য সীমিত সময় busy-wait করা হয়
     * (TSC-তে প্রায় কয়েক শত মিলিয়ন সাইকেল — আধুনিক CPU-তে এক সেকেন্ডেরও কম)। */
    cpu_sti();
    {
        uint64_t start = cpu_rdtsc();
        while (timer_ticks() == 0 && cpu_rdtsc() - start < 3000000000ULL) {
            cpu_pause();
        }
    }
    if (timer_ticks() == 0) {
        kpanic("timer interrupts are not arriving (IRQ0) - interrupt routing problem");
    }
    ksnprintf(msg, sizeof(msg), "Interrupts enabled (early map used %u/%u tables)",
              (unsigned)early_map_pool_used(), (unsigned)early_map_pool_size());
    print_ok(msg);
}

/* ধাপ ৩: ফিজিক্যাল ও ভার্চুয়াল মেমোরি, heap */
static void init_memory(void)
{
    char msg[96];
    struct pmm_stats ps;

    if (!pmm_init()) {
        kpanic("PMM: no usable memory for the page bitmap");
    }
    pmm_get_stats(&ps);
    ksnprintf(msg, sizeof(msg), "PMM (bitmap): %llu MiB free, bitmap %llu KiB",
              (unsigned long long)(ps.free_pages * PMM_PAGE_SIZE / (1024 * 1024)),
              (unsigned long long)(ps.bitmap_bytes / 1024));
    print_ok(msg);

    if (!vmm_init()) {
        kpanic("VMM: could not build the kernel page tables");
    }
    ksnprintf(msg, sizeof(msg), "VMM: own PML4 at 0x%llx, %llu table pages, NX %s",
              (unsigned long long)vmm_kernel_space()->pml4_phys,
              (unsigned long long)vmm_table_pages(), vmm_nx_enabled() ? "on" : "off");
    print_ok(msg);

    heap_init();
    if (!heap_selftest()) {
        kpanic("kernel heap self-test failed");
    }
    print_ok("Kernel heap: kmalloc/kfree (size classes 16-2048 + page allocations)");

#ifdef ROZOS_KCXX
    kcxx_set_allocator(kmalloc, kfree);
    if (kcxx_heap_selftest()) {
        print_ok("C++ new/delete on the kernel heap");
    } else {
        print_warn("C++ new/delete self-test FAILED");
    }
#endif

    /* VGA টেক্সট-মোড: শুধু BIOS বুটে অর্থবহ; ফ্রেমবাফার থাকলে সেটাই ব্যবহার হয় */
    if (bootinfo()->firmware_known && bootinfo()->firmware_type == LIMINE_FIRMWARE_TYPE_X86BIOS) {
        volatile void *vga = vmm_map_mmio(VGA_TEXT_PHYS, VGA_COLS * VGA_ROWS * 2);
        if (vga) {
            tty_attach_vga(vga);
        }
    }
}

/* ধাপ ৩: ACPI ও APIC; না পেলে PIC + PIT চালু থাকে */
static void init_apic(void)
{
    char msg[96];
    const struct apic_info *ai;

    if (!acpi_init()) {
        print_warn("ACPI not found: staying on 8259 PIC + PIT");
        return;
    }
    ksnprintf(msg, sizeof(msg), "ACPI %s (OEM \"%s\"): %u tables",
              acpi_revision() >= 2 ? "2.0+" : "1.0", acpi_oem_id(),
              (unsigned)acpi_table_count());
    print_ok(msg);

    if (!apic_init()) {
        print_warn("no usable MADT/I-O APIC: staying on 8259 PIC + PIT");
        return;
    }
    ai = apic_get_info();
    ksnprintf(msg, sizeof(msg), "Local APIC id %u (%s), %u CPU(s), %u I/O APIC(s); 8259 PIC disabled",
              (unsigned)ai->bsp_lapic_id, ai->x2apic ? "x2APIC" : "xAPIC",
              (unsigned)ai->cpu_count, (unsigned)ai->ioapic_count);
    print_ok(msg);

    if (apic_timer_init(TIMER_DEFAULT_HZ)) {
        ksnprintf(msg, sizeof(msg), "LAPIC timer at %u Hz (calibrated with PIT: %u ticks/ms)",
                  (unsigned)timer_frequency(), (unsigned)ai->lapic_timer_ticks_per_ms);
        print_ok(msg);
    } else {
        print_warn("LAPIC timer calibration failed: keeping PIT");
    }
}

/* ধাপ ৪: ফাইলসিস্টেম। initrd মডিউলের ডেটা executable_and_modules অঞ্চলে থাকে
 * (reclaim হয় না), আর এর তথ্য bootinfo-তে কপি করা — তাই এটা reclaim-এর আগে বা পরে
 * যেকোনো সময় চলতে পারে। */
static void init_vfs(void)
{
    char msg[128];
    struct initrd_stats st;
    int err;

    vfs_init();
    vfs_register_filesystem(&tmpfs_filesystem);
    devfs_init();

    err = vfs_mount("/", "tmpfs", NULL);
    if (err < 0) {
        kpanic("could not mount tmpfs on /: %s", kstrerror(err));
    }
    print_ok("VFS: tmpfs mounted on /");

    err = initrd_load("/", &st);
    if (err == -ENOENT) {
        print_warn("no initrd module: / is empty");
    } else if (err < 0 || st.errors) {
        ksnprintf(msg, sizeof(msg), "initrd %s: %llu files, %llu errors",
                  st.module_path, (unsigned long long)st.files, (unsigned long long)st.errors);
        print_warn(msg);
    } else {
        ksnprintf(msg, sizeof(msg), "initrd %s (%llu KiB): %llu files, %llu dirs, %llu bytes",
                  st.module_path, (unsigned long long)(st.archive_size / 1024),
                  (unsigned long long)st.files, (unsigned long long)st.dirs,
                  (unsigned long long)st.bytes);
        print_ok(msg);
    }

    /* /dev, /tmp নিশ্চিত (initrd-এ না থাকলেও) */
    vfs_mkdir_p("/dev");
    vfs_mkdir_p("/tmp");
    err = vfs_mount("/dev", "devfs", NULL);
    if (err < 0) {
        ksnprintf(msg, sizeof(msg), "could not mount devfs on /dev: %s", kstrerror(err));
        print_warn(msg);
    } else {
        print_ok("devfs mounted on /dev (null, zero, console, serial0, kbd)");
    }
}

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

    /* Limine-এর সব তথ্য নিজের মেমোরিতে কপি — পরে bootloader মেমোরি ফেরত নেওয়া হবে */
    if (!bootinfo_save()) {
        kpanic("could not copy boot information (missing HHDM or memory map)");
    }

    if (!limine_hhdm_req.response) {
        kpanic("bootloader did not provide the HHDM response");
    }
    if (!limine_memmap_req.response) {
        kpanic("bootloader did not provide a memory map");
    }

#ifdef ROZOS_KCXX
    check_kcxx();
#endif

    init_interrupts_and_devices();
    init_memory();
    init_apic();
    init_vfs();

    print_boot_info(revision);
    print_memory_map();

    /* শেষ ধাপ: কার্নেল এখন নিজের GDT, IDT, স্ট্যাক, পেজ টেবিল ও bootinfo কপিতে চলে,
     * তাই Limine-এর মেমোরি (response, পেজ টেবিল, স্ট্যাক) ফেরত নেওয়া নিরাপদ */
    {
        char msg[80];
        uint64_t bytes = pmm_reclaim_bootloader_memory();
        ksnprintf(msg, sizeof(msg), "Reclaimed %llu KiB of bootloader memory",
                  (unsigned long long)(bytes / 1024));
        kprintf("\n");
        print_ok(msg);
    }

    tty_set_color(TTY_COLOR_LIGHT_GREEN, TTY_COLOR_BLACK);
    kprintf("\nrozOS booted. Phase 4: virtual file system and initrd ready.\n");
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);

    /* /etc/motd থাকলে দেখানো — initrd থেকে আসা প্রথম "ফাইল" */
    {
        int fd = vfs_open("/etc/motd", O_RDONLY);
        if (fd >= 0) {
            char buf[256];
            int64_t n;
            kprintf("\n");
            while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
                tty_write(buf, (size_t)n);
            }
            vfs_close(fd);
        }
    }

    kmon_run();
}
