/* rozOS — kmon: কার্নেল মনিটর
 *
 * কমান্ড: help, clear, uptime, irq, mem, mouse, keys, sleep <ms>, echo <text>,
 *         int3, fault, reboot, halt
 * ধাপ ৩: pmm, heap, heaptest, v2p <hex>, acpi, apic
 * ধাপ ৪: pwd, cd, ls, cat, mkdir, touch, write, append, rm, rmdir, stat, tree, mount */

#include <stdint.h>
#include <stddef.h>
#include <rozos/kmon.h>
#include <rozos/console.h>
#include <rozos/printf.h>
#include <rozos/string.h>
#include <rozos/serial.h>
#include <rozos/keyboard.h>
#include <rozos/mouse.h>
#include <rozos/timer.h>
#include <rozos/ps2.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/acpi.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <arch/x86_64/pic.h>
#include <arch/x86_64/pit.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>

#define KMON_LINE_MAX 128
#define KMON_MAX_ARGS 8

/* ---------- ইনপুট ---------- */

/* শেষ কী কোথা থেকে এসেছে — serial-এ কোনো scancode থাকে না */
static int last_key_from_serial;

int kmon_getkey(void)
{
    int key;
    char c;

    for (;;) {
        cpu_cli();
        if (keyboard_try_getkey(&key)) {
            cpu_sti();
            last_key_from_serial = 0;
            return key;
        }
        /* serial-এ এখনো ইন্টারাপ্ট নেই — প্রতি টাইমার টিকে (1 ms) একবার দেখা হয় */
        if (serial_try_getc(&c)) {
            cpu_sti();
            last_key_from_serial = 1;
            if (c == '\r') return '\n';
            if (c == 0x7F) return '\b';           /* টার্মিনালের Backspace */
            return (unsigned char)c;
        }
        cpu_sti_hlt();
    }
}

int kmon_readline(char *buf, size_t size)
{
    size_t len = 0;

    if (size == 0) {
        return 0;
    }
    for (;;) {
        int key = kmon_getkey();

        if (key == '\n') {
            tty_putc('\n');
            buf[len] = '\0';
            return (int)len;
        }
        if (key == '\b') {
            if (len > 0) {
                len--;
                tty_write("\b \b", 3);
            }
            continue;
        }
        if (key == 3) {                           /* Ctrl+C */
            tty_write("^C\n", 3);
            buf[0] = '\0';
            return -1;
        }
        if (key == 12) {                          /* Ctrl+L */
            tty_clear();
            buf[len] = '\0';
            return -2;
        }
        if (key >= 0x20 && key < 0x7F && len + 1 < size) {
            buf[len++] = (char)key;
            tty_putc((char)key);
        }
        /* বাকি বিশেষ কী (তীর ইত্যাদি) এখানে উপেক্ষিত — এডিটর ধাপে ব্যবহার হবে */
    }
}

/* ---------- সহায়ক ---------- */

static int split_args(char *line, char **argv, int max)
{
    int argc = 0;
    while (*line && argc < max) {
        while (*line == ' ' || *line == '\t') {
            *line++ = '\0';
        }
        if (!*line) {
            break;
        }
        argv[argc++] = line;
        while (*line && *line != ' ' && *line != '\t') {
            line++;
        }
    }
    return argc;
}

static int parse_uint(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!s || !*s) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return 1;
}

static int parse_hex(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        char c = *s;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

static void print_uptime(void)
{
    uint64_t ms = timer_uptime_ms();
    kprintf("uptime: %llu.%03llu s (%llu ticks at %u Hz, source %s)\n",
            (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000),
            (unsigned long long)timer_ticks(), (unsigned)timer_frequency(),
            timer_source_name());
}

static void cmd_pmm(void)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    kprintf("PMM (bitmap, 4 KiB pages)\n");
    kprintf("  free      : %llu pages (%llu MiB)\n", (unsigned long long)st.free_pages,
            (unsigned long long)(st.free_pages * PMM_PAGE_SIZE / (1024 * 1024)));
    kprintf("  used      : %llu pages\n", (unsigned long long)(st.usable_pages - st.free_pages));
    kprintf("  usable    : %llu pages, bitmap covers %llu\n",
            (unsigned long long)st.usable_pages, (unsigned long long)st.total_pages);
    kprintf("  bitmap    : %llu KiB\n", (unsigned long long)(st.bitmap_bytes / 1024));
    kprintf("  reclaimed : %llu KiB of bootloader memory\n",
            (unsigned long long)(st.reclaimed_bytes / 1024));
    kprintf("  page tables (kernel): %llu pages, NX %s\n",
            (unsigned long long)vmm_table_pages(), vmm_nx_enabled() ? "on" : "off");
    if (st.double_frees) {
        kprintf("  double frees detected: %llu\n", (unsigned long long)st.double_frees);
    }
}

