/* rozOS — থ্রেড বদল (ধাপ ৬, GAS/AT&T)
 *
 * void switch_context(uint64_t *save_rsp, uint64_t new_rsp)
 *   SysV ABI-র callee-saved রেজিস্টারগুলো (rbx, rbp, r12-r15) বর্তমান স্ট্যাকে রেখে
 *   rsp *save_rsp-এ রাখে, তারপর new_rsp-এর স্ট্যাক থেকে একই ক্রমে তুলে `ret`।
 *   বাকি রেজিস্টার ডাকার নিয়মেই ডাকা ফাংশন নষ্ট করতে পারে, তাই সংরক্ষণ লাগে না।
 *   RFLAGS সংরক্ষিত হয় না: schedule() সবসময় IF = 0 অবস্থায় ডাকা হয় এবং প্রতিটি
 *   ডাকা জায়গা নিজের irq_restore দিয়ে আগের অবস্থা ফেরায়।
 *
 * নতুন থ্রেডের স্ট্যাক (sched.c সাজায়): r15 r14 r13 r12 rbx rbp ret
 *   kthread_start      rbx = ফাংশন, r12 = আর্গুমেন্ট
 *   user_thread_start  rbx = কার্নেল স্ট্যাকে রাখা interrupt_frame */

    .section .text

    .global switch_context
    .type switch_context, @function
switch_context:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size switch_context, . - switch_context

    .global kthread_start
    .type kthread_start, @function
kthread_start:
    movq    %r12, %rdi
    sti
    call    *%rbx
    call    sched_thread_exit   /* ফেরে না */
1:  hlt
    jmp     1b
    .size kthread_start, . - kthread_start

    .global user_thread_start
    .type user_thread_start, @function
user_thread_start:
    cli
    movq    %rbx, %rsp
    jmp     interrupt_return
    .size user_thread_start, . - user_thread_start

    .section .note.GNU-stack, "", @progbits
