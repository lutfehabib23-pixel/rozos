/* rozOS — প্রসেস: spawn, fork, execve, exit, wait, kill, page fault (rozos/proc.h দেখুন)
 *
 * জীবনচক্র:
 *   ALIVE ─exit/fault/kill─▶ ZOMBIE (ঠিকানা-স্থান ও fd সাথে সাথে মুক্ত, শুধু struct থাকে)
 *         ─wait4/proc_wait_kernel─▶ মুক্ত
 *   autoreap = 1 হলে (কেউ wait করবে না) থ্রেড মুছে যাওয়ার সময় struct-ও মুক্ত হয়।
 *   বাবা আগে মারা গেলে সন্তানের ppid = 0 আর autoreap = 1।
 *
 * কার্নেলের থ্রেডরা প্রসেস নয় (proc = NULL), তাদের fd টেবিল VFS-এর কার্নেল টেবিল। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/proc.h>
#include <rozos/sched.h>
#include <rozos/elf.h>
#include <rozos/vmm.h>
#include <rozos/pmm.h>
#include <rozos/heap.h>
#include <rozos/vfs.h>
#include <rozos/uaccess.h>
#include <rozos/timer.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/console.h>
#include <rozos/fbdev.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>

#define USER_RFLAGS  0x202          /* IF = 1, বিট 1 সবসময় 1 */

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9

static struct process *procs[PROC_MAX];
static int next_pid = 1;

/* ধাপ ১৬খ-২: থ্রেডের id pid-এর একই গোনা থেকে (Linux-এর মতো — কখনো মেলে না) */
int proc_alloc_tid(void)
{
    uint64_t fl = cpu_irq_save();
    int t = next_pid++;
    cpu_irq_restore(fl);
    return t;
}

/* ---------- টেবিল ---------- */

static struct vfs_fdtable *fdt_of_current(void)
{
    struct thread *t = sched_current();
    return (t && t->proc) ? t->proc->fdt : NULL;
}

void proc_init(void)
{
    vfs_set_fdtable_hook(fdt_of_current);
    idt_set_user_hooks(proc_user_fault, sched_return_to_user);
}

struct process *proc_current(void)
{
    struct thread *t = sched_current();
    return t ? t->proc : NULL;
}

struct process *proc_find(int pid)
{
    int i;
    for (i = 0; i < PROC_MAX; i++) {
        if (procs[i] && procs[i]->pid == pid) {
            return procs[i];
        }
    }
    return NULL;
}

size_t proc_count(void)
{
    size_t i, n = 0;
    for (i = 0; i < PROC_MAX; i++) {
        n += procs[i] != NULL;
    }
    return n;
}

struct process *proc_at(size_t index)
{
    size_t i;
    for (i = 0; i < PROC_MAX; i++) {
        if (procs[i] && index-- == 0) {
            return procs[i];
        }
    }
    return NULL;
}

static struct process *proc_alloc(const char *name, int ppid)
{
    struct process *p;
    int i;

