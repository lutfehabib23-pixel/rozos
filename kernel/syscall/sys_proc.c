/* rozOS — প্রসেস ও সময়-সংক্রান্ত syscall (Linux x86-64 ABI) */

#include <stdint.h>
#include <stddef.h>
#include <rozos/syscall.h>
#include <rozos/proc.h>
#include <rozos/sched.h>
#include <rozos/uaccess.h>
#include <rozos/timer.h>
#include <rozos/clock.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/pmm.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/kmon.h>
#include <rozos/random.h>
#include <arch/x86_64/cpu.h>
#include <rozos/block.h>
#include <rozos/acpi.h>
#include <rozos/ps2.h>
#include <rozos/printf.h>
#include <rozos/buildinfo.h>

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static int64_t sys_getpid(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return proc_current()->pid;
}

static int64_t sys_getppid(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return proc_current()->ppid;
}

static int64_t sys_fork(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return proc_fork(f);
}

static int64_t sys_execve(struct interrupt_frame *f, uint64_t upath, uint64_t uargv, uint64_t uenvp,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    char tmp[VFS_PATH_MAX], path[VFS_PATH_MAX];
    int64_t n;
    int err;
    (void)a4; (void)a5; (void)a6;

    n = copy_string_from_user(tmp, (const char *)(uintptr_t)upath, sizeof(tmp));
    if (n < 0) {
        return n;
    }
    err = proc_abs_path(tmp, path, sizeof(path));
    if (err < 0) {
        return err;
    }
    return proc_execve(f, path, (char *const *)(uintptr_t)uargv, (char *const *)(uintptr_t)uenvp);
}

/* ধাপ ১৬খ-২: exit (60) = শুধু এই থ্রেড (শেষ থ্রেড হলে পুরো প্রসেস — আগের মতো),
 * exit_group (231) = সব থ্রেডসহ প্রসেস। libc-র exit() exit_group ডাকে। */
static int64_t sys_exit(struct interrupt_frame *f, uint64_t code, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_thread_exit((int)code);
    return 0;
}

static int64_t sys_exit_group(struct interrupt_frame *f, uint64_t code, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_exit((int)code, 0);
    return 0;
}

/* clone(flags, newsp, parent_tid, child_tid, tls) — x86-64-এর ক্রম */
static int64_t sys_clone(struct interrupt_frame *f, uint64_t flags, uint64_t newsp, uint64_t uptid,
                         uint64_t uctid, uint64_t tls, uint64_t a6)
{
    (void)a6;
    return proc_clone(f, flags, newsp, uptid, uctid, tls);
}

static int64_t sys_set_tid_address(struct interrupt_frame *f, uint64_t addr, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct thread *t = sched_current();
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    t->clear_child_tid = addr;
    return t->utid ? t->utid : proc_current()->pid;
}

static int64_t sys_wait4(struct interrupt_frame *f, uint64_t pid, uint64_t ustatus, uint64_t options,
                         uint64_t urusage, uint64_t a5, uint64_t a6)
{
    (void)f; (void)urusage; (void)a5; (void)a6;
    return proc_wait4((int)(int64_t)pid, (int *)(uintptr_t)ustatus, (int)options);
}

static int64_t sys_kill(struct interrupt_frame *f, uint64_t pid, uint64_t sig, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (sig > 64) {
        return -EINVAL;
    }
    /* ধাপ ১৪: 0 = নিজের গ্রুপ, −N = গ্রুপ N */
    if ((int64_t)pid == 0) {
        int r = proc_kill_group(proc_current()->pgid, (int)sig);
        return r < 0 ? r : 0;
    }
    if ((int64_t)pid < 0) {
        int r;
        if ((int64_t)pid == -1) return -EPERM;   /* "সবাই" — দেওয়া হয় না */
        r = proc_kill_group((int)-(int64_t)pid, (int)sig);
        return r < 0 ? r : 0;
    }
    return proc_kill((int)pid, (int)sig);
}

static int64_t sys_sched_yield(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    sched_yield();
    return 0;
}

