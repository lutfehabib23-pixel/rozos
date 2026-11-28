/* rozOS — কার্নেলের মূল প্রবেশ বিন্দু (entry.S থেকে ডাকা হয়)
 *
 * ধাপ ১: serial → base revision যাচাই → ফন্ট → ফ্রেমবাফার → tty → বুট তথ্য ও
 * মেমোরি ম্যাপ দেখানো → (ঐচ্ছিক) C++ সাপোর্ট পরীক্ষা।
 * ধাপ ২: GDT/TSS → IDT → early page mapper → PIC (LAPIC LINT0 দিয়ে) → PIT টাইমার
 * (1000 Hz) → PS/2 কিবোর্ড ও মাউস → ইন্টারাপ্ট চালু → kmon (কার্নেল মনিটর)।
 * ধাপ ৩: bootinfo কপি → PMM → VMM (নিজস্ব PML4) → heap (kmalloc, C++ new/delete) →
 * ACPI → LAPIC/I-O APIC → LAPIC টাইমার → bootloader-reclaimable মেমোরি ফেরত।
 * ধাপ ৪: VFS → tmpfs "/" → devfs "/dev" → initrd (tar) "/"-এ খোলা।
 * ধাপ ৫: PCI স্ক্যান → AHCI ডিস্ক (/dev/sdX) → ext2 ডিস্ক থাকলে সেটাই "/",
 *        tmpfs "/tmp"-এ; ডিস্ক না থাকলে আগের মতো tmpfs + initrd।
 * পরের ধাপগুলো (প্রসেস, syscall ...) এখানে ক্রমানুসারে যোগ হবে। */

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
#include <rozos/fbdev.h>
#include <rozos/shm.h>
#include <rozos/initrd.h>
#include <rozos/errno.h>
#include <rozos/pci.h>
#include <rozos/block.h>
#include <rozos/ahci.h>
#include <rozos/nvme.h>
#include <rozos/usb.h>
#include <rozos/fat.h>
#include <rozos/buildinfo.h>
#include <rozos/ext2.h>
#include <rozos/clock.h>
#include <rozos/string.h>
#include <rozos/sched.h>
#include <rozos/proc.h>
#include <rozos/syscall.h>
#include <rozos/net.h>
#include <rozos/random.h>
#ifdef ROZOS_KCXX
#include <rozos/kcxx.h>
#endif
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/pic.h>
#include <arch/x86_64/pit.h>
#include <arch/x86_64/apic.h>

#define ROZOS_VERSION "0.26.2-phase25b"

#ifdef __TINYC__
/* ধাপ ১১: TCC-র লিংকারে linker.ld নেই — শুরু = Limine-এর দেওয়া virtual base,
 * শেষ = TCC-র নিজের _end প্রতীক (.bss-এর শেষ) */
extern char _end[];
#define KERNEL_IMAGE_START ((uintptr_t)bootinfo()->kernel_virt)
#define KERNEL_IMAGE_END   ((uintptr_t)_end)
#else
/* linker.ld থেকে */
extern char __kernel_start[], __kernel_end[];
#define KERNEL_IMAGE_START ((uintptr_t)__kernel_start)
#define KERNEL_IMAGE_END   ((uintptr_t)__kernel_end)
#endif

static void print_banner(void)
{
    tty_set_color(TTY_COLOR_LIGHT_CYAN, TTY_COLOR_BLACK);
#ifdef __TINYC__
    kprintf("rozOS %s (x86_64, kernel built inside rozOS with TCC)\n", ROZOS_VERSION);
#else
    kprintf("rozOS %s (x86_64)\n", ROZOS_VERSION);
#endif
    /* ধাপ ১১খ: বিল্ড নম্বর */
    kprintf("build %s, %s on %s, %s\n", rozos_build_id, rozos_build_compiler, rozos_build_where, rozos_build_date);
    /* build tag (kinstall ফাইলে খোঁজে) ব্যবহার করা, যেন লিংকার (--gc-sections) ফেলে না দেয় */
    if (rozos_build_tag[0] != 'R') {
        kprintf("(build tag missing)\n");
    }
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    kprintf("--------------------------------------------\n");
}