static void cmd_heap(void)
{
    struct heap_stats st;
    int i;
    heap_get_stats(&st);
    kprintf("kernel heap\n  %-6s %-6s %s\n", "class", "pages", "in use");
    for (i = 0; i < HEAP_CLASS_COUNT; i++) {
        kprintf("  %-6llu %-6llu %llu\n", (unsigned long long)st.class_size[i],
                (unsigned long long)st.class_pages[i], (unsigned long long)st.class_in_use[i]);
    }
    kprintf("  large: %llu allocations, %llu pages\n",
            (unsigned long long)st.large_allocs, (unsigned long long)st.large_pages);
    kprintf("  total kmalloc %llu, kfree %llu, bad frees %llu\n",
            (unsigned long long)st.total_allocs, (unsigned long long)st.total_frees,
            (unsigned long long)st.bad_frees);
}

static void cmd_v2p(int argc, char **argv)
{
    uint64_t v, p;
    if (argc < 2 || !parse_hex(argv[1], &v)) {
        kprintf("usage: v2p <hex virtual address>\n");
        return;
    }
    p = vmm_virt_to_phys(vmm_current_space(), v);
    if (p || v == 0xFFFF800000000000ULL) {
        kprintf("0x%016llx -> 0x%016llx\n", (unsigned long long)v, (unsigned long long)p);
    } else {
        kprintf("0x%016llx is not mapped\n", (unsigned long long)v);
    }
}

/* ---------- ফাইল কমান্ড (ধাপ ৪) ---------- */

static char cwd[VFS_PATH_MAX] = "/";

/* "a/../b/./c" ধরনের পাথ পরম ও পরিষ্কার করা (শুধু লেখার স্তরে, ফাইলসিস্টেম না দেখে) */
static int make_path(const char *arg, char *out, size_t size)
{
    char tmp[VFS_PATH_MAX];
    size_t n = 0, i = 0, len;

    if (!arg || !*arg) {
        arg = ".";
    }
    if (arg[0] == '/') {
        len = strnlen(arg, sizeof(tmp));
        if (len >= sizeof(tmp)) return -ENAMETOOLONG;
        memcpy(tmp, arg, len + 1);
    } else {
        size_t cl = strlen(cwd), al = strlen(arg);
        if (cl + 1 + al + 1 > sizeof(tmp)) return -ENAMETOOLONG;
        memcpy(tmp, cwd, cl);
        tmp[cl] = '/';
        memcpy(tmp + cl + 1, arg, al + 1);
    }

    out[n++] = '/';
    while (tmp[i]) {
        size_t start, clen;
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        start = i;
        while (tmp[i] && tmp[i] != '/') i++;
        clen = i - start;
        if (clen == 1 && tmp[start] == '.') {
            continue;
        }
        if (clen == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (n > 1) {
                n--;                                   /* শেষের '/' */
                while (n > 1 && out[n - 1] != '/') n--;
            }
            continue;
        }
        if (n + clen + 2 > size) return -ENAMETOOLONG;
        if (n > 1) out[n++] = '/';
        memcpy(out + n, tmp + start, clen);
        n += clen;
    }
    if (n > 1 && out[n - 1] == '/') n--;
    out[n] = '\0';
    return 0;
}

static int path_arg(int argc, char **argv, int i, char *out)
{
    int err = make_path(i < argc ? argv[i] : NULL, out, VFS_PATH_MAX);
    if (err < 0) {
        kprintf("%s\n", kstrerror(err));
    }
    return err;
}

static void report(const char *what, const char *path, int err)
{
    kprintf("%s: %s: %s\n", what, path, kstrerror(err));
}

static const char *type_char(enum vnode_type t)
{
    return t == VNODE_DIR ? "d" : t == VNODE_CHARDEV ? "c" : "-";
}

