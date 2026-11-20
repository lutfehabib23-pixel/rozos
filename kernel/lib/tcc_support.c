/* rozOS — TCC-বিল্ড কার্নেলের ছোট সহায়ক (ধাপ ১১)
 *
 * gcc বিল্ডে --gc-sections অব্যবহৃত কোড ফেলে দেয়, তাই এগুলো লাগে না। TCC-র লিংকারে
 * সেটা নেই — বন্ধ থাকা lwIP debug লাইনের উল্লেখ আর libtcc1-এর va_list.o-র abort()
 * রয়ে যায়, তাই এখানে সংজ্ঞা। gcc বিল্ডে এই ফাইল কিছুই তৈরি করে না। */

#ifdef __TINYC__

#include <rozos/panic.h>

void abort(void)
{
    kpanic("abort() called (TCC runtime)");
    for (;;) {
    }
}

/* lwIP-এর err.c আমরা নিইনি; LWIP_DEBUGF বন্ধ — কখনো ডাকা হয় না, শুধু লিংকের জন্য */
const char *lwip_strerr(signed char err)
{
    (void)err;
    return "lwIP error";
}

#else

typedef int rozos_tcc_support_unused;   /* ISO C: খালি translation unit নয় */

#endif