static void print_ok(const char *what)
{
    tty_quiet_dot();                     /* ধাপ ২৪ক: শান্ত বুটে পর্দায় শুধু একটা বিন্দু */
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
            (unsigned long long)(KERNEL_IMAGE_END - KERNEL_IMAGE_START) / 1024);
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

    /* ধাপ ১৯ঠ: মাউসের সীমা সবসময় পর্দার মাপে — আগে শুধু PS/2 মাউস পেলে বসত, ফলে আসল PC-তে
     * (PS/2 মাউস নেই, শুধু USB) USB মাউস 1024x768-এ আটকে থাকত */
    if (fb_is_ready()) {
        mouse_set_bounds((int32_t)fb_width(), (int32_t)fb_height());
    }
    if (ps2_init()) {
        print_ok("PS/2 controller");
        if (keyboard_init()) {
            print_ok("PS/2 keyboard (US QWERTY, IRQ1)");
        } else {
            print_warn("PS/2 keyboard did not respond");
        }
        if (mouse_init()) {
            print_ok(mouse_has_wheel() ? "PS/2 mouse with scroll wheel (IRQ12)" : "PS/2 mouse (IRQ12)");
        } else {
            print_warn("no PS/2 mouse");
        }
    } else {
        print_warn("no PS/2 controller: keyboard via USB (if any) or serial");
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
    char msg[128];
    struct pmm_stats ps;

    /* ধাপ ১৯ঝ: memory map কাটা পড়লে আগেই জানানো (ওপরের RAM আর ডিভাইস বাদ পড়ে) */
    if (bootinfo()->memmap_truncated) {
        ksnprintf(msg, sizeof(msg), "memory map: %llu entries did not fit (max %u) - some RAM is ignored",
                  (unsigned long long)bootinfo()->memmap_truncated, (unsigned)BOOTINFO_MAX_MEMMAP);
        print_warn(msg);
    }
    if (!pmm_init()) {
        kpanic("PMM: no usable memory for the page bitmap");
    }
    pmm_get_stats(&ps);
    ksnprintf(msg, sizeof(msg), "PMM (bitmap): %llu MiB free, bitmap %llu KiB, map %llu entries",
              (unsigned long long)(ps.free_pages * PMM_PAGE_SIZE / (1024 * 1024)),
              (unsigned long long)(ps.bitmap_bytes / 1024), (unsigned long long)bootinfo()->memmap_count);
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

/* ধাপ ৬: শিডিউলার, প্রসেস, syscall। heap আর চূড়ান্ত টাইমার (APIC) লাগে, আর
 * VFS-এর lock শিডিউলারের থ্রেড চেনে — তাই স্টোরেজের আগে। */
static void init_processes(void)
{
    char msg[96];

    random_init();
    if (random_has_rdrand()) {
        print_ok("Random numbers: ChaCha20 CSPRNG seeded from RDRAND/RDSEED + timer jitter");
    } else {
        print_ok("Random numbers: ChaCha20 CSPRNG seeded from timer jitter (no RDRAND on this CPU)");
    }

    sched_init();
    ksnprintf(msg, sizeof(msg), "Scheduler: round-robin, %u ms slices, %u KiB kernel stacks, SSE on",
              (unsigned)SCHED_SLICE_MS, (unsigned)(SCHED_KSTACK_SIZE / 1024));
    print_ok(msg);

    proc_init();
    shm_init();                              /* ধাপ ১৩: shared memory-র vmm hook */
    syscall_init();
    print_ok("Syscalls: syscall/sysret (fast) and int 0x80, Linux x86-64 numbers");
}

/* কার্নেলের command line-এ word আছে কিনা (স্পেস দিয়ে আলাদা শব্দ) */
static int cmdline_has(const char *word)
{
    const char *c = bootinfo()->cmdline;
    size_t n = strlen(word);
    while (*c) {
        while (*c == ' ') c++;
        if (!strncmp(c, word, n) && (c[n] == ' ' || c[n] == '\0')) {
            return 1;
        }
        while (*c && *c != ' ') c++;
    }
    return 0;
}

/* ধাপ ৬-৭: প্রথম ইউজার প্রোগ্রাম। ধাপ ৭ থেকে init shell (rozsh) চালায় এবং কখনো শেষ
 * হয় না, তাই এই থ্রেড শুধু অপেক্ষা করে; init কোনো কারণে শেষ হলে তবেই কনসোলে kmon
 * আসে (shell থেকে kmon চাইলে /bin/kmon)। */
static void start_init(void)
{
    /* ধাপ ১৪: কমান্ড লাইনে "text" থাকলে init ডেস্কটপ নয়, টেক্সট মোড চালায় */
    static const char *const argv_desktop[] = { "/bin/init", NULL };
    static const char *const argv_text[] = { "/bin/init", "text", NULL };
    const char *const *argv = cmdline_has("text") ? argv_text : argv_desktop;
    struct vfs_stat st;
    char msg[96];
    int pid, status, r;

    if (cmdline_has("noinit")) {
        print_warn("'noinit' on the kernel command line: /bin/init not started");
        return;
    }
    if (vfs_stat("/bin/init", &st) < 0) {
        print_warn("no /bin/init (run 'make disk-update' or 'make disk-reset')");
        return;
    }
    pid = proc_spawn("/bin/init", argv, 0);
    if (pid < 0) {
        ksnprintf(msg, sizeof(msg), "could not start /bin/init: %s", kstrerror(pid));
        print_warn(msg);
        return;
    }
    ksnprintf(msg, sizeof(msg), "Started /bin/init as pid %d (ring 3)", pid);
    print_ok(msg);
    kprintf("\n");

    r = proc_wait_kernel(pid, &status, 0);
    if (r < 0) {
        ksnprintf(msg, sizeof(msg), "waiting for /bin/init failed: %s", kstrerror(r));
        print_warn(msg);
        return;
    }
    if (status & 0x7F) {
        ksnprintf(msg, sizeof(msg), "/bin/init was killed by signal %d", status & 0x7F);
        print_warn(msg);
    } else {
        ksnprintf(msg, sizeof(msg), "/bin/init exited with code %d - kmon takes over the console",
                  (status >> 8) & 0xFF);
        print_warn(msg);
    }
}

/* ধাপ ৯: নেটওয়ার্ক — NIC খোঁজা, lwIP, DHCP (ঠিকানা পরে netd থ্রেড জানায়) */
static void init_network(void)
{
    char msg[96];
    int n = net_init();
    struct rozos_netinfo ni;

    if (n <= 0) {
        print_warn("no supported network card (e1000/e1000e): networking off");
        return;
    }
    net_get_info(0, &ni);
    ksnprintf(msg, sizeof(msg), "Network: %s (%s) %02x:%02x:%02x:%02x:%02x:%02x, lwIP, DHCP started",
              ni.name, ni.driver, ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    print_ok(msg);
}

/* ধাপ ৫: ডিস্ক। VFS আর devfs আগে তৈরি হয়, কারণ ডিস্ক ড্রাইভার নিজেকে /dev-এ
 * (sda ...) নিবন্ধন করে। */
/* ধাপ ১৫খ: QEMU/KVM/VirtualBox-এর ভেতরে কিনা — CPUID-এর hypervisor বিট, আর ডিস্কের মডেল */
static int cpu_is_virtual(void)
{
    uint32_t a = 1, b, c, d;
    __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c >> 31) & 1;
}

static int model_is_virtual(const char *m)
{
    return !strncmp(m, "QEMU", 4) || !strncmp(m, "VBOX", 4) || !strncmp(m, "VMware", 6) ||
           !strncmp(m, "Virtual", 7);
}

/* ধাপ ১৫খ: আসল কম্পিউটারে অন্য OS-এর ডিস্ক (যেমন Windows-এর NVMe) শুধু পড়া যাবে —
 * rozOS-এর partition ("rozOS root"/"rozOS boot") নেই এমন অভ্যন্তরীণ ডিস্ক। পেন ড্রাইভ
 * (removable) আর virtual machine-এর ডিস্কে লেখা যায়। বুট-মেনুর কমান্ড লাইনে "diskwrite"
 * দিলে সুরক্ষা বন্ধ। */
static void protect_foreign_disks(void)
{
    char msg[160];
    size_t i, k;
    int vm = cpu_is_virtual();
    if (cmdline_has("diskwrite")) {
        print_warn("diskwrite: every disk is writable (protection off)");
        return;
    }
    for (i = 0; i < block_device_count(); i++) {
        struct block_device *d = block_device_at(i);
        int ours = 0;
        if (d->parent || d->removable || vm || model_is_virtual(d->model)) continue;
        for (k = 0; k < block_device_count(); k++) {
            struct block_device *p = block_device_at(k);
            if (p->parent == d && !strncmp(p->part_label, "rozOS", 5)) ours = 1;
        }
        if (ours) continue;
        d->read_only = 1;
        ksnprintf(msg, sizeof(msg), "/dev/%s is READ-ONLY (not a rozOS disk on a real computer; boot option diskwrite allows writing)",
                  d->name);
        print_warn(msg);
    }
}

/* ---------- ধাপ ২৪খ: ISO লেখা পেন ড্রাইভে স্থায়ী জায়গা (persistence) ----------
 * ISO (DD করে লেখা) থেকে বুট হলে / আসলে RAM-এ (tmpfs + initrd) — বন্ধ করলেই সব হারায়।
 * এখন: বুটের পেন ড্রাইভে ISO-র পরে, ১ GiB থেকে শেষ পর্যন্ত, একটা ext2 ("rozOS-persist")।
 * MBR-এ নতুন partition লাগে না (xorriso-র চারটা ঘরই ভরা), তাই Windows-এ ড্রাইভ যেমন
 * ছিল তেমনই দেখায়; নতুন ISO আবার DD করলেও (ISO < ১ GiB) এই জায়গা অক্ষত থাকে।
 *   - প্রথম বুট: ext2 বানানো → / হিসেবে mount → initrd-এর সব ফাইল সেখানে কপি
 *   - পরের বুট: সরাসরি / = এই ext2 (Desktop-এর ফাইল, ওয়ালপেপার, প্যাকেজ সব থাকে)
 *   - ISO বদলালে (বিল্ড আলাদা): সিস্টেম ফাইল নতুন করে, /home আর নিজের সেটিং রেখে
 * বুটের কমান্ড লাইনে "nopersist" দিলে আগের মতো শুধু RAM। */
#define PERSIST_OFFSET   (1024ull * 1024 * 1024)          /* ১ GiB */
#define PERSIST_MIN      (512ull * 1024 * 1024)           /* অন্তত ৫১২ MiB জায়গা লাগবে */
#define PERSIST_LABEL    "rozOS-persist"
#define PERSIST_MARK     "/etc/.rozos-persist"

static struct block_device persist_part;
static struct block_device *persist_dev;
static int persist_created;

/* ধাপ ২৪গ: পর্দায় অগ্রগতির দাগ — "  Creating persistent storage  [#####-----]  50%"
 * একই লাইনে (\r) বারবার আঁকা; serial-এ প্রতি ১০%-এ একটা লাইন */
static const char *prog_what;
static int prog_last = -1;

static void prog_draw(unsigned pct)
{
    char line[120], bar[31];
    int i, fill;
    if (pct > 100) pct = 100;
    if ((int)pct == prog_last) return;
    if (prog_last < 0 || pct / 10 != (unsigned)prog_last / 10) {
        int n = ksnprintf(line, sizeof(line), "[....] %s: %u%%\n", prog_what, pct);
        serial_write(line, (size_t)n);
    }
    prog_last = (int)pct;
    fill = (int)(pct * 30 / 100);
    for (i = 0; i < 30; i++) bar[i] = i < fill ? '#' : '-';
    bar[30] = '\0';
    ksnprintf(line, sizeof(line), "\r  %-30s [%s] %3u%%", prog_what, bar, pct);
    tty_set_color(TTY_COLOR_LIGHT_CYAN, TTY_COLOR_BLACK);
    tty_screen_puts(line);
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
}

static void prog_begin(const char *what)
{
    prog_what = what;
    prog_last = -1;
    tty_screen_puts("\n");
    prog_draw(0);
}

static void prog_end(void)
{
    prog_draw(100);
    tty_screen_puts("\n");
}

static void prog_mkfs(unsigned done, unsigned total) { prog_draw(total ? done * 100 / total : 100); }
static void prog_copy(uint64_t done, uint64_t total) { prog_draw(total ? (unsigned)(done * 100 / total) : 100); }

/* এই ডিস্কে আমাদের ISO (ISO9660 PVD, volume id "ROZOS") আছে কিনা; থাকলে ISO-র মাপ */
static uint64_t rozos_iso_size(struct block_device *d)
{
    uint8_t pvd[2048];
    if (block_read_bytes(d, 32768, pvd, sizeof(pvd)) < 0) return 0;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) || memcmp(pvd + 40, "ROZOS", 5)) return 0;
    return (uint64_t)(pvd[80] | (pvd[81] << 8) | (pvd[82] << 16) | ((uint32_t)pvd[83] << 24)) * 2048;
}