    for (i = 0; i < PROC_MAX && procs[i]; i++) {
    }
    if (i == PROC_MAX) {
        return NULL;
    }
    p = (struct process *)kmalloc(sizeof(*p));
    if (!p) {
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->pid = next_pid++;
    p->ppid = ppid;
    p->pgid = p->pid;                    /* ধাপ ১৪: নিজের গ্রুপ (fork-এ মা-র গ্রুপ নেয়) */
    p->sid = p->pid;
    p->state = PROC_ALIVE;
    p->mmap_next = USER_MMAP_BASE;
    p->start_tick = timer_ticks();
    strcpy(p->cwd, "/");
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    procs[i] = p;
    return p;
}

void proc_free(struct process *p)
{
    int i;
    for (i = 0; i < PROC_MAX; i++) {
        if (procs[i] == p) {
            procs[i] = NULL;
        }
    }
    if (p->fdt) {
        vfs_fdtable_destroy(p->fdt);
    }
    if (p->space) {
        vmm_destroy_space(p->space, 1);
    }
    kfree(p);
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (; *path; path++) {
        if (*path == '/' && path[1]) {
            b = path + 1;
        }
    }
    return b;
}

/* ---------- পাথ ---------- */

int proc_abs_path(const char *in, char *out, size_t size)
{
    struct process *p = proc_current();
    size_t a, b;

    if (!in || !*in) {
        return -ENOENT;
    }
    if (in[0] == '/' || !p) {
        if (strlen(in) + 1 > size) {
            return -ENAMETOOLONG;
        }
        strcpy(out, in);
        return 0;
    }
    a = strlen(p->cwd);
    b = strlen(in);
    if (a + 1 + b + 1 > size) {
        return -ENAMETOOLONG;
    }
    strcpy(out, p->cwd);
    if (a > 0 && out[a - 1] != '/') {
        out[a++] = '/';
    }
    memcpy(out + a, in, b + 1);
    return 0;
}

/* ---------- ইউজার মেমোরি ---------- */

static int in_stack_area(uint64_t addr)
{
    return addr < USER_STACK_TOP && addr >= USER_STACK_TOP - USER_STACK_MAX;
}

/* স্ট্যাক এলাকার না-আসা পেজ আনা (শূন্য, RW) */
static int fault_in_stack(struct process *p, uint64_t addr)
{
    uint64_t va = addr & ~(VMM_PAGE_SIZE - 1), phys;

    if (!p || !in_stack_area(addr)) {
        return 0;
    }
    if (vmm_query(p->space, va, NULL, NULL)) {
        return 1;                                /* ধাপ ১৬খ-২: আরেক থ্রেড এর মধ্যেই এনেছে */
    }
    phys = pmm_alloc_zeroed_page();
    if (!phys) {
        return 0;
    }
    if (!vmm_map_page(p->space, va, phys, VMM_USER | VMM_WRITE)) {
        pmm_free_page(phys);
        return 0;
    }
    return 1;
}

int user_range_ok(uint64_t addr, size_t len, int write)
{
    struct process *p = proc_current();
    uint64_t va, end;

    if (!p || !p->space) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }
    end = addr + len;
    if (end < addr || end > VMM_USER_TOP || addr < USER_MIN_ADDR) {
        return 0;
    }
    for (va = addr & ~(VMM_PAGE_SIZE - 1); va < end; va += VMM_PAGE_SIZE) {
        uint32_t f;
        if (!vmm_query(p->space, va, NULL, &f)) {
            if (!fault_in_stack(p, va)) {
                return 0;
            }
            continue;
        }
        if (!(f & VMM_USER) || (write && !(f & VMM_WRITE))) {
            return 0;
        }
    }
    return 1;
}

int copy_from_user(void *dst, const void *usrc, size_t len)
{
    if (!user_range_ok((uint64_t)(uintptr_t)usrc, len, 0)) {
        return -EFAULT;
    }
    memcpy(dst, usrc, len);
    return 0;
}

int copy_to_user(void *udst, const void *src, size_t len)
{
    if (!user_range_ok((uint64_t)(uintptr_t)udst, len, 1)) {
        return -EFAULT;
    }
    memcpy(udst, src, len);
    return 0;
}

int64_t copy_string_from_user(char *dst, const char *usrc, size_t max)
{
    uint64_t a = (uint64_t)(uintptr_t)usrc;
    size_t n = 0;

    while (n < max) {
        /* প্রতিটি নতুন পেজে একবার যাচাই */
        if (n == 0 || ((a + n) & (VMM_PAGE_SIZE - 1)) == 0) {
            if (!user_range_ok(a + n, 1, 0)) {
                return -EFAULT;
            }
        }
        dst[n] = usrc[n];
        if (dst[n] == '\0') {
            return (int64_t)n;
        }
        n++;
    }
    return -ENAMETOOLONG;
}

/* অন্য (এখনো চালু না হওয়া) ঠিকানা-স্থানে লেখা — HHDM দিয়ে, পেজ ধরে ধরে */
static int space_write(struct vmm_space *sp, uint64_t va, const void *src, size_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    while (len > 0) {
        uint64_t phys = vmm_virt_to_phys(sp, va);
        size_t chunk = VMM_PAGE_SIZE - (va & (VMM_PAGE_SIZE - 1));
        if (!phys) {
            return -EFAULT;
        }
        if (chunk > len) {
            chunk = len;
        }
        memcpy(pmm_phys_to_virt(phys), s, chunk);
        va += chunk;
        s += chunk;
        len -= chunk;
    }
    return 0;
}

/* ---------- আর্গুমেন্ট ও স্ট্যাক ---------- */

/* argv/envp এক জায়গায় জড়ো করা (কার্নেলের বাফারে) — পুরোনো ঠিকানা-স্থান মোছার আগে */
struct arg_pack {
    char *buf;                  /* PROC_ARGS_BYTES */
    size_t used;
    int argc, envc;
    char *argv[PROC_ARG_MAX];
    char *envp[PROC_ARG_MAX];
};

