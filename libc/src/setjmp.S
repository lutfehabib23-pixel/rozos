/* rozlibc — setjmp/longjmp (x86-64 SysV): rbx rbp r12 r13 r14 r15 rsp rip */

    .section .text
    .global setjmp
    .type setjmp, @function
setjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rdx           /* ফেরার পরের rsp */
    movq    %rdx, 48(%rdi)
    movq    (%rsp), %rdx            /* ফেরার ঠিকানা */
    movq    %rdx, 56(%rdi)
    xorl    %eax, %eax
    ret
    .size setjmp, . - setjmp

    .global longjmp
    .type longjmp, @function
longjmp:
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax                /* longjmp(env, 0) → setjmp 1 দেয় */
1:
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    jmpq    *56(%rdi)
    .size longjmp, . - longjmp

    .section .note.GNU-stack, "", @progbits