static int is_boot_disk(struct block_device *d, int loose)
{
    const struct bootinfo *bi = bootinfo();
    uint8_t mbr[512];
    uint32_t id, want = 0;
    uint64_t i;
    for (i = 0; i < bi->module_count; i++) if (bi->modules[i].mbr_disk_id) want = bi->modules[i].mbr_disk_id;
    if (!want || loose) return d->removable;        /* বুটলোডার জানায়নি/মেলেনি: শুধু পেন ড্রাইভ */
    if (block_read(d, 0, 1, mbr) < 0) return 0;
    id = (uint32_t)mbr[440] | ((uint32_t)mbr[441] << 8) | ((uint32_t)mbr[442] << 16) | ((uint32_t)mbr[443] << 24);
    return id == want;
}

/* ধাপ ২৪গ সুরক্ষা: শুধু পেন ড্রাইভ (removable), যার MBR/GPT-এর কোনো partition ১ GiB-এর
 * পরে নেই — অর্থাৎ ১ GiB-এর পরের জায়গা কারো নয়। অভ্যন্তরীণ ডিস্কে (HDD/SSD/NVMe) কখনো নয়। */
static int persist_area_free(struct block_device *d, uint64_t start_lba)
{
    size_t k;
    if (!d->removable) return 0;
    for (k = 0; k < block_device_count(); k++) {
        struct block_device *p = block_device_at(k);
        if (p->parent == d && p != &persist_part && p->start_lba + p->sector_count > start_lba) return 0;
    }
    return 1;
}

