/* rozOS — ২৫৬টি ইন্টারাপ্ট এন্ট্রি stub ও সাধারণ হ্যান্ডলার (GAS/AT&T)
 *
 * প্রতিটি stub ঠিক 16 বাইট সীমায় শুরু হয়, তাই ভেক্টর n-এর stub-এর ঠিকানা =
 * isr_stub_table + 16 * n। idt.c এভাবেই ঠিকানা হিসাব করে — আলাদা নামের
 * দরকার পড়ে না।
 *
 * যেসব এক্সেপশনে CPU নিজে error code push করে (8, 10-14, 17, 21, 29, 30),
 * সেগুলো বাদে বাকি সবগুলোয় stub একটা 0 push করে, যাতে স্ট্যাকের বিন্যাস
 * সবসময় একই থাকে (struct interrupt_frame)।
 *
 * স্ট্যাক alignment: 64-bit মোডে CPU ফ্রেম push করার আগে rsp-কে 16-এ মেলায়।
 * CPU-র 5টা + error + vector + 15টা রেজিস্টার = 22 × 8 = 176 বাইট (16-এর গুণিতক),
 * তাই `call interrupt_dispatch`-এর আগে rsp 16-byte aligned থাকে (SysV ABI)।
 *
 * ধাপ ৬: ইউজার-মোড (CS-এর RPL = 3) থেকে এলে ঢোকা ও বেরোনোর সময় swapgs —
 * কার্নেলে GS = CPU-র নিজস্ব তথ্য (sched.c-র cpu_local), ইউজারে GS = ইউজারের।
 * interrupt_return লেবেলটা বাইরে থেকেও ব্যবহার হয়: নতুন ইউজার থ্রেড ও fork-এর
 * সন্তান কার্নেল স্ট্যাকে একটা interrupt_frame বানিয়ে এখানে লাফ দেয়। */

#include <rozos/asm_compat.h>

    .section .text

    .global isr_stub_table
    .align 16
isr_stub_table:
#ifdef __TINYC__
    /* ধাপ ১১: TCC-র assembler .if-এর ভেতরে ! আর || বোঝে না — একই ২৫৬টা stub
     * (একই ক্রম, একই 16-বাইট সারি) isr_stubs_tcc.inc-এ স্পষ্ট করে লেখা */
#include "isr_stubs_tcc.inc"
#else
    .set vec, 0
    .rept 256
    .align 16
    .if !((vec == 8) || ((vec >= 10) && (vec <= 14)) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30))
    pushq   $0                  /* নকল error code */
    .endif
    pushq   $vec
    jmp     isr_common
    .set vec, vec + 1
    .endr
#endif

    .align 16
    .type isr_common, @function
isr_common:
    testb   $3, 24(%rsp)        /* CS (vector, error, rip-এর পরে) */
    jz      1f
    SWAPGS
1:
    cld                         /* SysV ABI: direction flag শূন্য */
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

    movq    %rsp, %rdi          /* struct interrupt_frame * */
    call    interrupt_dispatch

    .global interrupt_return
interrupt_return:
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
    testb   $3, 8(%rsp)         /* ফেরার CS */
    jz      2f
    SWAPGS
2:
    iretq
    .size isr_common, . - isr_common

    .section .note.GNU-stack, "", @progbits
