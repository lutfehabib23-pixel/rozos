/* rozOS — .S ফাইলের জন্য assembler-ভেদের সমাধান (ধাপ ১১)
 *
 * TCC-র নিজের assembler (rozOS-এর ভেতরে kernel বিল্ডে) কিছু নির্দেশ চেনে না। এখানে
 * সেগুলোর ম্যাক্রো: GNU as-এ আসল নাম, TCC-তে একই মেশিন-কোডের বাইট। দুই পথে
 * তৈরি নির্দেশ হুবহু এক। */
#ifndef ROZOS_ASM_COMPAT_H
#define ROZOS_ASM_COMPAT_H

#ifdef __TINYC__
#define SWAPGS  .byte 0x0f, 0x01, 0xf8      /* 0F 01 F8 */
#define LRETQ   .byte 0x48, 0xcb            /* REX.W CB  (64-bit far return) */
#else
#define SWAPGS  swapgs
#define LRETQ   lretq
#endif

#endif