static void cmd_ls(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    struct vfs_dirent de;
    struct vfs_stat st;
    int fd, r, count = 0;

    if (path_arg(argc, argv, 1, path) < 0) return;
    if ((r = vfs_stat(path, &st)) < 0) { report("ls", path, r); return; }
    if (st.type != VNODE_DIR) {
        kprintf("%s %8llu  %s\n", type_char(st.type), (unsigned long long)st.size, path);
        return;
    }
    fd = vfs_open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { report("ls", path, fd); return; }
    while ((r = vfs_readdir(fd, &de)) > 0) {
        if (de.type == VNODE_DIR) {
            tty_set_color(TTY_COLOR_LIGHT_BLUE, TTY_COLOR_BLACK);
        } else if (de.type == VNODE_CHARDEV) {
            tty_set_color(TTY_COLOR_YELLOW, TTY_COLOR_BLACK);
        }
        kprintf("%s %8llu  %s%s\n", type_char(de.type), (unsigned long long)de.size,
                de.name, de.type == VNODE_DIR ? "/" : "");
        tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
        count++;
    }
    if (r < 0) report("ls", path, r);
    vfs_close(fd);
    if (count == 0) kprintf("(empty)\n");
}

static void cmd_cat(int argc, char **argv)
{
    char path[VFS_PATH_MAX], buf[256];
    int fd, i;
    int64_t n;

    if (argc < 2) { kprintf("usage: cat <file>...\n"); return; }
    for (i = 1; i < argc; i++) {
        if (path_arg(argc, argv, i, path) < 0) return;
        if (!strncmp(path, "/dev/", 5) && (!strcmp(path + 5, "console") ||
            !strcmp(path + 5, "kbd") || !strcmp(path + 5, "serial0") || !strcmp(path + 5, "zero"))) {
            kprintf("cat: %s: endless device - not reading it here\n", path);
            continue;
        }
        fd = vfs_open(path, O_RDONLY);
        if (fd < 0) { report("cat", path, fd); continue; }
        while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
            tty_write(buf, (size_t)n);
        }
        if (n < 0) report("cat", path, (int)n);
        vfs_close(fd);
    }
}

static void cmd_write(int argc, char **argv, int append)
{
    char path[VFS_PATH_MAX];
    int fd, i;
    int64_t r = 0;

    if (argc < 2) { kprintf("usage: %s <file> <text...>\n", append ? "append" : "write"); return; }
    if (path_arg(argc, argv, 1, path) < 0) return;
    fd = vfs_open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
    if (fd < 0) { report(append ? "append" : "write", path, fd); return; }
    for (i = 2; i < argc && r >= 0; i++) {
        r = vfs_write(fd, argv[i], strlen(argv[i]));
        if (r >= 0 && i + 1 < argc) r = vfs_write(fd, " ", 1);
    }
    if (r >= 0) r = vfs_write(fd, "\n", 1);
    if (r < 0) report("write", path, (int)r);
    vfs_close(fd);
}

static void cmd_stat(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    int r;
    if (path_arg(argc, argv, 1, path) < 0) return;
    if ((r = vfs_stat(path, &st)) < 0) { report("stat", path, r); return; }
    kprintf("  path : %s\n  type : %s\n  size : %llu\n  inode: %llu (%s)\n"
            "  mode : %o\n  links: %u\n  mtime: %llu ms after boot\n",
            path, st.type == VNODE_DIR ? "directory" : st.type == VNODE_CHARDEV ? "character device" : "file",
            (unsigned long long)st.size, (unsigned long long)st.inode, st.fs_name,
            (unsigned)st.mode, (unsigned)st.nlink, (unsigned long long)st.mtime);
}

static void tree_walk(const char *path, int depth, int *count)
{
    struct vfs_dirent de;
    int fd = vfs_open(path, O_RDONLY | O_DIRECTORY), r, i;
    if (fd < 0 || depth > 16) {
        if (fd >= 0) vfs_close(fd);
        return;
    }
    while ((r = vfs_readdir(fd, &de)) > 0) {
        char child[VFS_PATH_MAX];
        for (i = 0; i < depth; i++) kprintf("  ");
        kprintf("%s%s", de.name, de.type == VNODE_DIR ? "/" : "");
        if (de.type == VNODE_FILE) kprintf("  (%llu)", (unsigned long long)de.size);
        kprintf("\n");
        (*count)++;
        if (de.type == VNODE_DIR &&
            ksnprintf(child, sizeof(child), "%s%s%s", path, strcmp(path, "/") ? "/" : "", de.name) < (int)sizeof(child)) {
            tree_walk(child, depth + 1, count);
        }
    }
    vfs_close(fd);
}

