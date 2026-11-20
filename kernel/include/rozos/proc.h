/* rozOS — প্রসেস (ধাপ ৬)
 *
 * প্রতিটি প্রসেস: নিজস্ব ঠিকানা-স্থান (PML4), নিজস্ব fd টেবিল, cwd, একটা থ্রেড।
 * pid 0 = কার্নেল (কোনো process struct নেই); কার্নেল থেকে চালানো প্রসেসের ppid 0।
 *
 * ইউজার ঠিকানা-বিন্যাস:
 *   0x0000000000400000…        ELF (স্ট্যাটিক, ET_EXEC) — তারপর brk heap
 *   0x0000700000000000…        mmap এলাকা (নিচ থেকে ওপরে)
 *   0x00007FFFFFFFF000 নিচে     ইউজার স্ট্যাক: শুরুতে 64 KiB, page fault-এ 8 MiB পর্যন্ত বাড়ে */
#ifndef ROZOS_PROC_H
#define ROZOS_PROC_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>

#define PROC_MAX            64
#define PROC_NAME_MAX       32

#define USER_STACK_TOP      0x00007FFFFFFFF000ULL
#define USER_STACK_INITIAL  (64ULL * 1024)
#define USER_STACK_MAX      (8ULL * 1024 * 1024)
#define USER_MMAP_BASE      0x0000700000000000ULL
#define USER_MMAP_END       (USER_STACK_TOP - USER_STACK_MAX - 0x100000ULL)
#define USER_MIN_ADDR       0x0000000000001000ULL

/* execve: আর্গুমেন্টের সীমা */
#define PROC_ARG_MAX        64
#define PROC_ARGS_BYTES     (32 * 1024)

/* signal নম্বর (Linux-এর একই) — এখন শুধু প্রসেস শেষ করার কারণ হিসেবে */
#define SIGILL   4
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV  11
#define SIGTERM  15

enum proc_state {
    PROC_ALIVE = 1,
    PROC_ZOMBIE
};

struct thread;
struct vmm_space;
struct interrupt_frame;

struct process {
    int pid;
    int ppid;
    char name[PROC_NAME_MAX];
    enum proc_state state;
    int exit_code;                  /* exit() মান (0-255) */
    int term_signal;                /* signal-এ মারা গেলে তার নম্বর, নইলে 0 */
    int killed;                     /* kill করা হয়েছে (signal নম্বর) — পরের ইউজার-ফেরায় শেষ */
    int waiting;                    /* wait4()-এ ঘুমিয়ে আছে */
    int autoreap;                   /* কেউ wait করবে না — শেষ হলে নিজেই মুছে যায় */
    struct thread *kwaiter;         /* কার্নেলের কোনো থ্রেড এই প্রসেসের জন্য অপেক্ষায় */
    struct vmm_space *space;
    struct vfs_fdtable *fdt;
    char cwd[VFS_PATH_MAX];
    uint64_t brk_start, brk_end;    /* heap: [brk_start, brk_end) ম্যাপ করা */
    uint64_t mmap_next;
    uint64_t start_tick;
    struct thread *thread;
};

void proc_init(void);

/* কার্নেল থেকে নতুন প্রসেস (ppid 0), stdin/stdout/stderr = /dev/console।
 * argv NULL-সমাপ্ত (NULL হলে argv[0] = path)। ফেরত pid বা -ERRNO। */
int proc_spawn(const char *path, const char *const *argv, int autoreap);

/* কার্নেলের কোনো থ্রেড থেকে অপেক্ষা: শেষ হলে status (wait4-এর মতো এনকোড করা) দেয়,
 * timeout_ms = 0 মানে চিরকাল; সময় ফুরালে -ETIMEDOUT */
int proc_wait_kernel(int pid, int *status, uint64_t timeout_ms);

int  proc_kill(int pid, int sig);
/* zombie প্রসেসের struct মুক্ত করা (sched.c-র reaper ও wait ডাকে) */
void proc_free(struct process *p);
struct process *proc_current(void);
struct process *proc_find(int pid);
size_t proc_count(void);
struct process *proc_at(size_t index);

/* syscall-এর জন্য (sys_proc.c) */
int64_t proc_fork(struct interrupt_frame *frame);
int64_t proc_execve(struct interrupt_frame *frame, const char *path,
                    char *const *uargv, char *const *uenvp);
void    proc_exit(int code, int sig);   /* ফেরে না */
int64_t proc_wait4(int pid, int *ustatus, int options);

/* ইউজার-মোডের এক্সেপশন (idt.c-র hook): 1 = সামলানো হয়েছে */
int proc_user_fault(struct interrupt_frame *frame);

/* পাথ: আপেক্ষিক হলে প্রসেসের cwd যোগ করে পরম পাথ বানায় */
int proc_abs_path(const char *in, char *out, size_t size);

/* wait4 status এনকোডিং (Linux) */
#define W_EXITCODE(code)   (((code) & 0xFF) << 8)
#define W_SIGNALED(sig)    ((sig) & 0x7F)
#define WNOHANG 1

#endif
