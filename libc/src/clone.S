/* rozlibc — থ্রেড বানানো (ধাপ ১৬খ-২)
 *
 * long __roz_clone(int (*fn)(void *), void *stack, int flags, void *arg,
 *                  int *ptid, void *tls, int *ctid)
 * নতুন থ্রেড নতুন স্ট্যাকে শুরু হয় — C-র ফাংশন থেকে ফেরা যায় না, তাই fn আর arg আগেই
 * নতুন স্ট্যাকে রেখে দেওয়া; নতুন থ্রেড সেগুলো তুলে fn(arg) ডাকে, তারপর SYS_exit (60,
 * শুধু এই থ্রেড)। মূল থ্রেড clone-এর ফল (নতুন tid বা -errno) নিয়ে ফেরে। */

    .section .text
    .global __roz_clone
    .type __roz_clone, @function
__roz_clone:
    andq    $-16, %rsi
    subq    $16, %rsi
    movq    %rdi, 0(%rsi)           /* fn */
    movq    %rcx, 8(%rsi)           /* arg */
    movl    %edx, %edi              /* flags */
    movq    %r8, %rdx               /* parent_tid */
    movq    8(%rsp), %r10           /* child_tid (সপ্তম আর্গুমেন্ট, স্ট্যাকে) */
    movq    %r9, %r8                /* tls */
    movl    $56, %eax               /* clone */
    syscall
    testq   %rax, %rax
    jnz     1f
    /* নতুন থ্রেড: rsp = নতুন স্ট্যাক (fn, arg) */
    xorl    %ebp, %ebp
    popq    %rax                    /* fn */
    popq    %rdi                    /* arg */
    callq   *%rax
    movl    %eax, %edi
    movl    $60, %eax               /* exit: শুধু এই থ্রেড */
    syscall
    hlt
1:
    ret
    .size __roz_clone, . - __roz_clone

    .section .note.GNU-stack, "", @progbits