static void persist_setup(void)
{
    char msg[200];
    size_t i, n = block_device_count();
    int loose;
    if (cmdline_has("nopersist")) return;
    /* প্রথমে MBR signature মিলিয়ে; কোনোটা না মিললে ISO লেখা পেন ড্রাইভ */
    for (loose = 0; loose < 2; loose++)
    for (i = 0; i < n; i++) {
        struct block_device *d = block_device_at(i);
        uint64_t iso, total, start;
        int err;
        if (d->parent || d->read_only || d->offline || !d->removable) continue;   /* ২৪গ: শুধু পেন ড্রাইভ */
        iso = rozos_iso_size(d);
        if (!iso || !is_boot_disk(d, loose)) continue;
        total = d->sector_count * d->sector_size;
        if (iso > PERSIST_OFFSET || total < PERSIST_OFFSET + PERSIST_MIN) {
            ksnprintf(msg, sizeof(msg), "/dev/%s: too small for persistent storage (needs 1.5 GiB) - changes are lost at power off",
                      d->name);
            print_warn(msg);
            return;
        }
        start = PERSIST_OFFSET / d->sector_size;
        if (!persist_area_free(d, start)) {
            ksnprintf(msg, sizeof(msg), "/dev/%s: has partitions after 1 GiB - not used for persistent storage", d->name);
            print_warn(msg);
            return;
        }
        memset(&persist_part, 0, sizeof(persist_part));
        persist_part.parent = d;
        persist_part.start_lba = start;
        persist_part.sector_count = d->sector_count - start;
        persist_part.part_number = 9;
        persist_part.removable = d->removable;
        strcpy(persist_part.part_type, "rozOS persistence");
        strcpy(persist_part.part_label, "rozOS persist");
        if (block_register_partition(&persist_part) < 0) return;
        /* বুট অপশন "persistreset": সব মুছে নতুন করে (কখনো নিজে থেকে মোছে না) */
        if (!ext2_has_label(&persist_part, PERSIST_LABEL) || cmdline_has("persistreset")) {
            ksnprintf(msg, sizeof(msg), "first boot from this pen drive: creating persistent storage on /dev/%s (%llu MiB), please wait...",
                      persist_part.name,
                      (unsigned long long)(persist_part.sector_count * persist_part.sector_size / (1024 * 1024)));
            print_warn(msg);
            prog_begin("Creating persistent storage");
            err = ext2_mkfs(&persist_part, PERSIST_LABEL, prog_mkfs);
            prog_end();
            if (err < 0) {
                ksnprintf(msg, sizeof(msg), "could not create persistent storage: %s", kstrerror(err));
                print_warn(msg);
                return;
            }
            persist_created = 1;
        }
        persist_dev = &persist_part;
        ksnprintf(msg, sizeof(msg), "persistent storage on /dev/%s (after the %llu MiB ISO)", persist_part.name,
                  (unsigned long long)(iso / (1024 * 1024)));
        print_ok(msg);
        return;
    }
}

