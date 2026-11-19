/* rozOS — কার্নেলের প্রথম নির্দেশ (GAS/AT&T সিনট্যাক্স)
 *
 * Limine আমাদের এখানে আনে 64-bit long mode-এ, পেজিং চালু অবস্থায়, ইন্টারাপ্ট বন্ধ
 * (IF=0) রেখে। Limine-এর দেওয়া স্ট্যাক bootloader-reclaimable মেমোরিতে থাকে, যা পরে
 * PMM ফেরত নিতে পারে — তাই প্রথম কাজই হলো কার্নেলের নিজের স্ট্যাকে চলে যাওয়া। */

    .section .bss
    .align 16
    .global kernel_stack_bottom
kernel_stack_bottom:
    .skip 65536                 /* 64 KiB কার্নেল স্ট্যাক */
    .global kernel_stack_top
kernel_stack_top:

    .section .text
    .global _start
    .type _start, @function
_start:
    cli
    leaq    kernel_stack_top(%rip), %rsp    /* স্ট্যাক 16-byte aligned */
    xorq    %rbp, %rbp                      /* স্ট্যাক-ট্রেসের শেষ চিহ্ন */
    call    kmain                           /* kmain কখনো ফেরার কথা নয় */

1:  cli
    hlt
    jmp     1b
    .size _start, . - _start

    /* স্ট্যাক executable নয় — লিংকারের সতর্কবার্তা এড়াতে */
    .section .note.GNU-stack, "", @progbits
