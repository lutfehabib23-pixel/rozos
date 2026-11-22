/* rozOS — syscall-এর প্রবেশ ও ডিসপ্যাচ (rozos/syscall.h দেখুন) */

#include <stdint.h>
#include <stddef.h>
#include <rozos/syscall.h>
#include <rozos/sched.h>
#include <rozos/errno.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084
#define EFER_SCE   (1ULL << 0)

/* syscall-এ ঢোকার সময় মোছা RFLAGS বিট: TF, IF, DF, IOPL, NT, AC */
#define SYSCALL_FMASK 0x47700ULL

extern char syscall_entry[];

static syscall_fn_t table[SYSCALL_MAX];
static const char *names[SYSCALL_MAX];
static volatile uint64_t calls;

void syscall_register(unsigned nr, syscall_fn_t fn, const char *name)
{
    if (nr < SYSCALL_MAX) {
        table[nr] = fn;
        names[nr] = name;
    }
}

uint64_t syscall_count(void) { return calls; }

void syscall_dispatch(struct interrupt_frame *f)
{
    uint64_t nr = f->rax;

    calls++;
    {
        struct thread *t = sched_current();
        if (t) t->sys_nr = nr;                   /* ধাপ ১৬খ-১: SA_RESTART-এ আবার চালাতে */
    }
    if (nr < SYSCALL_MAX && table[nr]) {
        f->rax = (uint64_t)table[nr](f, f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    } else {
        f->rax = (uint64_t)(int64_t)-ENOSYS;
    }
}

/* syscall_entry.S থেকে: IF = 1 অবস্থায় আসে, IF = 0 করে ফেরে */
void syscall_fast_entry(struct interrupt_frame *f)
{
    syscall_dispatch(f);
    cpu_cli();
    sched_return_to_user(f);
}

/* int 0x80: interrupt gate IF মুছে দেয় — লম্বা syscall-এ ইন্টারাপ্ট আটকে না রাখতে চালু করা।
 * ফেরার আগে শিডিউলিং idt.c-র user-return hook করে। */
static void int80_handler(struct interrupt_frame *f)
{
    cpu_sti();
    syscall_dispatch(f);
    cpu_cli();
}

void syscall_init(void)
{
    /* sysret: CS = STAR[63:48] + 16, SS = STAR[63:48] + 8 → 0x10 থেকে user code 0x20, user data 0x18
     * syscall: CS = STAR[47:32], SS = +8 → kernel code 0x08, kernel data 0x10 */
    uint64_t star = ((uint64_t)(GDT_KERNEL_DATA) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);

    cpu_wrmsr(MSR_EFER, cpu_rdmsr(MSR_EFER) | EFER_SCE);
    cpu_wrmsr(MSR_STAR, star);
    cpu_wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    cpu_wrmsr(MSR_FMASK, SYSCALL_FMASK);

    idt_register_handler(SYSCALL_VECTOR, int80_handler);
    idt_set_gate_options(SYSCALL_VECTOR, 0, 3);

    sys_fs_register();
    sys_proc_register();
    sys_mm_register();
    sys_net_register();
}