/* ISO বদলালে নতুন ফাইল বসানোর সময় এগুলো আগেরটাই থাকে */
static int persist_keep(const char *p)
{
    static const char *const keep[] = {
        "/etc/rozos.conf", "/etc/rozpkg.conf", "/etc/hosts", "/etc/resolv.conf", "/etc/passwd",
        "/etc/group", "/etc/console-font", NULL
    };
    int i;
    if (!strncmp(p, "/home/", 6)) return 1;
    for (i = 0; keep[i]; i++) if (!strcmp(p, keep[i])) return 1;
    return 0;
}

/* স্থায়ী root mount হওয়ার পরে: প্রথমবার বা ISO বদলালে initrd থেকে ফাইল */
static void persist_fill(void)
{
    char want[96], have[96] = "", msg[160];
    struct initrd_stats st;
    int fd, err;
    int64_t r;
    ksnprintf(want, sizeof(want), "%s %s\n", ROZOS_VERSION, rozos_build_id);
    fd = vfs_open(PERSIST_MARK, O_RDONLY);
    if (fd >= 0) {
        r = vfs_read(fd, have, sizeof(have) - 1);
        have[r > 0 ? r : 0] = '\0';
        vfs_close(fd);
    }
    if (!strcmp(want, have)) return;
    if (!persist_created) print_warn("this ISO is a different build: updating the system files (your files and settings stay)");
    prog_begin(persist_created ? "Copying the system files" : "Updating the system files");
    initrd_set_progress(prog_copy);
    block_batch_begin(persist_dev);                  /* ধাপ ২৪ঘ: ছোট লেখাগুলো জমিয়ে, ক্রম অনুযায়ী বড় করে */
    err = initrd_load_keep("/", &st, persist_created ? NULL : persist_keep);
    if (block_batch_end(persist_dev) < 0 && err >= 0) err = -EIO;
    initrd_set_progress(NULL);
    prog_end();
    if (err < 0 || st.errors) {
        ksnprintf(msg, sizeof(msg), "persistent storage: copying the system files: %llu errors",
                  (unsigned long long)st.errors);
        print_warn(msg);
    } else {
        ksnprintf(msg, sizeof(msg), "persistent storage: %llu system files copied from the ISO",
                  (unsigned long long)st.files);
        print_ok(msg);
    }
    fd = vfs_open(PERSIST_MARK, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        vfs_write(fd, want, strlen(want));
        vfs_close(fd);
    }
    block_sync(NULL);
}

