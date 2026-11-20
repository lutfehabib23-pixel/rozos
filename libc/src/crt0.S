/* rozlibc — প্রোগ্রামের শুরু (_start)
 *
 * কার্নেল স্ট্যাক সাজায় (SysV ABI): (%rsp) = argc, তারপর argv[], NULL, envp[], NULL, auxv।
 * __libc_start(argc, argv, envp) libc তৈরি করে main ডাকে, তারপর exit()। */

    .section .text
    .global _start
    .type _start, @function
_start:
    xorl    %ebp, %ebp              /* স্ট্যাক-ফ্রেম শৃঙ্খলের শেষ */
    movq    (%rsp), %rdi            /* argc */
    leaq    8(%rsp), %rsi           /* argv */
    leaq    8(%rsi,%rdi,8), %rdx    /* envp = argv + (argc + 1) ঘর */
    andq    $-16, %rsp
    call    __libc_start            /* ফেরে না */
    hlt
    .size _start, . - _start

    .section .note.GNU-stack, "", @progbits
