/* rozlibc — signal handler-এর ফেরা (ধাপ ১৬খ-১)
 *
 * কার্নেল handler-এর ফেরার ঠিকানা হিসেবে এটা বসায় (sigaction-এর SA_RESTORER):
 * handler `ret` করলে এখানে আসে, তখন rt_sigreturn (15) — কার্নেল স্ট্যাকের ফ্রেম থেকে
 * সব রেজিস্টার, signal mask আর FPU অবস্থা ফিরিয়ে আগের জায়গায় চালায়। */

    .section .text
    .global __restore_rt
    .type __restore_rt, @function
__restore_rt:
    movq    $15, %rax
    syscall
    hlt                             /* এখানে আসার কথা নয় */
    .size __restore_rt, . - __restore_rt

    .section .note.GNU-stack, "", @progbits