static void init_storage(void)
{
    char msg[128];
    struct ahci_info ai;
    size_t i;
    int n;

    vfs_init();
    devfs_init();

    n = pci_init();
    ksnprintf(msg, sizeof(msg), "PCI: %d device(s)", n);
    print_ok(msg);

    n = ahci_init();
    ahci_get_info(&ai);
    if (!ai.present) {
        print_warn("no AHCI (SATA) controller");
    } else {
        ksnprintf(msg, sizeof(msg), "AHCI %u.%u: %d disk(s), %u command slots, %s DMA",
                  (unsigned)(ai.version >> 16), (unsigned)((ai.version >> 8) & 0xFF), n,
                  (unsigned)ai.command_slots, ai.supports_64bit ? "64-bit" : "32-bit");
        print_ok(msg);
    }
    /* ধাপ ১৫ক: NVMe SSD */
    {
        struct nvme_info ni;
        n = nvme_init();
        nvme_get_info(&ni);
        if (ni.controllers) {
            ksnprintf(msg, sizeof(msg), "NVMe %u.%u: %d disk(s) on %d controller(s), \"%s\"",
                      (unsigned)(ni.version >> 16), (unsigned)((ni.version >> 8) & 0xFF), n, ni.controllers, ni.model);
            print_ok(msg);
        }
    }
    /* ধাপ ১৫খ: USB (xHCI) — পেন ড্রাইভ এখানেই চেনা হলে নিচের তালিকায় partition-সহ আসে */
    n = usb_init();
    if (n > 0) {
        ksnprintf(msg, sizeof(msg), "USB: %d xHCI controller(s); keyboard, mouse and pen drives (lsusb)", n);
        print_ok(msg);
    }
    {
        size_t disks = block_device_count();          /* partition যোগ হওয়ার আগের সংখ্যা */
        for (i = 0; i < disks; i++) {
            struct block_device *d = block_device_at(i);
            size_t first = block_device_count(), k;
            int parts;
            ksnprintf(msg, sizeof(msg), "/dev/%s: %llu MiB, %u-byte sectors, \"%s\"",
                      d->name, (unsigned long long)(d->sector_count * d->sector_size / (1024 * 1024)),
                      (unsigned)d->sector_size, d->model);
            print_ok(msg);
            /* ধাপ ১১খ: GPT/MBR partition */
            parts = partition_scan(d);
            for (k = first; parts > 0 && k < block_device_count(); k++) {
                struct block_device *p = block_device_at(k);
                ksnprintf(msg, sizeof(msg), "  /dev/%s: %s partition, %llu MiB%s%s%s", p->name, p->part_type,
                          (unsigned long long)(p->sector_count * p->sector_size / (1024 * 1024)),
                          p->part_label[0] ? " \"" : "", p->part_label, p->part_label[0] ? "\"" : "");
                print_ok(msg);
            }
        }
    }
    protect_foreign_disks();                         /* ধাপ ১৫খ */
    persist_setup();                                 /* ধাপ ২৪খ */
}

/* ext2 পাওয়া প্রথম ডিস্ক "/"-এ; সফল হলে 1
 * (ধাপ ১১খ: partition থাকা ডিস্কে পুরো ডিস্ক নয়, partition-গুলোই দেখা হয়) */
