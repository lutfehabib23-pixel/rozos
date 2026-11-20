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
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/kmon.h>
#include <arch/x86_64/cpu.h>

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

static int64_t sys_exit(struct interrupt_frame *f, uint64_t code, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_exit((int)code, 0);
    return 0;
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
    if ((int64_t)pid <= 0) {
        return -EINVAL;                          /* প্রসেস-গ্রুপ এখনো নেই */
    }
    if (sig > 64) {
        return -EINVAL;
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
    sched_sleep_ms(ms);
    if (urem) {
        memset(&ts, 0, sizeof(ts));
        copy_to_user((void *)(uintptr_t)urem, &ts, sizeof(ts));
    }
    return proc_current()->killed ? -EINTR : 0;
}

/* struct utsname: ৬টা 65-বাইটের ঘর */
static int64_t sys_uname(struct interrupt_frame *f, uint64_t ubuf, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    static const char *const fields[6] = {
        "rozOS", "rozos", "0.9.0-phase9", "phase9", "x86_64", "(none)"
    };
    char buf[6 * 65];
    int i;
    (void)f; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < 6; i++) {
        strncpy(buf + i * 65, fields[i], 64);
    }
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

void sys_proc_register(void)
{
    syscall_register(SYS_rozos_kmon, sys_rozos_kmon, "rozos_kmon");
    syscall_register(SYS_getpid, sys_getpid, "getpid");
    syscall_register(SYS_getppid, sys_getppid, "getppid");
    syscall_register(SYS_fork, sys_fork, "fork");
    syscall_register(SYS_execve, sys_execve, "execve");
    syscall_register(SYS_exit, sys_exit, "exit");
    syscall_register(SYS_exit_group, sys_exit, "exit_group");
    syscall_register(SYS_wait4, sys_wait4, "wait4");
    syscall_register(SYS_kill, sys_kill, "kill");
    syscall_register(SYS_sched_yield, sys_sched_yield, "sched_yield");
    syscall_register(SYS_nanosleep, sys_nanosleep, "nanosleep");
    syscall_register(SYS_uname, sys_uname, "uname");
    syscall_register(SYS_arch_prctl, sys_arch_prctl, "arch_prctl");
    syscall_register(SYS_clock_gettime, sys_clock_gettime, "clock_gettime");
}