static void cmd_tree(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    int count = 0;
    if (path_arg(argc, argv, 1, path) < 0) return;
    kprintf("%s\n", path);
    tree_walk(path, 1, &count);
    kprintf("%d entries\n", count);
}

static void cmd_mount(void)
{
    size_t i;
    for (i = 0; i < vfs_mount_count(); i++) {
        const struct mount *m = vfs_mount_at(i);
        kprintf("  %-8s on %s\n", m->fs->name, m->path);
    }
    kprintf("  open files: %d/%d\n", vfs_open_count(), VFS_MAX_FDS);
}

static void cmd_cd(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    int r;
    if (argc < 2) {
        strcpy(path, "/");                         /* শুধু "cd" → root */
    } else if (path_arg(argc, argv, 1, path) < 0) {
        return;
    }
    if ((r = vfs_stat(path, &st)) < 0) { report("cd", path, r); return; }
    if (st.type != VNODE_DIR) { report("cd", path, -ENOTDIR); return; }
    strcpy(cwd, path);
}

static void cmd_acpi(void)
{
    size_t i;
    if (!acpi_available()) {
        kprintf("ACPI not available\n");
        return;
    }
    kprintf("ACPI revision %u, OEM \"%s\", %u tables\n",
            (unsigned)acpi_revision(), acpi_oem_id(), (unsigned)acpi_table_count());
    for (i = 0; i < acpi_table_count(); i++) {
        const struct acpi_sdt_header *t = acpi_table(i);
        kprintf("  %.4s  rev %u  %6u bytes  OEM %.6s\n", t->signature,
                (unsigned)t->revision, (unsigned)t->length, t->oem_id);
    }
}

static void cmd_apic(void)
{
    const struct apic_info *ai = apic_get_info();
    unsigned i;
    if (!apic_active()) {
        kprintf("APIC not active - using 8259 PIC\n");
        return;
    }
    kprintf("Local APIC: phys 0x%llx, id %u, version 0x%x, %s\n",
            (unsigned long long)ai->lapic_phys, (unsigned)ai->bsp_lapic_id,
            (unsigned)ai->lapic_version, ai->x2apic ? "x2APIC" : "xAPIC");
    kprintf("CPUs in MADT: %u (LAPIC ids:", (unsigned)ai->cpu_count);
    for (i = 0; i < ai->cpu_count; i++) {
        kprintf(" %u", (unsigned)ai->cpu_lapic_ids[i]);
    }
    kprintf(")\n");
    for (i = 0; i < ai->ioapic_count; i++) {
        kprintf("I/O APIC %u: phys 0x%llx, GSI %u-%u\n", (unsigned)ai->ioapics[i].id,
                (unsigned long long)ai->ioapics[i].phys, (unsigned)ai->ioapics[i].gsi_base,
                (unsigned)(ai->ioapics[i].gsi_base + ai->ioapics[i].redirections - 1));
    }
    kprintf("ISA overrides:");
    for (i = 0; i < 16; i++) {
        if (ai->isa_gsi[i] != i || ai->isa_flags[i]) {
            kprintf(" IRQ%u->GSI%u", i, (unsigned)ai->isa_gsi[i]);
        }
    }
    kprintf("\nLAPIC timer: %s, %u ticks/ms (div 16); errors %llu, spurious %llu\n",
            apic_timer_active() ? "active" : "off", (unsigned)ai->lapic_timer_ticks_per_ms,
            (unsigned long long)apic_error_count(), (unsigned long long)apic_spurious_count());
}

/* ---------- কমান্ড ---------- */