static int pack_add(struct arg_pack *ap, const char *s, int env)
{
    size_t n = strlen(s) + 1;
    if (ap->used + n > PROC_ARGS_BYTES) {
        return -E2BIG;
    }
    if (env ? ap->envc >= PROC_ARG_MAX : ap->argc >= PROC_ARG_MAX) {
        return -E2BIG;
    }
    memcpy(ap->buf + ap->used, s, n);
    if (env) {
        ap->envp[ap->envc++] = ap->buf + ap->used;
    } else {
        ap->argv[ap->argc++] = ap->buf + ap->used;
    }
    ap->used += n;
    return 0;
}

/* ইউজারের NULL-সমাপ্ত char *[] থেকে */
static int pack_from_user(struct arg_pack *ap, char *const *uv, int env)
{
    char tmp[256];
    int i, err;

    if (!uv) {
        return 0;
    }
    for (i = 0; ; i++) {
        uint64_t ptr;
        int64_t n;
        if (copy_from_user(&ptr, &uv[i], sizeof(ptr)) < 0) {
            return -EFAULT;
        }
        if (!ptr) {
            return 0;
        }
        n = copy_string_from_user(tmp, (const char *)(uintptr_t)ptr, sizeof(tmp));
        if (n == -ENAMETOOLONG) {
            /* লম্বা আর্গুমেন্ট: সরাসরি বাফারে (দৈর্ঘ্য আগে যাচাই করে) */
            size_t room = PROC_ARGS_BYTES - ap->used;
            n = copy_string_from_user(ap->buf + ap->used, (const char *)(uintptr_t)ptr, room);
            if (n < 0) {
                return n == -ENAMETOOLONG ? -E2BIG : (int)n;
            }
            if (env ? ap->envc >= PROC_ARG_MAX : ap->argc >= PROC_ARG_MAX) {
                return -E2BIG;
            }
            if (env) ap->envp[ap->envc++] = ap->buf + ap->used;
            else     ap->argv[ap->argc++] = ap->buf + ap->used;
            ap->used += (size_t)n + 1;
            continue;
        }
        if (n < 0) {
            return (int)n;
        }
        err = pack_add(ap, tmp, env);
        if (err < 0) {
            return err;
        }
    }
}

/* ইউজার স্ট্যাক ম্যাপ করে SysV ABI অনুযায়ী সাজানো:
 *   [উঁচু] স্ট্রিংগুলো … auxv, NULL, envp…, NULL, argv…, argc ← rsp (16-byte aligned) */
static int setup_stack(struct vmm_space *sp, const struct arg_pack *ap,
                       const struct elf_image *img, uint64_t *rsp_out)
{
    uint64_t va, str_top = USER_STACK_TOP, sp_va;
    uint64_t argv_va[PROC_ARG_MAX], envp_va[PROC_ARG_MAX];
    uint64_t *words;
    size_t nwords, k;
    int i, err = 0;

    for (va = USER_STACK_TOP - USER_STACK_INITIAL; va < USER_STACK_TOP; va += VMM_PAGE_SIZE) {
        uint64_t phys = pmm_alloc_zeroed_page();
        if (!phys) {
            return -ENOMEM;
        }
        if (!vmm_map_page(sp, va, phys, VMM_USER | VMM_WRITE)) {
            pmm_free_page(phys);
            return -ENOMEM;
        }
    }

    /* স্ট্রিং (envp আগে, তারপর argv — ক্রম গুরুত্বপূর্ণ নয়) */
    for (i = ap->envc - 1; i >= 0 && err == 0; i--) {
        size_t n = strlen(ap->envp[i]) + 1;
        str_top -= n;
        envp_va[i] = str_top;
        err = space_write(sp, str_top, ap->envp[i], n);
    }
    for (i = ap->argc - 1; i >= 0 && err == 0; i--) {
        size_t n = strlen(ap->argv[i]) + 1;
        str_top -= n;
        argv_va[i] = str_top;
        err = space_write(sp, str_top, ap->argv[i], n);
    }
    if (err < 0) {
        return err;
    }

    /* argc + argv + NULL + envp + NULL + auxv (৬ জোড়া) */
    nwords = 1 + (size_t)ap->argc + 1 + (size_t)ap->envc + 1 + 12;
    sp_va = ((str_top & ~15ULL) - nwords * 8) & ~15ULL;
    words = (uint64_t *)kmalloc(nwords * 8);
    if (!words) {
        return -ENOMEM;
    }
    k = 0;
    words[k++] = (uint64_t)ap->argc;
    for (i = 0; i < ap->argc; i++) words[k++] = argv_va[i];
    words[k++] = 0;
    for (i = 0; i < ap->envc; i++) words[k++] = envp_va[i];
    words[k++] = 0;
    words[k++] = AT_PAGESZ; words[k++] = VMM_PAGE_SIZE;
    words[k++] = AT_ENTRY;  words[k++] = img->entry;
    words[k++] = AT_PHDR;   words[k++] = img->phdr_vaddr;
    words[k++] = AT_PHENT;  words[k++] = 56;
    words[k++] = AT_PHNUM;  words[k++] = img->phnum;
    words[k++] = AT_NULL;   words[k++] = 0;
    err = space_write(sp, sp_va, words, nwords * 8);
    kfree(words);
    if (err < 0) {
        return err;
    }
    *rsp_out = sp_va;
    return 0;
}