static int64_t sys_nanosleep(struct interrupt_frame *f, uint64_t ureq, uint64_t urem, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct linux_timespec ts;
    uint64_t ms;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;

    if (copy_from_user(&ts, (const void *)(uintptr_t)ureq, sizeof(ts)) < 0) {
        return -EFAULT;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
        return -EINVAL;
    }
    ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec + 999999) / 1000000;
    {
        /* ধাপ ১৬খ-১: signal-এ আগে জাগলে বাকি সময় urem-এ, ফেরত -EINTR */
        uint64_t start = timer_uptime_ms(), done;
        int intr;
        sched_sleep_ms(ms);
        done = timer_uptime_ms() - start;
        intr = proc_signal_pending(proc_current()) && done < ms;
        if (urem) {
            uint64_t left = intr ? ms - done : 0;
            ts.tv_sec = (int64_t)(left / 1000);
            ts.tv_nsec = (int64_t)(left % 1000) * 1000000;
            copy_to_user((void *)(uintptr_t)urem, &ts, sizeof(ts));
        }
        return intr ? -EINTR : 0;
    }
}

/* struct utsname: ৬টা 65-বাইটের ঘর */
static int64_t sys_uname(struct interrupt_frame *f, uint64_t ubuf, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    static const char *const fields[6] = {
        "rozOS", "rozos", "0.19.10-phase19j", "phase19j", "x86_64", "(none)"
    };
    char buf[6 * 65];
    int i;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < 6; i++) {
        strncpy(buf + i * 65, fields[i], 64);
    }
    /* ধাপ ১১খ: version = "#<বিল্ড> <compiler> <তারিখ>" (Linux-এর ধাঁচে) */
    ksnprintf(buf + 3 * 65, 65, "#%s %s %s", rozos_build_id, rozos_build_compiler, rozos_build_date);
    /* hostname ফাইল থাকলে সেটাই nodename */
    {
        int fd = vfs_open("/etc/hostname", O_RDONLY);
        if (fd >= 0) {
            int64_t n = vfs_read(fd, buf + 65, 64);
            vfs_close(fd);
            if (n > 0) {
                int64_t k;
                for (k = 0; k < n; k++) {
                    if (buf[65 + k] == '\n' || buf[65 + k] == '\r') {
                        break;
                    }
                }
                memset(buf + 65 + k, 0, (size_t)(65 - k));
            }
        }
    }
    return copy_to_user((void *)(uintptr_t)ubuf, buf, sizeof(buf));
}

static int64_t sys_arch_prctl(struct interrupt_frame *f, uint64_t code, uint64_t addr, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (code == ARCH_SET_FS) {
        if (addr >= VMM_USER_TOP) {
            return -EPERM;                       /* non-canonical/কার্নেল ঠিকানা নয় */
        }
        sched_set_fs_base(addr);
        return 0;
    }
    if (code == ARCH_GET_FS) {
        uint64_t v = sched_current()->fs_base;
        return copy_to_user((void *)(uintptr_t)addr, &v, sizeof(v));
    }
    return -EINVAL;
}

static int64_t sys_clock_gettime(struct interrupt_frame *f, uint64_t clk, uint64_t uts, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct linux_timespec ts;
    uint64_t ns = timer_uptime_ns();
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;

    if (clk == CLOCK_REALTIME) {
        ts.tv_sec = clock_unix_time();
        ts.tv_nsec = (int64_t)(ns % 1000000000ULL);
    } else if (clk == CLOCK_MONOTONIC) {
        ts.tv_sec = (int64_t)(ns / 1000000000ULL);
        ts.tv_nsec = (int64_t)(ns % 1000000000ULL);
    } else {
        return -EINVAL;
    }
    return copy_to_user((void *)(uintptr_t)uts, &ts, sizeof(ts));
}

/* ধাপ ৭: /bin/kmon — kmon আলাদা কার্নেল-থ্রেডে চলে (কার্নেলের fd টেবিল ও cwd নিয়ে),
 * ডাকা প্রসেস "exit" না দেওয়া পর্যন্ত ঘুমিয়ে থাকে */
static struct thread *kmon_waiter;
static volatile int kmon_busy, kmon_done;

static int kmon_thread(void *arg)
{
    (void)arg;
    kmon_session();
    kmon_done = 1;
    if (kmon_waiter) {
        sched_wake(kmon_waiter);
    }
    return 0;
}

/* ================= ধাপ ১৯চ: sysinfo (99) আর প্রসেস-তালিকা (509) — Settings-এর জন্য ================= */

struct linux_sysinfo {                      /* Linux x86-64-এর বিন্যাস */
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char     reserved[4];
};