static int mount_disk_root(void)
{
    char msg[128];
    size_t i;
    int pass;
    /* ধাপ ২৪খ: বুটের পেন ড্রাইভের স্থায়ী জায়গা সবার আগে */
    if (persist_dev && vfs_mount("/", "ext2", persist_dev->name) == 0) {
        struct vfs_statfs sf;
        vfs_statfs("/", &sf);
        ksnprintf(msg, sizeof(msg), "persistent storage /dev/%s mounted on / (%llu MiB free)", persist_dev->name,
                  (unsigned long long)(sf.free_blocks * sf.block_size / (1024 * 1024)));
        print_ok(msg);
        return 2;
    }
    /* ধাপ ১৫খ: আগে "rozOS root" নামের partition (আসল কম্পিউটারে অন্য Linux-এর ext2 নয়),
     * না পেলে আগের মতো প্রথম ext2 */
    for (pass = 0; pass < 2; pass++)
    for (i = 0; i < block_device_count(); i++) {
        struct block_device *d = block_device_at(i);
        struct vfs_statfs sf;
        if (!d->parent && d->partitions) {
            continue;
        }
        if (pass == 0 && strcmp(d->part_label, "rozOS root")) {
            continue;
        }
        if (vfs_mount("/", "ext2", d->name) == 0) {
            vfs_statfs("/", &sf);
            ksnprintf(msg, sizeof(msg), "ext2 on /dev/%s mounted on / (%llu MiB, %llu MiB free%s)",
                      d->name,
                      (unsigned long long)(sf.total_blocks * sf.block_size / (1024 * 1024)),
                      (unsigned long long)(sf.free_blocks * sf.block_size / (1024 * 1024)),
                      sf.read_only ? ", read-only" : "");
            print_ok(msg);
            return 1;
        }
    }
    return 0;
}

/* ধাপ ১১খ: boot partition (FAT, Limine আর kernel-গুলো) → /boot। প্রথমে "EFI System"
 * partition, না থাকলে FAT দেখায় এমন প্রথম partition। */
static void mount_boot_partition(void)
{
    char msg[128];
    size_t i;
    int pass;
    /* ধাপ ১৫খ: pass 0 = "rozOS boot" নামের partition (আসল কম্পিউটারে Windows-এর EFI
     * partition নয়), 1 = যেকোনো "EFI System", 2 = প্রথম FAT */
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < block_device_count(); i++) {
            struct block_device *d = block_device_at(i);
            struct vfs_statfs sf;
            if (!d->parent || (pass == 0 && strcmp(d->part_label, "rozOS boot")) ||
                (pass == 1 && strcmp(d->part_type, "EFI System"))) {
                continue;
            }
            vfs_mkdir_p("/boot");
            if (vfs_mount("/boot", "fat", d->name) == 0) {
                vfs_statfs("/boot", &sf);
                ksnprintf(msg, sizeof(msg), "FAT on /dev/%s mounted on /boot (%llu MiB, %llu MiB free)", d->name,
                          (unsigned long long)(sf.total_blocks * sf.block_size / (1024 * 1024)),
                          (unsigned long long)(sf.free_blocks * sf.block_size / (1024 * 1024)));
                print_ok(msg);
                return;
            }
        }
    }
}

/* ধাপ ২৪গ: ভেতরের সব ডিস্কের (SATA HDD/SSD, NVMe) চেনা partition → /mnt/<নাম>
 * (যেমন /mnt/nvmea3)। অন্য OS-এর ডিস্ক protect_foreign_disks-এর কারণে শুধু পড়া;
 * NTFS সবসময় শুধু পড়া। পেন ড্রাইভ/USB ডিস্ক usbd নিজে /mnt/usb-তে বসায়। */
static int mounted_source(const char *name)
{
    size_t k;
    for (k = 0; k < vfs_mount_count(); k++) {
        const struct mount *m = vfs_mount_at(k);
        if (m && m->active && !strcmp(m->source, name)) return 1;
    }
    return 0;
}

static void automount_disks(void)
{
    char msg[160], mp[48];
    size_t i;
    if (cmdline_has("noautomount")) return;
    for (i = 0; i < block_device_count(); i++) {
        struct block_device *b = block_device_at(i);
        struct block_device *top = b->parent ? b->parent : b;
        const char *fs;
        if (top->removable || (!b->parent && b->partitions) || b == &persist_part) continue;
        if (mounted_source(b->name)) continue;
        fs = fs_driver_for(b);
        if (!fs) continue;
        ksnprintf(mp, sizeof(mp), "/mnt/%s", b->name);
        vfs_mkdir_p(mp);
        if (vfs_mount(mp, fs, b->name) == 0) {
            ksnprintf(msg, sizeof(msg), "%s on /dev/%s mounted on %s%s", fs, b->name, mp,
                      (block_is_read_only(b) || !strcmp(fs, "ntfs")) ? " (read-only)" : "");
            print_ok(msg);
        }
    }
}

