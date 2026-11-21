/* rozOS — IDT তৈরি, এক্সেপশন হ্যান্ডলিং ও ইন্টারাপ্ট ডিসপ্যাচ */

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>
#include <rozos/printf.h>
#include <rozos/panic.h>
#include <rozos/console.h>
#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "cpu.h"

#define IDT_TYPE_INTERRUPT_GATE 0x0E   /* ঢোকার সময় IF মুছে দেয় */
#define IDT_PRESENT             0x80
#define ISR_STUB_SIZE           16

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;           /* বিট 0-2 */
    uint8_t  type_attr;     /* P | DPL | 0 | type */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} ROZ_PACKED;

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} ROZ_PACKED;

extern char isr_stub_table[];   /* isr.S */

static struct idt_entry idt[IDT_ENTRIES] ROZ_ALIGNED(16);
static interrupt_handler_t handlers[IDT_ENTRIES];
static int  (*user_fault_hook)(struct interrupt_frame *frame);    /* ধাপ ৬ */
static void (*user_return_hook)(struct interrupt_frame *frame);   /* ধাপ ৬ */

static const char *const exception_names[IDT_EXCEPTION_COUNT] = {
    "Divide error (#DE)",              "Debug (#DB)",
    "Non-maskable interrupt (NMI)",    "Breakpoint (#BP)",
    "Overflow (#OF)",                  "Bound range exceeded (#BR)",
    "Invalid opcode (#UD)",            "Device not available (#NM)",
    "Double fault (#DF)",              "Coprocessor segment overrun",
    "Invalid TSS (#TS)",               "Segment not present (#NP)",
    "Stack-segment fault (#SS)",       "General protection fault (#GP)",
    "Page fault (#PF)",                "Reserved (15)",
    "x87 floating-point (#MF)",        "Alignment check (#AC)",
    "Machine check (#MC)",             "SIMD floating-point (#XM)",
    "Virtualization (#VE)",            "Control protection (#CP)",
    "Reserved (22)",                   "Reserved (23)",
    "Reserved (24)",                   "Reserved (25)",
    "Reserved (26)",                   "Reserved (27)",
    "Hypervisor injection (#HV)",      "VMM communication (#VC)",
    "Security exception (#SX)",        "Reserved (31)"
};

const char *exception_name(uint64_t vector)
{
    return vector < IDT_EXCEPTION_COUNT ? exception_names[vector] : "Interrupt";
}

static void set_gate(uint8_t vector, uint64_t addr, uint8_t ist, uint8_t dpl)
{
    struct idt_entry *e = &idt[vector];
    e->offset_low  = (uint16_t)(addr & 0xFFFF);
    e->selector    = GDT_KERNEL_CODE;
    e->ist         = ist & 0x7;
    e->type_attr   = (uint8_t)(IDT_PRESENT | ((dpl & 3) << 5) | IDT_TYPE_INTERRUPT_GATE);
    e->offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    e->offset_high = (uint32_t)(addr >> 32);
    e->reserved    = 0;
}

static uint64_t stub_address(unsigned vector)
{
    return (uint64_t)(uintptr_t)isr_stub_table + (uint64_t)vector * ISR_STUB_SIZE;
}

void idt_set_gate_options(uint8_t vector, uint8_t ist, uint8_t dpl)
{
    set_gate(vector, stub_address(vector), ist, dpl);
}

void idt_register_handler(uint8_t vector, interrupt_handler_t handler)
{
    handlers[vector] = handler;
}

