/* rozOS — কার্নেলের linker script
 *
 * PROTOCOL.md ("Memory Layout at Entry") অনুযায়ী কার্নেল সেগমেন্ট 0xffffffff80000000
 * বা তার উপরে থাকতে হবে, এবং ELF-এর PT_LOAD permission (R/W/X) মেনেই Limine ম্যাপ করে।
 * তাই প্রতিটি অংশ আলাদা PT_LOAD সেগমেন্টে, আলাদা permission-এ, পেজ-সীমায় সাজানো।
 * FLAGS: 1 = X, 2 = W, 4 = R */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    limine_requests PT_LOAD FLAGS(6);   /* R W  — বুটলোডার response লেখে */
    text            PT_LOAD FLAGS(5);   /* R X */
    rodata          PT_LOAD FLAGS(4);   /* R   */
    data            PT_LOAD FLAGS(6);   /* R W */
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    /* Limine request — start marker, request-গুলো, end marker এই ক্রমে */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
    } :rodata

    /* C++ গ্লোবাল কনস্ট্রাক্টরের তালিকা — kcxx_init() চালায় */
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
        __rodata_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        __data_start = .;
        *(.data .data.*)
    } :data

    /* .bss শেষে রাখা হয় যাতে ফাইলে জায়গা না নেয় */
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    __kernel_end = .;

    /* কার্নেলে প্রয়োজন নেই */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
