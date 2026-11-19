/* rozOS — কম্পাইলার-নির্ভর attribute-গুলো এক জায়গায়।
 * GCC এবং TCC দুটোই __attribute__((...)) সাপোর্ট করে; ভবিষ্যতে অন্য কম্পাইলার
 * এলে শুধু এই ফাইল বদলালেই চলবে। কার্নেলের অন্য কোথাও সরাসরি __attribute__ লিখবে না। */
#ifndef ROZOS_COMPILER_H
#define ROZOS_COMPILER_H

#define ROZ_SECTION(s)   __attribute__((section(s)))
#define ROZ_ALIGNED(n)   __attribute__((aligned(n)))
#define ROZ_PACKED       __attribute__((packed))
#define ROZ_NORETURN     __attribute__((noreturn))
#define ROZ_UNUSED       __attribute__((unused))

/* "used": অপ্টিমাইজার যেন রেফারেন্সহীন ভেরিয়েবল (যেমন Limine request) ফেলে না দেয়। */
#if defined(__TINYC__)
#  define ROZ_USED
#else
#  define ROZ_USED       __attribute__((used))
#endif

/* printf-ধাঁচের ফাংশনের ফরম্যাট যাচাই (শুধু GCC-তে) */
#if defined(__GNUC__) && !defined(__TINYC__)
#  define ROZ_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define ROZ_PRINTF(fmt_idx, arg_idx)
#endif

#define ROZ_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