static void user_frame(struct interrupt_frame *f, uint64_t rip, uint64_t rsp)
{
    memset(f, 0, sizeof(*f));
    f->rip = rip;
    f->rsp = rsp;
    f->cs = GDT_USER_CODE_SEL;
    f->ss = GDT_USER_DATA_SEL;
    f->rflags = USER_RFLAGS;
    f->vector = 0x80;
}

/* নতুন ঠিকানা-স্থানে ELF আর স্ট্যাক — সফল হলে *space_out ও entry/rsp */
static int build_image(const char *path, const struct arg_pack *ap, struct vmm_space **space_out,
                       struct elf_image *img, uint64_t *rsp)
{
    struct vmm_space *sp = vmm_create_space();
    int err;

    if (!sp) {
        return -ENOMEM;
    }
    err = elf_load(sp, path, img);
    if (err == 0) {
        err = setup_stack(sp, ap, img, rsp);
    }
    if (err < 0) {
        vmm_destroy_space(sp, 1);
        return err;
    }
    *space_out = sp;
    return 0;
}

/* ---------- spawn (কার্নেল থেকে) ---------- */

static int open_console_fds(void)
{
    int fd = vfs_open("/dev/console", O_RDWR);
    if (fd < 0) {
        return fd;
    }
    if (vfs_dup(fd) < 0 || vfs_dup(fd) < 0) {
        return -EMFILE;
    }
    return 0;
}

int proc_spawn(const char *path, const char *const *argv, int autoreap)
{
    struct arg_pack ap;
    struct process *p;
    struct thread *t;
    struct elf_image img;
    struct interrupt_frame f;
    uint64_t rsp;
    int err, i;

    memset(&ap, 0, sizeof(ap));
    ap.buf = (char *)kmalloc(PROC_ARGS_BYTES);
    if (!ap.buf) {
        return -ENOMEM;
    }
    err = 0;
    if (argv && argv[0]) {
        for (i = 0; argv[i] && err == 0; i++) {
            err = pack_add(&ap, argv[i], 0);
        }
    } else {
        err = pack_add(&ap, path, 0);
    }
    if (err == 0) err = pack_add(&ap, "PATH=/bin", 1);
    if (err == 0) err = pack_add(&ap, "HOME=/home", 1);
    if (err == 0) err = pack_add(&ap, "TERM=rozos", 1);

    p = err == 0 ? proc_alloc(base_name(path), 0) : NULL;
    if (err == 0 && !p) {
        err = -EAGAIN;
    }
    if (err == 0) {
        p->autoreap = autoreap;
        p->fdt = vfs_fdtable_create();
        err = p->fdt ? build_image(path, &ap, &p->space, &img, &rsp) : -ENOMEM;
    }
    if (err == 0) {
        /* stdin/out/err নতুন প্রসেসের নিজের টেবিলে খুলতে হবে: VFS চলমান থ্রেডের
         * প্রসেস দেখে টেবিল বাছে, তাই কিছুক্ষণের জন্য এই থ্রেডকে নতুন প্রসেসের সাথে
         * যুক্ত করা হয় (এর মধ্যে থ্রেড বদল হলেও ক্ষতি নেই — কার্নেল-অর্ধেক সব
         * ঠিকানা-স্থানে একই) */
        struct thread *self = sched_current();
        struct process *saved = self->proc;
        self->proc = p;
        err = open_console_fds();
        self->proc = saved;
    }
    if (err == 0) {
        p->brk_start = p->brk_end = img.brk_start;
        user_frame(&f, img.entry, rsp);
        t = sched_create_user_thread(p->name, p, &f, NULL);
        if (!t) {
            err = -ENOMEM;
        } else {
            p->thread = t;
            t->utid = p->pid;                    /* ধাপ ১৬খ-২ */
            p->nthreads = 1;
        }
    }
    kfree(ap.buf);
    if (err < 0) {
        if (p) {
            proc_free(p);
        }
        return err;
    }
    sched_start_thread(p->thread);
    return p->pid;
}

/* ---------- fork ---------- */