static int64_t sys_sysinfo(struct interrupt_frame *f, uint64_t uinfo, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct linux_sysinfo si;
    struct pmm_stats st;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    memset(&si, 0, sizeof(si));
    pmm_get_stats(&st);
    si.uptime = (int64_t)(timer_uptime_ms() / 1000);
    si.totalram = st.usable_pages * PMM_PAGE_SIZE;
    si.freeram = st.free_pages * PMM_PAGE_SIZE;
    si.procs = (uint16_t)proc_count();
    si.mem_unit = 1;
    return copy_to_user((void *)(uintptr_t)uinfo, &si, sizeof(si)) < 0 ? -EFAULT : 0;
}

static int64_t sys_proclist(struct interrupt_frame *f, uint64_t ubuf, uint64_t len, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    char *k;
    size_t i, o = 0;
    uint64_t hz = timer_frequency() ? timer_frequency() : 1000;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    if (len == 0) return 0;
    if (len > 32768) len = 32768;
    k = (char *)kzalloc(len);
    if (!k) return -ENOMEM;
    for (i = 0; i < proc_count() && o + 1 < len; i++) {
        struct process *p = proc_at(i);
        const char *state = p->state == PROC_ZOMBIE ? "zombie" : p->thread ? sched_state_name(p->thread->state) : "?";
        uint64_t ms = p->thread ? p->thread->cpu_ticks * 1000 / hz : 0;
        uint64_t kib = p->state == PROC_ZOMBIE ? 0 : vmm_user_pages(p->space) * 4;
        o += (size_t)ksnprintf(k + o, len - o, "%d %d %s %llu %d %llu %s\n", p->pid, p->ppid, state,
                               (unsigned long long)ms, p->state == PROC_ZOMBIE ? 0 : p->nthreads,
                               (unsigned long long)kib, p->name);
    }
    if (o >= len) o = len - 1;
    if (copy_to_user((void *)(uintptr_t)ubuf, k, o + 1) < 0) o = (size_t)-EFAULT;
    kfree(k);
    return (int64_t)o;
}

static int64_t sys_rozos_kmon(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (kmon_busy) {
        return -EBUSY;
    }
    kmon_busy = 1;
    kmon_done = 0;
    kmon_waiter = sched_current();
    if (!sched_create_kthread("kmon", kmon_thread, NULL)) {
        kmon_busy = 0;
        return -ENOMEM;
    }
    while (!kmon_done) {
        uint64_t flags = cpu_irq_save();
        if (!kmon_done) {
            sched_block();
        }
        cpu_irq_restore(flags);
    }
    kmon_waiter = NULL;
    kmon_busy = 0;
    return 0;
}

/* ধাপ ১০: getrandom(buf, len, flags) — কার্নেলের CSPRNG থেকে (কখনো আটকায় না) */
static int64_t sys_getrandom(struct interrupt_frame *f, uint64_t ubuf, uint64_t len, uint64_t flags,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint8_t tmp[256];
    uint64_t done = 0;
    (void)f; (void)flags; (void)a4; (void)a5; (void)a6;
    if (len > 1024 * 1024) len = 1024 * 1024;
    while (done < len) {
        size_t n = len - done > sizeof(tmp) ? sizeof(tmp) : (size_t)(len - done);
        random_get(tmp, n);
        if (copy_to_user((void *)(uintptr_t)(ubuf + done), tmp, n) < 0) {
            return done ? (int64_t)done : -EFAULT;
        }
        done += n;
    }
    memset(tmp, 0, sizeof(tmp));
    return (int64_t)done;
}

/* ধাপ ১১খ: reboot(magic1, magic2, cmd, arg) — Linux-এর মতো magic যাচাই।
 * cmd: RESTART 0x01234567, POWER_OFF 0x4321FEDC, HALT 0xCDEF0123।
 * আগে ডিস্ক sync (write-through cache, তবু ডিস্কের নিজস্ব cache খালি করা)। */
#define REBOOT_MAGIC1    0xfee1deadu
#define REBOOT_MAGIC2    672274793u
#define REBOOT_RESTART   0x01234567u
#define REBOOT_POWER_OFF 0x4321FEDCu
#define REBOOT_HALT      0xCDEF0123u