/* ধাপ ৪-৫: ফাইলসিস্টেম। initrd মডিউলের ডেটা executable_and_modules অঞ্চলে থাকে
 * (reclaim হয় না), আর এর তথ্য bootinfo-তে কপি করা — তাই এটা reclaim-এর আগে বা পরে
 * যেকোনো সময় চলতে পারে। */
static void init_vfs(void)
{
    char msg[128];
    struct initrd_stats st;
    int err, disk_root;

    vfs_register_filesystem(&tmpfs_filesystem);
    vfs_register_filesystem(&ext2_filesystem);
    vfs_register_filesystem(&fat_filesystem);        /* ধাপ ১১খ */
    vfs_register_filesystem(&ntfs_filesystem);       /* ধাপ ২৪গ: শুধু পড়া */
    vfs_register_filesystem(&exfat_filesystem);      /* ধাপ ২৪গ */

    disk_root = mount_disk_root();
    if (disk_root) {
        if (disk_root == 2) {
            persist_fill();                          /* ধাপ ২৪খ */
        } else if (bootinfo()->module_count) {
            print_ok("initrd present but not used: root is on disk");
        }
        vfs_mkdir_p("/tmp");
        vfs_mkdir_p("/dev");
        err = vfs_mount("/tmp", "tmpfs", NULL);
        if (err < 0) {
            ksnprintf(msg, sizeof(msg), "could not mount tmpfs on /tmp: %s", kstrerror(err));
            print_warn(msg);
        } else {
            print_ok("tmpfs mounted on /tmp");
        }
        if (disk_root != 2) mount_boot_partition();  /* ধাপ ২৪খ: ISO-তে /boot নেই */
    } else {
        err = vfs_mount("/", "tmpfs", NULL);
        if (err < 0) {
            kpanic("could not mount tmpfs on /: %s", kstrerror(err));
        }
        print_ok("VFS: no ext2 disk - tmpfs mounted on /");
        err = initrd_load("/", &st);
        disk_root = -1;
    }

    if (disk_root < 0) {
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
    }

    /* /dev, /tmp নিশ্চিত (initrd-এ না থাকলেও) */
    vfs_mkdir_p("/dev");
    vfs_mkdir_p("/tmp");
    err = vfs_mount("/dev", "devfs", NULL);
    if (err < 0) {
        ksnprintf(msg, sizeof(msg), "could not mount devfs on /dev: %s", kstrerror(err));
        print_warn(msg);
    } else {
        print_ok("devfs mounted on /dev");
        fbdev_init();                        /* ধাপ ১৩: /dev/fb0 */
    }
    automount_disks();                       /* ধাপ ২৪গ */
    usb_storage_ready();                     /* ধাপ ১৫খ: পেন ড্রাইভ এখন /mnt/usb-তে */
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

    /* ধাপ ২৪ক: শান্ত বুট — পর্দায় এক লাইন, বাকি সব serial আর bootlog-এ।
     * command line এখনো পড়া হয়নি (bootinfo পরে কপি হয়), "bootdebug" দেখলে নিচে
     * জমা লেখা আবার দেখিয়ে স্বাভাবিক হয়ে যায়। */
    tty_set_color(TTY_COLOR_LIGHT_CYAN, TTY_COLOR_BLACK);
    tty_screen_puts("Starting rozOS " ROZOS_VERSION " ");
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    tty_set_quiet(1);

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

    if (cmdline_has("bootdebug")) {
        tty_set_quiet(0);
        tty_screen_puts("\n");
        tty_bootlog_replay();
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
    init_processes();
    init_storage();
    init_vfs();
    init_network();

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

    if (tty_quiet()) {
        /* ধাপ ২৪ক: শান্ত বুট শেষ — বিন্দুর লাইনের পরে " ok" */
        tty_set_color(TTY_COLOR_LIGHT_GREEN, TTY_COLOR_BLACK);
        tty_screen_puts(" ok\n");
        tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
        tty_set_quiet(0);
    }
    tty_set_color(TTY_COLOR_LIGHT_GREEN, TTY_COLOR_BLACK);
    kprintf("\nrozOS booted.\n");   /* ধাপ ২৪ক: ছোট (আগে ধাপ ১৫খ-এর বৈশিষ্ট্যের তালিকা ছিল) */
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

    tty_bootlog_close();                 /* ধাপ ২৪ক: bootlog শুধু বুটের লেখা রাখে */
    start_init();
    kmon_run();
}
