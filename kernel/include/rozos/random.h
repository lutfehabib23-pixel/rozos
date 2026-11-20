/* rozOS — কার্নেলের র‍্যান্ডম সংখ্যা (ধাপ ১০: TLS-এর জন্য)
 *
 * ChaCha20 দিয়ে CSPRNG, "fast key erasure": প্রতিবার আউটপুটের পরে নতুন key।
 * entropy আসে: RDSEED/RDRAND (CPU-তে থাকলে), বুটের সময়, প্রতিটি টাইমার টিকের TSC,
 * কিবোর্ড ও নেটওয়ার্ক প্যাকেট আসার মুহূর্ত। /dev/random, /dev/urandom ও getrandom()।
 * সীমা: RDRAND ছাড়া (QEMU-র ডিফল্ট CPU) entropy মূলত সময়ের jitter থেকে — শেখা ও
 * পরীক্ষার জন্য যথেষ্ট, আসল নিরাপত্তার জন্য দুর্বল হতে পারে। */
#ifndef ROZOS_RANDOM_H
#define ROZOS_RANDOM_H

#include <stdint.h>
#include <stddef.h>

void random_init(void);
void random_add_entropy(uint64_t value);     /* ইন্টারাপ্ট থেকেও ডাকা যায় (দ্রুত) */
void random_get(void *buf, size_t len);
int  random_has_rdrand(void);

#endif