int64_t proc_fork(struct interrupt_frame *frame)
{
    struct process *parent = proc_current(), *child;
    struct thread *t;
    struct interrupt_frame f;

    if (!parent) {
        return -EPERM;
    }
    child = proc_alloc(parent->name, parent->pid);
    if (!child) {
        return -EAGAIN;
    }
    strcpy(child->cwd, parent->cwd);
    child->pgid = parent->pgid;              /* ধাপ ১৪ */
    child->sid = parent->sid;
    child->sigignore = parent->sigignore;
    proc_signal_fork(child, parent);         /* ধাপ ১৬খ-১: handler, mask, বিকল্প স্ট্যাক */
    child->brk_start = parent->brk_start;
    child->brk_end = parent->brk_end;
    child->mmap_next = parent->mmap_next;
    child->space = vmm_create_space();
    child->fdt = vfs_fdtable_clone();
    if (!child->space || !child->fdt || !vmm_clone_user(child->space, parent->space)) {
        proc_free(child);
        return -ENOMEM;
    }
    memcpy(&f, frame, sizeof(f));
    f.rax = 0;                                   /* সন্তানে fork() = 0 */
    sched_fpu_save_current();
    t = sched_create_user_thread(child->name, child, &f, sched_current()->fpu);
    if (!t) {
        proc_free(child);
        return -ENOMEM;
    }
    t->fs_base = sched_current()->fs_base;
    t->utid = child->pid;                        /* ধাপ ১৬খ-২: সন্তানে শুধু ডাকা থ্রেডটা */
    child->nthreads = 1;
    child->thread = t;
    sched_start_thread(t);
    return child->pid;
}

/* ---------- ধাপ ১৬খ-২: থ্রেড ---------- */

static void mark_exit(struct thread *t, void *arg)
{
    if (t != (struct thread *)arg) {
        t->exit_req = 1;
        if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
            t->state = THREAD_READY;             /* অপেক্ষা ভাঙুক (sched_should_abort) */
        }
    }
}

/* নিজে ছাড়া প্রসেসের বাকি সব থ্রেড শেষ করানো, শেষ না হওয়া পর্যন্ত অপেক্ষা */
static void kill_other_threads(struct process *p)
{
    struct thread *self = sched_current();
    while (p->nthreads > 1) {
        sched_for_each_proc_thread(p, mark_exit, self);
        sched_sleep_ms(1);
    }
}

/* CLONE_* (Linux) */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED       0x00400000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_THREAD_NEED    (CLONE_VM | CLONE_SIGHAND | CLONE_THREAD)

/* clone: CLONE_THREAD হলে একই প্রসেসে নতুন থ্রেড (নিজের স্ট্যাক ও TLS নিয়ে); CLONE_VM ছাড়া
 * সাধারণ fork (SIGCHLD = 17 নিচের বাইটে) — অন্য মিশ্রণ এখনো নেই */
int64_t proc_clone(struct interrupt_frame *frame, uint64_t flags, uint64_t newsp,
                   uint64_t uptid, uint64_t uctid, uint64_t tls)
{
    struct process *p = proc_current();
    struct thread *t;
    struct interrupt_frame f;
    int tid;

    if (!p) return -EPERM;
    if (!(flags & CLONE_VM) && !(flags & (CLONE_THREAD | CLONE_SIGHAND))) {
        int64_t r = proc_fork(frame);            /* fork() বা vfork()-এর মতো ব্যবহার */
        if (r > 0 && (flags & CLONE_PARENT_SETTID) && uptid) {
            int32_t v = (int32_t)r;
            copy_to_user((void *)(uintptr_t)uptid, &v, sizeof(v));
        }
        return r;
    }
    if ((flags & CLONE_THREAD_NEED) != CLONE_THREAD_NEED || !(flags & CLONE_FILES)) {
        return -EINVAL;
    }
    if (!newsp || newsp >= VMM_USER_TOP || (tls && tls >= VMM_USER_TOP)) {
        return -EINVAL;
    }
    if (p->exiting) return -EAGAIN;
    tid = proc_alloc_tid();
    if ((flags & CLONE_PARENT_SETTID) && uptid) {
        int32_t v = tid;
        if (copy_to_user((void *)(uintptr_t)uptid, &v, sizeof(v)) < 0) return -EFAULT;
    }
    if ((flags & CLONE_CHILD_SETTID) && uctid) {
        int32_t v = tid;
        if (copy_to_user((void *)(uintptr_t)uctid, &v, sizeof(v)) < 0) return -EFAULT;
    }
    memcpy(&f, frame, sizeof(f));
    f.rax = 0;                                   /* নতুন থ্রেডে clone() = 0 */
    f.rsp = newsp;
    sched_fpu_save_current();
    t = sched_create_user_thread(p->name, p, &f, sched_current()->fpu);
    if (!t) return -ENOMEM;
    t->utid = tid;
    t->fs_base = (flags & CLONE_SETTLS) ? tls : sched_current()->fs_base;
    t->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? uctid : 0;
    {
        uint64_t fl = cpu_irq_save();
        p->nthreads++;
        cpu_irq_restore(fl);
    }
    sched_start_thread(t);
    return tid;
}

