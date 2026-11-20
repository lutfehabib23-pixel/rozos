/* rozOS — কার্নেল ও ইউজার মেমোরির মাঝে নিরাপদে কপি (ধাপ ৬)
 *
 * চলমান প্রসেসের ঠিকানা-স্থান CR3-এ থাকে, তাই কার্নেল সরাসরি ইউজার ঠিকানা পড়তে
 * পারে — কিন্তু আগে যাচাই: পুরো পরিসর ইউজার-অর্ধেকে, প্রতিটি পেজ ম্যাপ করা ও
 * USER বিট সহ (লেখার জন্য WRITE সহ)। স্ট্যাক এলাকার না-আসা পেজ এখানেই আনা হয়।
 * সব ফাংশন সফল হলে 0, নইলে -EFAULT। */
#ifndef ROZOS_UACCESS_H
#define ROZOS_UACCESS_H

#include <stdint.h>
#include <stddef.h>

int user_range_ok(uint64_t addr, size_t len, int write);
int copy_from_user(void *dst, const void *usrc, size_t len);
int copy_to_user(void *udst, const void *src, size_t len);
/* NUL পর্যন্ত (সহ) কপি; max-এর মধ্যে NUL না পেলে -ENAMETOOLONG; ফেরত দৈর্ঘ্য */
int64_t copy_string_from_user(char *dst, const char *usrc, size_t max);

#endif
