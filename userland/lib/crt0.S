/* rozOS — ইউজার প্রোগ্রামের শুরু (ধাপ ৬; ধাপ ৭-এ rozlibc-র crt0 এর জায়গা নেবে)
 *
 * কার্নেল স্ট্যাক সাজিয়ে দেয় (SysV ABI): (%rsp) = argc, তারপর argv[], NULL, envp[], NULL, auxv।
 * main(argc, argv, envp) ডেকে তার ফলাফল দিয়ে exit। */

    .section .text
    .global _start
    .type _start, @function
_start:
    xorl    %ebp, %ebp              /* স্ট্যাক-ফ্রেম শৃঙ্খলের শেষ */
    movq    (%rsp), %rdi            /* argc */
    leaq    8(%rsp), %rsi           /* argv */
    leaq    8(%rsi,%rdi,8), %rdx    /* envp = argv + (argc + 1) ঘর */
    andq    $-16, %rsp
    call    main
    movl    %eax, %edi
    movl    $60, %eax               /* SYS_exit */
    syscall
    hlt                             /* এখানে আসার কথা নয় — এলে #GP, প্রসেস শেষ */
    .size _start, . - _start

    .section .note.GNU-stack, "", @progbits