static int64_t sys_reboot(struct interrupt_frame *f, uint64_t m1, uint64_t m2, uint64_t cmd,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a4; (void)a5; (void)a6;
    if ((uint32_t)m1 != REBOOT_MAGIC1 || (uint32_t)m2 != REBOOT_MAGIC2) {
        return -EINVAL;
    }
    if ((uint32_t)cmd != REBOOT_RESTART && (uint32_t)cmd != REBOOT_POWER_OFF && (uint32_t)cmd != REBOOT_HALT) {
        return -EINVAL;
    }
    block_sync(NULL);
    if ((uint32_t)cmd == REBOOT_RESTART) {
        kprintf("\n[rozOS] restarting...\n");
        ps2_reset_cpu();
    } else if ((uint32_t)cmd == REBOOT_POWER_OFF) {
        kprintf("\n[rozOS] power off\n");
        acpi_poweroff();
        kprintf("[rozOS] ACPI power off failed - it is now safe to turn off the computer\n");
    } else {
        kprintf("\n[rozOS] system halted - it is now safe to turn off the computer\n");
    }
    cpu_cli();
    for (;;) {
        cpu_hlt();
    }
    return 0;
}

/* ---------- ধাপ ১৪: process group, session, SIG_IGN ---------- */

static int64_t sys_setpgid(struct interrupt_frame *f, uint64_t pid, uint64_t pgid, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *me = proc_current(), *p;
    (void)f; (void)a3; (void)a4; (void)a5; (void)a6;
    p = pid ? proc_find((int)pid) : me;
    if (!p) return -ESRCH;
    if (p != me && p->ppid != me->pid) return -ESRCH;
    if ((int64_t)pgid < 0) return -EINVAL;
    p->pgid = pgid ? (int)pgid : p->pid;
    return 0;
}

static int64_t sys_getpgid(struct interrupt_frame *f, uint64_t pid, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    p = pid ? proc_find((int)pid) : proc_current();
    return p ? p->pgid : -ESRCH;
}

static int64_t sys_getpgrp(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return proc_current()->pgid;
}

static int64_t sys_setsid(struct interrupt_frame *f, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p = proc_current();
    (void)f; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    p->sid = p->pid;
    p->pgid = p->pid;
    return p->sid;
}

static int64_t sys_getsid(struct interrupt_frame *f, uint64_t pid, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct process *p;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    p = pid ? proc_find((int)pid) : proc_current();
    return p ? p->sid : -ESRCH;
}

/* rt_sigaction ধাপ ১৬খ-১ থেকে kernel/proc/signal.c-তে (আসল handler, mask, sigreturn) */

void sys_proc_register(void)
{
    syscall_register(SYS_setpgid, sys_setpgid, "setpgid");        /* ধাপ ১৪ */
    syscall_register(SYS_getpgid, sys_getpgid, "getpgid");
    syscall_register(SYS_getpgrp, sys_getpgrp, "getpgrp");
    syscall_register(SYS_setsid, sys_setsid, "setsid");
    syscall_register(SYS_getsid, sys_getsid, "getsid");
    syscall_register(SYS_reboot, sys_reboot, "reboot");
    syscall_register(SYS_getrandom, sys_getrandom, "getrandom");
    syscall_register(SYS_rozos_kmon, sys_rozos_kmon, "rozos_kmon");
    syscall_register(SYS_sysinfo, sys_sysinfo, "sysinfo");              /* ধাপ ১৯চ */
    syscall_register(SYS_rozos_proclist, sys_proclist, "proclist");
    syscall_register(SYS_getpid, sys_getpid, "getpid");
    syscall_register(SYS_getppid, sys_getppid, "getppid");
    syscall_register(SYS_fork, sys_fork, "fork");
    syscall_register(SYS_execve, sys_execve, "execve");
    syscall_register(SYS_exit, sys_exit, "exit");
    syscall_register(SYS_exit_group, sys_exit_group, "exit_group");
    syscall_register(56, sys_clone, "clone");                    /* ধাপ ১৬খ-২ */
    syscall_register(218, sys_set_tid_address, "set_tid_address");
    syscall_register(SYS_wait4, sys_wait4, "wait4");
    syscall_register(SYS_kill, sys_kill, "kill");
    syscall_register(SYS_sched_yield, sys_sched_yield, "sched_yield");
    syscall_register(SYS_nanosleep, sys_nanosleep, "nanosleep");
    syscall_register(SYS_uname, sys_uname, "uname");
    syscall_register(SYS_arch_prctl, sys_arch_prctl, "arch_prctl");
    syscall_register(SYS_clock_gettime, sys_clock_gettime, "clock_gettime");
    proc_signal_init();                          /* ধাপ ১৬খ-১: rt_sigaction … sigaltstack, alarm */
    futex_init();                                /* ধাপ ১৬খ-২ */
}
