/* rozOS — কার্নেলের ঐচ্ছিক C++ সাপোর্ট (kernel/cxx/)
 *
 * শুধু ENABLE_KCXX=1 হলে (হোস্টের x86_64-elf-g++ দিয়ে বিল্ড) এটা কম্পাইল হয় এবং
 * ROZOS_KCXX ম্যাক্রো সংজ্ঞায়িত থাকে। TCC দিয়ে বিল্ডের সময় পুরো অংশটা বাদ যায়,
 * তাই C কোড সবসময় #ifdef ROZOS_KCXX দিয়ে এগুলো ডাকবে।
 *
 * new/delete-এর জন্য heap দরকার। ধাপ ৩-এ kmalloc/kfree তৈরি হলে
 * kcxx_set_allocator(kmalloc, kfree) ডাকা হবে; তার আগে new ডাকলে kpanic হবে। */
#ifndef ROZOS_KCXX_H
#define ROZOS_KCXX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*kcxx_alloc_fn)(size_t size);
typedef void  (*kcxx_free_fn)(void *ptr);

/* .init_array-এর সব গ্লোবাল কনস্ট্রাক্টর চালায়; চালানো কনস্ট্রাক্টরের সংখ্যা ফেরত দেয় */
size_t kcxx_init(void);
void   kcxx_set_allocator(kcxx_alloc_fn alloc, kcxx_free_fn free_fn);
int    kcxx_has_allocator(void);
/* গ্লোবাল কনস্ট্রাক্টর ঠিকমতো চলেছে কিনা যাচাই (selftest.cpp) — সফল হলে 1 */
int    kcxx_selftest(void);

#ifdef __cplusplus
}
#endif

#endif
