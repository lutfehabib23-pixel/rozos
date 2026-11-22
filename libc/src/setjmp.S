/* rozlibc — setjmp/longjmp (x86-64 SysV): rbx rbp r12 r13 r14 r15 rsp rip */

    .section .text
    .global setjmp
    .type setjmp, @function
    /* ধাপ ১৬ক: _setjmp একই (setjmp signal mask রাখে না — glibc-র মতো) */
    .global _setjmp
    .type _setjmp, @function
setjmp:
_setjmp:
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
    .global _longjmp
    .type _longjmp, @function
longjmp:
_longjmp:
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

/* ধাপ ১৬খ-১: sigsetjmp(env, savemask) — env[8] = savemask, env[9] = তখনকার signal mask
 * (rt_sigprocmask(SIG_BLOCK, NULL, &env[9])); siglongjmp সেটা ফিরিয়ে তারপর longjmp। */
    .global sigsetjmp
    .type sigsetjmp, @function
sigsetjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rdx
    movq    %rdx, 48(%rdi)
    movq    (%rsp), %rdx
    movq    %rdx, 56(%rdi)
    movslq  %esi, %rsi
    movq    %rsi, 64(%rdi)
    testq   %rsi, %rsi
    jz      1f
    pushq   %rdi
    leaq    72(%rdi), %rdx          /* oldset */
    xorl    %esi, %esi              /* set = NULL */
    xorl    %edi, %edi              /* how = SIG_BLOCK */
    movq    $8, %r10
    movq    $14, %rax               /* rt_sigprocmask */
    syscall
    popq    %rdi
1:
    xorl    %eax, %eax
    ret
    .size sigsetjmp, . - sigsetjmp

    .global siglongjmp
    .type siglongjmp, @function
siglongjmp:
    cmpq    $0, 64(%rdi)
    je      longjmp
    pushq   %rdi
    pushq   %rsi
    leaq    72(%rdi), %rsi          /* set = সংরক্ষিত mask */
    movl    $2, %edi                /* SIG_SETMASK */
    xorl    %edx, %edx
    movq    $8, %r10
    movq    $14, %rax
    syscall
    popq    %rsi
    popq    %rdi
    jmp     longjmp
    .size siglongjmp, . - siglongjmp

    .section .note.GNU-stack, "", @progbits