static void cmd_help(void)
{
    kprintf("kmon commands:\n"
            "  help           this list\n"
            "  clear          clear the screen (or Ctrl+L)\n"
            "  uptime         time since boot\n"
            "  irq            interrupt counters\n"
            "  mem            memory map\n"
            "  mouse          mouse position and buttons\n"
            "  keys           show key codes (Esc to stop)\n"
            "  sleep <ms>     sleep using the timer\n"
            "  pmm            physical memory statistics\n"
            "  heap           kernel heap statistics\n"
            "  heaptest       run the heap self-test\n"
            "  v2p <hex>      translate a virtual address\n"
            "  acpi           list ACPI tables\n"
            "  apic           local APIC / I/O APIC information\n"
            "  pwd, cd <dir>  current directory\n"
            "  ls [path]      list a directory\n"
            "  cat <file>     print files\n"
            "  mkdir <dir>    make a directory (-p: with parents)\n"
            "  touch <file>   create an empty file\n"
            "  write <f> <t>  write text to a file (append <f> <t> adds)\n"
            "  rm <file>, rmdir <dir>, stat <path>, tree [path], mount\n"
            "  echo <text>    print text\n"
            "  int3           breakpoint exception test (returns)\n"
            "  fault          invalid opcode test (panics)\n"
            "  reboot         reset the machine\n"
            "  halt           stop the CPU\n");
}

static void cmd_irq(void)
{
    static const char *const names[PIC_IRQ_COUNT] = {
        "timer", "keyboard", "cascade", "COM2", "COM1", "LPT2", "floppy", "LPT1",
        "RTC", "free", "free", "free", "mouse", "FPU", "ATA0", "ATA1"
    };
    unsigned i;
    for (i = 0; i < PIC_IRQ_COUNT; i++) {
        uint64_t n = irq_count((uint8_t)i);
        if (n) {
            kprintf("  IRQ%-2u %-9s %llu\n", i, names[i], (unsigned long long)n);
        }
    }
    kprintf("  spurious        %llu\n", (unsigned long long)irq_spurious_count());
    kprintf("  controller      %s\n", pic_apic_mode() ? "I/O APIC + LAPIC" : "8259 PIC");
}

