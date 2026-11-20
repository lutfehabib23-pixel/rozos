/* rozOS — TCC-র কনফিগারেশন (TCC-র configure স্ক্রিপ্টের বদলে; Makefile এটা ব্যবহার করে)
 *
 * TCC চলবে rozOS-এর ভেতরে, বানাবে rozOS-এর প্রোগ্রাম: স্ট্যাটিক x86-64 ELF,
 * rozlibc দিয়ে (/usr/include, /usr/lib/libc.a, /usr/lib/crt1.o)। */
#define TCC_VERSION "0.9.28rc"
#define CONFIG_TCCDIR "/usr/lib/tcc"                          /* {B}: libtcc1.a, include/ */
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:/usr/include"
#define CONFIG_TCC_LIBPATHS "/usr/lib:{B}"
#define CONFIG_TCC_CRTPREFIX "/usr/lib"
#define CONFIG_TCC_ELFINTERP "-"                              /* dynamic linker নেই */
#define CONFIG_TCC_STATIC 1                                   /* dlopen নয় */
#define CONFIG_TCC_ROZOS 1                                    /* rozos.patch: সবসময় -static */
#define CONFIG_TCC_BACKTRACE 0                                /* signal/ucontext এখনো নেই */
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_PREDEFS 0                                  /* tccdefs.h চলার সময় include থেকে পড়ে */
#define CONFIG_TCC_SEMLOCK 0                                  /* থ্রেড নেই */
#define TCC_TARGET_X86_64 1
#define ONE_SOURCE 1