/* শুধু চলমান থ্রেডটা শেষ (pthread_exit, বা exit_group-এ বাকিরা); শেষ থ্রেড হলে প্রসেসও */
struct pick_arg { struct thread *self, *found; };

static void pick_other(struct thread *t, void *arg)
{
    struct pick_arg *a = (struct pick_arg *)arg;
    if (t != a->self && !a->found) a->found = t;
}

void proc_thread_exit(int code)
{
    struct thread *self = sched_current();
    struct process *p = self->proc;
    uint64_t fl;

    if (!p) sched_thread_exit();
    fl = cpu_irq_save();
    if (p->nthreads <= 1 && !(p->exiting && p->exiter != self)) {
        cpu_irq_restore(fl);
        proc_exit(code, 0);                      /* শেষ থ্রেড: পুরো প্রসেস */
    }
    cpu_irq_restore(fl);
    if (self->clear_child_tid && !p->exiting) {
        int32_t zero = 0;
        if (copy_to_user((void *)(uintptr_t)self->clear_child_tid, &zero, sizeof(zero)) == 0) {
            futex_wake_addr(p, self->clear_child_tid, 1);     /* pthread_join জাগে */
        }
    }
    fl = cpu_irq_save();
    p->nthreads--;
    if (p->thread == self) {
        struct pick_arg a;
        a.self = self;
        a.found = NULL;
        sched_for_each_proc_thread(p, pick_other, &a);
        p->thread = a.found;                     /* signal এখন এই থ্রেড পাবে */
    }
    if (p->wait_thread == self) p->wait_thread = NULL;
    self->proc = NULL;                           /* reaper যেন প্রসেসের struct না ছোঁয় */
    if (p->exiting && p->exiter) sched_wake(p->exiter);
    cpu_irq_restore(fl);
    sched_thread_exit();
}

/* ---------- execve ---------- */

int64_t proc_execve(struct interrupt_frame *frame, const char *path,
                    char *const *uargv, char *const *uenvp)
{
    struct process *p = proc_current();
    struct arg_pack ap;
    struct vmm_space *sp, *old;
    struct elf_image img;
    uint64_t rsp;
    int err;

    if (!p) {
        return -EPERM;
    }
    memset(&ap, 0, sizeof(ap));
    ap.buf = (char *)kmalloc(PROC_ARGS_BYTES);
    if (!ap.buf) {
        return -ENOMEM;
    }
    if (p->nthreads > 1) {
        /* ধাপ ১৬খ-২: POSIX — execve-তে বাকি থ্রেড শেষ, ডাকা থ্রেডই প্রসেসের থ্রেড */
        p->exiting = 1;
        p->exiter = sched_current();
        kill_other_threads(p);
        p->exiting = 0;
        p->exiter = NULL;
        p->thread = sched_current();
        sched_current()->utid = p->pid;
        sched_current()->clear_child_tid = 0;
    }
    err = pack_from_user(&ap, uargv, 0);
    if (err == 0) err = pack_from_user(&ap, uenvp, 1);
    if (err == 0 && ap.argc == 0) err = pack_add(&ap, path, 0);
    if (err == 0) err = build_image(path, &ap, &sp, &img, &rsp);
    kfree(ap.buf);
    if (err < 0) {
        return err;                              /* পুরোনো প্রোগ্রাম অক্ষত থাকে */
    }

    /* ফেরার পথ নেই: পুরোনো ঠিকানা-স্থান মুছে নতুনটা */
    old = p->space;
    p->space = sp;
    vmm_switch(sp);
    vmm_destroy_space(old, 1);
    p->brk_start = p->brk_end = img.brk_start;
    p->mmap_next = USER_MMAP_BASE;
    memset(p->name, 0, sizeof(p->name));
    strncpy(p->name, base_name(path), PROC_NAME_MAX - 1);
    memset(sched_current()->name, 0, SCHED_NAME_MAX);
    strncpy(sched_current()->name, p->name, SCHED_NAME_MAX - 1);

    user_frame(frame, img.entry, rsp);
    proc_signal_exec(p);                         /* ধাপ ১৬খ-১: ধরা signal ডিফল্টে ফেরে */
    sched_set_fs_base(0);
    sched_fpu_load(sched_fpu_initial_state());
    return 0;
}