static void cmd_mouse(void)
{
    struct mouse_state st;
    if (!mouse_present()) {
        kprintf("no PS/2 mouse\n");
        return;
    }
    mouse_get_state(&st);
    kprintf("mouse: x=%d y=%d buttons=[%c%c%c] packets=%llu\n",
            (int)st.x, (int)st.y,
            (st.buttons & MOUSE_BUTTON_LEFT) ? 'L' : '-',
            (st.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-',
            (st.buttons & MOUSE_BUTTON_RIGHT) ? 'R' : '-',
            (unsigned long long)st.packets);
}

static void cmd_keys(void)
{
    kprintf("press keys, Esc to stop\n");
    for (;;) {
        int key = kmon_getkey();
        uint8_t m = keyboard_modifiers();
        if (key == 27) {
            break;
        }
        if (key >= 0x100) {
            kprintf("  special %-8s", keyboard_key_name(key));
        } else if (key >= 0x20 && key < 0x7F) {
            kprintf("  char '%c' (%3d)  ", (char)key, key);
        } else {
            kprintf("  ctrl  %3d        ", key);
        }
        if (last_key_from_serial) {
            kprintf(" (serial)\n");
            continue;
        }
        kprintf(" scancode=0x%02x mods=%s%s%s%s%s\n",
                (unsigned)keyboard_last_scancode(),
                (m & KEYMOD_SHIFT) ? "Shift " : "",
                (m & KEYMOD_CTRL) ? "Ctrl " : "",
                (m & KEYMOD_ALT) ? "Alt " : "",
                (m & KEYMOD_CAPS) ? "Caps " : "",
                (m & KEYMOD_NUM) ? "Num" : "");
    }
}

static void run_command(int argc, char **argv)
{
    const char *cmd = argv[0];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        cmd_help();
    } else if (!strcmp(cmd, "clear")) {
        tty_clear();
    } else if (!strcmp(cmd, "uptime")) {
        print_uptime();
    } else if (!strcmp(cmd, "irq")) {
        cmd_irq();
    } else if (!strcmp(cmd, "mem")) {
        print_memory_map();
        cmd_pmm();
    } else if (!strcmp(cmd, "pmm")) {
        cmd_pmm();
    } else if (!strcmp(cmd, "heap")) {
        cmd_heap();
    } else if (!strcmp(cmd, "heaptest")) {
        kprintf("heap self-test: %s\n", heap_selftest() ? "passed" : "FAILED");
    } else if (!strcmp(cmd, "v2p")) {
        cmd_v2p(argc, argv);
    } else if (!strcmp(cmd, "acpi")) {
        cmd_acpi();
    } else if (!strcmp(cmd, "pwd")) {
        kprintf("%s\n", cwd);
    } else if (!strcmp(cmd, "cd")) {
        cmd_cd(argc, argv);
    } else if (!strcmp(cmd, "ls")) {
        cmd_ls(argc, argv);
    } else if (!strcmp(cmd, "cat")) {
        cmd_cat(argc, argv);
    } else if (!strcmp(cmd, "mkdir")) {
        char path[VFS_PATH_MAX];
        int p = (argc > 2 && !strcmp(argv[1], "-p")), r;
        if (argc < 2 + p) { kprintf("usage: mkdir [-p] <dir>\n"); return; }
        if (path_arg(argc, argv, 1 + p, path) < 0) return;
        r = p ? vfs_mkdir_p(path) : vfs_mkdir(path);
        if (r < 0) report("mkdir", path, r);
    } else if (!strcmp(cmd, "touch")) {
        char path[VFS_PATH_MAX];
        int fd;
        if (argc < 2) { kprintf("usage: touch <file>\n"); return; }
        if (path_arg(argc, argv, 1, path) < 0) return;
        fd = vfs_open(path, O_WRONLY | O_CREAT);
        if (fd < 0) report("touch", path, fd); else vfs_close(fd);
    } else if (!strcmp(cmd, "write")) {
        cmd_write(argc, argv, 0);
    } else if (!strcmp(cmd, "append")) {
        cmd_write(argc, argv, 1);
    } else if (!strcmp(cmd, "rm") || !strcmp(cmd, "rmdir")) {
        char path[VFS_PATH_MAX];
        int r;
        if (argc < 2) { kprintf("usage: %s <path>\n", cmd); return; }
        if (path_arg(argc, argv, 1, path) < 0) return;
        r = !strcmp(cmd, "rm") ? vfs_unlink(path) : vfs_rmdir(path);
        if (r < 0) report(cmd, path, r);
    } else if (!strcmp(cmd, "stat")) {
        cmd_stat(argc, argv);
    } else if (!strcmp(cmd, "tree")) {
        cmd_tree(argc, argv);
    } else if (!strcmp(cmd, "mount")) {
        cmd_mount();
    } else if (!strcmp(cmd, "apic")) {
        cmd_apic();
    } else if (!strcmp(cmd, "mouse")) {
        cmd_mouse();
    } else if (!strcmp(cmd, "keys")) {
        cmd_keys();
    } else if (!strcmp(cmd, "sleep")) {
        uint64_t ms, start;
        if (argc < 2 || !parse_uint(argv[1], &ms)) {
            kprintf("usage: sleep <ms>\n");
            return;
        }
        start = timer_uptime_ms();
        timer_sleep_ms(ms);
        kprintf("slept %llu ms\n", (unsigned long long)(timer_uptime_ms() - start));
    } else if (!strcmp(cmd, "echo")) {
        int i;
        for (i = 1; i < argc; i++) {
            kprintf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
        }
        kprintf("\n");
    } else if (!strcmp(cmd, "int3")) {
        __asm__ __volatile__("int3");
        kprintf("returned from breakpoint handler\n");
    } else if (!strcmp(cmd, "fault")) {
        __asm__ __volatile__("ud2");
    } else if (!strcmp(cmd, "reboot")) {
        kprintf("rebooting...\n");
        cpu_cli();
        if (ps2_present()) {
            ps2_reset_cpu();
        }
        kprintf("reset failed - halting\n");
        cpu_halt_forever();
    } else if (!strcmp(cmd, "halt")) {
        kprintf("halted.\n");
        cpu_halt_forever();
    } else {
        kprintf("unknown command '%s' - type 'help'\n", cmd);
    }
}

void kmon_run(void)
{
    char line[KMON_LINE_MAX];
    char *argv[KMON_MAX_ARGS];
    int argc, n;

    kprintf("\nType 'help' for commands.\n");
    for (;;) {
        tty_set_color(TTY_COLOR_LIGHT_CYAN, TTY_COLOR_BLACK);
        kprintf("kmon:%s> ", cwd);
        tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);

        n = kmon_readline(line, sizeof(line));
        if (n <= 0) {
            continue;
        }
        argc = split_args(line, argv, KMON_MAX_ARGS);
        if (argc > 0) {
            run_command(argc, argv);
        }
    }
}
