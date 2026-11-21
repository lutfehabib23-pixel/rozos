/* rozOS — syscall (ধাপ ৬)
 *
 * দুটো পথ, একই নম্বর ও একই ABI (Linux x86-64):
 *   syscall নির্দেশ  — দ্রুত পথ (STAR/LSTAR/FMASK MSR), ফেরা sysretq দিয়ে
 *   int 0x80         — ব্যাকআপ পথ (DPL 3 gate), ফেরা iretq দিয়ে
 * নম্বর rax-এ, আর্গুমেন্ট rdi, rsi, rdx, r10, r8, r9; ফলাফল rax-এ (ত্রুটি -ERRNO)। */
#ifndef ROZOS_SYSCALL_H
#define ROZOS_SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0x80

/* Linux x86-64 নম্বর */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_ioctl           16
#define SYS_writev          20
#define SYS_sched_yield     24
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_pipe            22      /* ধাপ ১৩ */
#define SYS_pipe2           293     /* ধাপ ১৩ */
#define SYS_poll            7       /* ধাপ ১৩ */
/* ধাপ ৯: নেটওয়ার্ক */
#define SYS_socket          41
#define SYS_connect         42
#define SYS_accept          43
#define SYS_sendto          44
#define SYS_recvfrom        45
#define SYS_shutdown        48
#define SYS_bind            49
#define SYS_listen          50
#define SYS_getsockname     51
#define SYS_getpeername     52
#define SYS_setsockopt      54
#define SYS_getsockopt      55
#define SYS_accept4         288
#define SYS_getrandom       318     /* ধাপ ১০ */
#define SYS_reboot          169     /* ধাপ ১১খ */
#define SYS_nanosleep       35
#define SYS_getpid          39
#define SYS_fork            57
#define SYS_execve          59
#define SYS_exit            60
#define SYS_wait4           61
#define SYS_kill            62
#define SYS_uname           63
#define SYS_truncate        76
#define SYS_getcwd          79
#define SYS_chdir           80
#define SYS_mkdir           83
#define SYS_rmdir           84
#define SYS_unlink          87
#define SYS_rename          82   /* ধাপ ১৫ক */
#define SYS_getppid         110
#define SYS_rt_sigaction    13      /* ধাপ ১৪: শুধু SIG_IGN / SIG_DFL মানে রাখে */
#define SYS_setpgid         109     /* ধাপ ১৪ */
#define SYS_getpgrp         111
#define SYS_setsid          112
#define SYS_getpgid         121
#define SYS_getsid          124
#define SYS_arch_prctl      158
#define SYS_getdents64      217
#define SYS_clock_gettime   228
#define SYS_exit_group      231

/* rozOS-এর নিজস্ব (Linux-এর সংখ্যার অনেক ওপরে, যাতে কখনো না মেলে) — ধাপ ৭ */
#define SYS_rozos_kmon      500     /* কার্নেল মনিটর চালানো, "exit" পর্যন্ত অপেক্ষা */
#define SYS_rozos_netinfo   501     /* ধাপ ৯: interface-এর তথ্য (struct rozos_netinfo) */
#define SYS_rozos_shm_open  503     /* ধাপ ১৩: (name, flags, mode) → fd */
#define SYS_rozos_shm_unlink 504    /* ধাপ ১৩ */
#define SYS_rozos_openpty   505     /* ধাপ ১৪: (int fds[2]) master, slave */
#define SYS_init_module     175     /* ধাপ ১৫ক: (image, len, name) — Linux-এর মতো নম্বর */
#define SYS_delete_module   176     /* ধাপ ১৫ক: (name, flags) */
#define SYS_rozos_modlist   506     /* ধাপ ১৫ক: (buf, len, what) what 0 = মডিউল, 1 = কার্নেল প্রতীক */
#define SYS_mount           165     /* ধাপ ১৫ক: (source, target, fstype, flags, data) */
#define SYS_umount2         166     /* ধাপ ১৫ক: (target, flags) */
#define SYS_rozos_usblist   508     /* ধাপ ১৫খ: (buf, len) lsusb */
#define SYS_rozos_mounts    507     /* ধাপ ১৫ক: (buf, len) "source path fstype\n" সারি */
#define SYS_ftruncate       77      /* ধাপ ১৩ */
#define SYS_rozos_netconfig 502     /* ধাপ ৯: হাতে IP বসানো / আবার DHCP */

#define SYSCALL_MAX         512

struct interrupt_frame;

typedef int64_t (*syscall_fn_t)(struct interrupt_frame *f, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

void syscall_init(void);                 /* MSR + int 0x80 */
void syscall_dispatch(struct interrupt_frame *frame);
uint64_t syscall_count(void);

/* sys_*.c নিজেদের নিবন্ধন করে */
void syscall_register(unsigned nr, syscall_fn_t fn, const char *name);
void sys_fs_register(void);
void sys_proc_register(void);
void sys_mm_register(void);
void sys_net_register(void);

#endif