/* ---------- exit / wait ---------- */

void proc_exit(int code, int sig)
{
    struct process *p = proc_current(), *parent;
    int i;

    if (!p) {
        sched_thread_exit();
    }
    /* ধাপ ১৬খ-২: আরেকটা থ্রেড আগেই শেষ শুরু করেছে — এই থ্রেড শুধু নিজে শেষ */
    {
        uint64_t fl = cpu_irq_save();
        if (p->exiting && p->exiter && p->exiter != sched_current()) {
            cpu_irq_restore(fl);
            proc_thread_exit(code);
        }
        p->exiting = 1;
        p->exiter = sched_current();
        cpu_irq_restore(fl);
    }
    kill_other_threads(p);                       /* বাকি থ্রেড শেষ না হলে ঠিকানা-স্থান মোছা যায় না */
    p->thread = sched_current();
    /* সন্তানদের অনাথ করা */
    for (i = 0; i < PROC_MAX; i++) {
        struct process *c = procs[i];
        if (c && c->ppid == p->pid) {
            c->ppid = 0;
            if (c->state == PROC_ZOMBIE && !c->thread) {
                proc_free(c);
            } else {
                c->autoreap = 1;
            }
        }
    }

    vfs_fdtable_destroy(p->fdt);
    p->fdt = NULL;
    vmm_switch(vmm_kernel_space());
    vmm_destroy_space(p->space, 1);
    p->space = NULL;
    fbdev_proc_exit(p->pid);             /* ধাপ ১৩: গ্রাফিক্স প্রোগ্রাম শেষ → টেক্সট কনসোল */

    p->exit_code = code & 0xFF;
    p->term_signal = sig;
    p->state = PROC_ZOMBIE;

    parent = p->ppid ? proc_find(p->ppid) : NULL;
    if (parent && parent->waiting && (parent->wait_thread || parent->thread)) {
        sched_wake(parent->wait_thread ? parent->wait_thread : parent->thread);   /* ধাপ ১৬খ-২ */
    }
    if (parent) {                                /* ধাপ ১৬খ-১: মা-কে SIGCHLD (ডিফল্টে উপেক্ষা) */
        proc_send_signal(parent, SIGCHLD, p->pid, sig ? 2 /* CLD_KILLED */ : 1 /* CLD_EXITED */,
                         sig ? sig : (code & 0xFF));
    }
    if (p->kwaiter) {
        sched_wake(p->kwaiter);
    }
    if (!parent && !p->kwaiter) {
        p->autoreap = 1;                         /* কার্নেল থেকে চালানো, কেউ অপেক্ষায় নেই */
    }
    sched_thread_exit();
}

static int encode_status(const struct process *c)
{
    return c->term_signal ? W_SIGNALED(c->term_signal) : W_EXITCODE(c->exit_code);
}

int64_t proc_wait4(int pid, int *ustatus, int options)
{
    struct process *me = proc_current();

    if (!me) {
        return -ECHILD;
    }
    for (;;) {
        int i, found = 0;
        for (i = 0; i < PROC_MAX; i++) {
            struct process *c = procs[i];
            if (!c || c->ppid != me->pid || (pid > 0 && c->pid != pid)) {
                continue;
            }
            found = 1;
            if (c->state == PROC_ZOMBIE) {
                int status = encode_status(c), cpid = c->pid;
                if (ustatus && copy_to_user(ustatus, &status, sizeof(status)) < 0) {
                    return -EFAULT;
                }
                if (c->thread) {
                    c->autoreap = 1;             /* থ্রেড মোছার সময় struct যাবে */
                    c->ppid = 0;
                } else {
                    proc_free(c);
                }
                return cpid;
            }
        }
        if (!found) {
            return -ECHILD;
        }
        if (options & WNOHANG) {
            return 0;
        }
        if (proc_signal_pending(me)) {           /* ধাপ ১৬খ-১: যেকোনো signal-এ (SA_RESTART-এ আবার) */
            return -EINTR;
        }
        {
            uint64_t flags = cpu_irq_save();
            me->waiting = 1;
            me->wait_thread = sched_current();   /* ধাপ ১৬খ-২: কোন থ্রেড অপেক্ষায় */
            sched_block();
            me->waiting = 0;
            me->wait_thread = NULL;
            cpu_irq_restore(flags);
        }
    }
}

