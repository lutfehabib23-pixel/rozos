/* rozOS — `syscall` নির্দেশের প্রবেশপথ (ধাপ ৬, GAS/AT&T)
 *
 * CPU যা করে: rcx = ইউজারের rip, r11 = ইউজারের rflags, CS/SS = কার্নেলের (STAR),
 * RFLAGS থেকে FMASK-এর বিট মোছে (IF সহ)। স্ট্যাক বদলায় না — তাই প্রথমেই swapgs
 * করে CPU-র নিজস্ব তথ্য (sched.c-র struct cpu_local) থেকে কার্নেল স্ট্যাক নেওয়া:
 *   %gs:0  kernel_rsp   (চলমান থ্রেডের কার্নেল স্ট্যাকের মাথা)
 *   %gs:8  user_rsp     (সাময়িক জায়গা)
 *
 * তারপর int 0x80-এর মতোই একটা সম্পূর্ণ struct interrupt_frame বানানো হয়, যাতে
 * দুই পথে একই C কোড (syscall_dispatch) চলে এবং fork/execve ফ্রেম বদলাতে পারে।
 *
 * ফেরা: সাধারণত sysretq (rcx = rip, r11 = rflags)। ফেরার rip non-canonical হলে
 * sysret কার্নেল-মোডেই #GP দেয় (পরিচিত নিরাপত্তা ত্রুটি) — তখন iretq পথ। */

#include <rozos/asm_compat.h>

#define USER_DATA_SEL  0x1B
#define USER_CODE_SEL  0x23

    .section .text

    .global syscall_entry
    .type syscall_entry, @function
syscall_entry:
    SWAPGS
    movq    %rsp, %gs:8
    movq    %gs:0, %rsp

    pushq   $USER_DATA_SEL      /* ss */
    pushq   %gs:8               /* rsp */
    pushq   %r11                /* rflags */
    pushq   $USER_CODE_SEL      /* cs */
    pushq   %rcx                /* rip */
    pushq   $0                  /* error code */
    pushq   $0x80               /* vector: syscall */

    cld
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rsp, %rdi
    sti
    call    syscall_fast_entry  /* syscall.c: dispatch + sched_return_to_user, IF = 0 করে ফেরে */
    cli

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    addq    $16, %rsp           /* vector + error code */

    /* এখন স্ট্যাকে: rip, cs, rflags, rsp, ss */
    movq    (%rsp), %rcx
    shrq    $47, %rcx           /* canonical ইউজার ঠিকানা: উপরের 17 বিট শূন্য */
    jnz     1f
    movq    (%rsp), %rcx        /* rip */
    movq    16(%rsp), %r11      /* rflags */
    movq    24(%rsp), %rsp      /* ইউজারের স্ট্যাক — এরপর কার্নেল স্ট্যাক ব্যবহার নয় */
    SWAPGS
    sysretq
1:
    movq    -40(%rsp), %rcx     /* shrq-এ নষ্ট হওয়া rcx: ফ্রেমে rip-এর 5 ঘর নিচে ছিল */
    SWAPGS
    iretq
    .size syscall_entry, . - syscall_entry

    .section .note.GNU-stack, "", @progbits