void interrupt_dump_frame(const struct interrupt_frame *f)
{
    kprintf("  RIP=%016llx  CS=%04llx  RFLAGS=%016llx\n",
            (unsigned long long)f->rip, (unsigned long long)f->cs,
            (unsigned long long)f->rflags);
    kprintf("  RSP=%016llx  SS=%04llx  ERR=%016llx\n",
            (unsigned long long)f->rsp, (unsigned long long)f->ss,
            (unsigned long long)f->error_code);
    kprintf("  RAX=%016llx RBX=%016llx RCX=%016llx\n",
            (unsigned long long)f->rax, (unsigned long long)f->rbx, (unsigned long long)f->rcx);
    kprintf("  RDX=%016llx RSI=%016llx RDI=%016llx\n",
            (unsigned long long)f->rdx, (unsigned long long)f->rsi, (unsigned long long)f->rdi);
    kprintf("  RBP=%016llx R8 =%016llx R9 =%016llx\n",
            (unsigned long long)f->rbp, (unsigned long long)f->r8, (unsigned long long)f->r9);
    kprintf("  R10=%016llx R11=%016llx R12=%016llx\n",
            (unsigned long long)f->r10, (unsigned long long)f->r11, (unsigned long long)f->r12);
    kprintf("  R13=%016llx R14=%016llx R15=%016llx\n",
            (unsigned long long)f->r13, (unsigned long long)f->r14, (unsigned long long)f->r15);
    kprintf("  CR0=%016llx CR2=%016llx CR3=%016llx\n",
            (unsigned long long)cpu_read_cr0(), (unsigned long long)cpu_read_cr2(),
            (unsigned long long)cpu_read_cr3());
}

static void describe_page_fault(uint64_t err)
{
    kprintf("  Page fault at 0x%016llx: %s, %s, %s mode%s%s\n",
            (unsigned long long)cpu_read_cr2(),
            (err & 0x1) ? "protection violation" : "page not present",
            (err & 0x2) ? "write" : "read",
            (err & 0x4) ? "user" : "kernel",
            (err & 0x10) ? ", instruction fetch" : "",
            (err & 0x8) ? ", reserved bit set" : "");
}

/* কোনো হ্যান্ডলার নিবন্ধিত নেই এমন এক্সেপশন — রিপোর্ট করে কার্নেল থামানো */
static void unhandled_exception(struct interrupt_frame *f)
{
    tty_set_color(TTY_COLOR_WHITE, TTY_COLOR_RED);
    kprintf("\n*** CPU EXCEPTION %llu: %s ***\n",
            (unsigned long long)f->vector, exception_name(f->vector));
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    if (f->vector == EXC_PAGE_FAULT) {
        describe_page_fault(f->error_code);
    }
    interrupt_dump_frame(f);
    kpanic("unhandled %s in %s mode", exception_name(f->vector),
           (f->cs & 3) ? "user" : "kernel");
}

/* #BP (int3): ডিবাগিংয়ের জন্য — রিপোর্ট করে আগের জায়গায় ফিরে যায় */
static void breakpoint_handler(struct interrupt_frame *f)
{
    tty_set_color(TTY_COLOR_YELLOW, TTY_COLOR_BLACK);
    kprintf("[int3] breakpoint at RIP=0x%016llx - continuing\n", (unsigned long long)f->rip);
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
}

void idt_set_user_hooks(int (*fault)(struct interrupt_frame *frame),
                        void (*ret)(struct interrupt_frame *frame))
{
    user_fault_hook = fault;
    user_return_hook = ret;
}

/* ধাপ ১৫ক: ইউজারে ফেরার iretq নিজেই #GP দিলে (KVM-এ কার্নেল বিল্ডের সময় দেখা গেছে,
 * ERR=0x18 = user data selector) — আগে পুরো কার্নেল panic হত। এখন Linux-এর মতো:
 * কারণ লিখে রাখা (ফেরার ফ্রেম, GDT), GDT নষ্ট হলে ঠিক করে আবার চেষ্টা, নইলে শুধু ওই
 * প্রসেস SIGSEGV-তে শেষ — কার্নেল চলতে থাকে। */
extern char interrupt_iretq[], syscall_iretq[];