int proc_wait_kernel(int pid, int *status, uint64_t timeout_ms)
{
    struct process *p = proc_find(pid);
    uint64_t deadline = timeout_ms ? timer_uptime_ms() + timeout_ms : 0;

    if (!p || p->ppid != 0) {
        return -ECHILD;
    }
    p->autoreap = 0;
    p->kwaiter = sched_current();
    while (p->state != PROC_ZOMBIE) {
        if (deadline && timer_uptime_ms() >= deadline) {
            p->kwaiter = NULL;
            return -ETIMEDOUT;
        }
        if (deadline) {
            sched_sleep_ms(10);                  /* সময়সীমা দেখতে মাঝে মাঝে জাগা */
        } else {
            uint64_t flags = cpu_irq_save();
            if (p->state != PROC_ZOMBIE) {
                sched_block();
            }
            cpu_irq_restore(flags);
        }
    }
    if (status) {
        *status = encode_status(p);
    }
    p->kwaiter = NULL;
    if (p->thread) {
        p->autoreap = 1;
    } else {
        proc_free(p);
    }
    return pid;
}

int proc_kill(int pid, int sig)
{
    struct process *p = proc_find(pid);

    if (!p) {
        return -ESRCH;
    }
    if (p->state == PROC_ZOMBIE) {
        return 0;
    }
    if (sig == 0) {
        return 0;                                /* শুধু অস্তিত্ব যাচাই */
    }
    /* ধাপ ১৪: SIG_IGN করা signal উপেক্ষা (SIGKILL ছাড়া) */
    if (sig != SIGKILL && sig >= 1 && sig <= 64 && (p->sigignore & (1ULL << (sig - 1)))) {
        return 0;
    }
    /* ধাপ ১৬খ-১: আগে সরাসরি killed (= প্রসেস শেষ) বসত; এখন অপেক্ষমাণ signal হিসেবে —
     * handler থাকলে সেটা চলে, নইলে ডিফল্ট (শেষ বা উপেক্ষা), ঘুম থেকে তোলা আগের মতোই */
    {
        struct process *me = proc_current();
        return proc_send_signal(p, sig, me ? me->pid : 0, 0 /* SI_USER */, 0);
    }
}

int proc_kill_group(int pgid, int sig)
{
    int i, n = 0;
    if (pgid <= 0) return -EINVAL;
    for (i = 0; i < PROC_MAX; i++) {
        struct process *p = procs[i];
        if (p && p->pgid == pgid && p->state != PROC_ZOMBIE) {
            int ignored = sig != SIGKILL && sig >= 1 && sig <= 64 && (p->sigignore & (1ULL << (sig - 1)));
            proc_kill(p->pid, sig);
            if (!ignored) n++;
        }
    }
    return n ? n : -ESRCH;     /* কেউ পায়নি (সবাই উপেক্ষা করে বা কেউ নেই) */
}

/* ---------- ইউজার-মোডের এক্সেপশন ---------- */

int proc_user_fault(struct interrupt_frame *f)
{
    struct process *p = proc_current();
    int sig;

    if (!p) {
        return 0;
    }
    if (f->vector == EXC_PAGE_FAULT && !(f->error_code & 1)) {
        uint64_t addr = cpu_read_cr2();
        /* স্ট্যাক বাড়ানো: rsp-এর একটু নিচে (push/call) পর্যন্ত গ্রহণযোগ্য */
        if (in_stack_area(addr) && addr + 65536 >= f->rsp && fault_in_stack(p, addr)) {
            return 1;
        }
    }
    switch (f->vector) {
    case EXC_BREAKPOINT:
        return 0;                                /* idt.c-র breakpoint হ্যান্ডলার */
    case EXC_DIVIDE_ERROR:
    case EXC_X87_FP:
    case EXC_SIMD_FP:
        sig = SIGFPE;
        break;
    case EXC_INVALID_OPCODE:
        sig = SIGILL;
        break;
    default:
        sig = SIGSEGV;
        break;
    }
    /* ধাপ ১৬খ-১: প্রোগ্রামের নিজের handler থাকলে (যেমন SIGSEGV ধরে siglongjmp) সেটা চলবে */
    if (proc_signal_fault(p, sig, f->vector == EXC_PAGE_FAULT ? cpu_read_cr2() : f->rip)) {
        return 1;
    }
    tty_set_color(TTY_COLOR_LIGHT_RED, TTY_COLOR_BLACK);
    kprintf("[proc] pid %d (%s): %s at rip 0x%llx", p->pid, p->name,
            exception_name(f->vector), (unsigned long long)f->rip);
    if (f->vector == EXC_PAGE_FAULT) {
        kprintf(", address 0x%llx", (unsigned long long)cpu_read_cr2());
    }
    kprintf(" - killed (signal %d)\n", sig);
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    cpu_sti();
    proc_exit(0, sig);
    return 1;
}
