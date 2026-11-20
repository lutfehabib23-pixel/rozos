/* rozOS — নতুন GDT লোড ও সেগমেন্ট রেজিস্টার নতুন করে বসানো (GAS/AT&T)
 *
 * void gdt_load(const struct gdt_pointer *ptr, uint16_t code_sel, uint16_t data_sel)
 *   rdi = ptr, si = code_sel, dx = data_sel
 * void tss_load(uint16_t sel)
 *   di = sel */

    .section .text

    .global gdt_load
    .type gdt_load, @function
gdt_load:
    lgdt    (%rdi)

    /* data segment রেজিস্টার */
    movw    %dx, %ds
    movw    %dx, %es
    movw    %dx, %ss
    /* FS/GS: null — পরে এদের base MSR দিয়ে per-CPU ডেটার জন্য বসানো হবে */
    xorl    %eax, %eax
    movw    %ax, %fs
    movw    %ax, %gs

    /* CS সরাসরি mov করা যায় না — far return দিয়ে নতুন CS লোড */
    movzwq  %si, %rsi
    pushq   %rsi
    leaq    1f(%rip), %rax
    pushq   %rax
    lretq
1:
    ret
    .size gdt_load, . - gdt_load

    .global tss_load
    .type tss_load, @function
tss_load:
    ltr     %di
    ret
    .size tss_load, . - tss_load

    .section .note.GNU-stack, "", @progbits