static int bad_iret(struct interrupt_frame *f)
{
    const uint64_t *b = (const uint64_t *)(uintptr_t)f->rsp;   /* rip, cs, rflags, rsp, ss */
    int to_user = (b[1] & 3) != 0, repaired;
    if (to_user) {
        __asm__ __volatile__(".byte 0x0f, 0x01, 0xf8");   /* swapgs (TCC-র assembler-এর জন্য বাইটে) */          /* iretq-র ঠিক আগে ইউজারের GS বসানো হয়েছিল */
    }
    tty_set_color(TTY_COLOR_YELLOW, TTY_COLOR_BLACK);
    kprintf("\n[kernel] iretq to %s failed (%s, error 0x%llx): RIP=%016llx CS=%04llx RFLAGS=%016llx RSP=%016llx SS=%04llx\n",
            to_user ? "user" : "kernel", exception_name(f->vector), (unsigned long long)f->error_code, (unsigned long long)b[0],
            (unsigned long long)b[1], (unsigned long long)b[2], (unsigned long long)b[3], (unsigned long long)b[4]);
    repaired = gdt_check_repair();
    tty_set_color(TTY_COLOR_LIGHT_GREY, TTY_COLOR_BLACK);
    if (!to_user) {
        return 0;                                /* কার্নেলে ফেরা ভাঙা — আগের মতো panic */
    }
    if (repaired && b[1] == GDT_USER_CODE_SEL && b[4] == GDT_USER_DATA_SEL) {
        kprintf("[kernel] GDT repaired - retrying the return to user mode\n");
        __asm__ __volatile__(".byte 0x0f, 0x01, 0xf8");   /* swapgs (TCC-র assembler-এর জন্য বাইটে) */          /* আবার ইউজারের GS — iretq-তে ফেরা */
        return 1;
    }
    if (user_fault_hook) {
        struct interrupt_frame u = *f;           /* ভাঙা ফ্রেমের প্রসেসটাকে দোষী ধরা */
        u.rip = b[0];
        u.cs = b[1];
        u.rflags = b[2];
        u.rsp = b[3];
        u.ss = b[4];
        u.vector = EXC_GENERAL_PROTECTION;
        user_fault_hook(&u);                     /* proc_exit — ফেরে না */
    }
    return 0;
}

void interrupt_dispatch(struct interrupt_frame *frame)
{
    uint64_t v = frame->vector;
    int from_user = (frame->cs & 3) != 0;

    /* #NP (11), #SS (12), #GP (13) — iretq-এর selector যাচাইয়ে যেকোনোটা আসতে পারে */
    if ((v == EXC_GENERAL_PROTECTION || v == 11 || v == 12) && !from_user &&
        (frame->rip == (uint64_t)(uintptr_t)interrupt_iretq || frame->rip == (uint64_t)(uintptr_t)syscall_iretq)) {
        if (bad_iret(frame)) {
            return;
        }
    }

    if (v >= PIC_VECTOR_BASE && v < PIC_VECTOR_BASE + PIC_IRQ_COUNT) {
        pic_dispatch(frame);
    } else if (v < IDT_EXCEPTION_COUNT && from_user && user_fault_hook && user_fault_hook(frame)) {
        /* ইউজার প্রোগ্রামের এক্সেপশন — প্রসেস সামলেছে (স্ট্যাক বাড়ানো বা প্রসেস শেষ) */
    } else if (handlers[v]) {
        handlers[v](frame);
    } else if (v < IDT_EXCEPTION_COUNT) {
        unhandled_exception(frame);
    } else {
        kprintf("[WARN] unexpected interrupt vector %llu\n", (unsigned long long)v);
    }

    /* ধাপ ৬: ইউজার-মোডে ফেরার আগে শিডিউলার (সময় ফুরালে অন্য থ্রেড, kill হলে শেষ) */
    if (from_user && user_return_hook) {
        user_return_hook(frame);
    }
}

void idt_init(void)
{
    struct idt_pointer ptr;
    unsigned v;

    for (v = 0; v < IDT_ENTRIES; v++) {
        set_gate((uint8_t)v, stub_address(v), 0, 0);
        handlers[v] = NULL;
    }
    /* নিশ্চিতভাবে ভালো স্ট্যাকে চলা দরকার এমন এক্সেপশন */
    idt_set_gate_options(EXC_DOUBLE_FAULT, IST_DOUBLE_FAULT, 0);
    idt_set_gate_options(EXC_NMI, IST_NMI, 0);
    idt_set_gate_options(EXC_MACHINE_CHECK, IST_MACHINE_CHECK, 0);

    idt_register_handler(EXC_BREAKPOINT, breakpoint_handler);

    ptr.limit = sizeof(idt) - 1;
    ptr.base  = (uint64_t)(uintptr_t)idt;
    __asm__ __volatile__("lidt %0" : : "m"(ptr) : "memory");
}
