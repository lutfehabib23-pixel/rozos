/* rozOS — কার্নেল heap (kmalloc/kfree), size-class পদ্ধতি
 *
 * ছোট অনুরোধ (≤ 2048 বাইট) ৮টা আকারের শ্রেণিতে যায়: 16, 32, 64, 128, 256, 512,
 * 1024, 2048। প্রতিটি শ্রেণির জন্য PMM থেকে 4 KiB পেজ নিয়ে সমান টুকরো করা হয়, আর
 * খালি টুকরোগুলো একটা free list-এ থাকে। বড় অনুরোধ সরাসরি পাশাপাশি পেজ হিসেবে।
 *
 * প্রতিটি পেজের শুরুতে 16 বাইটের হেডার থাকে, তাই kfree(ptr) শুধু
 * ptr & ~0xFFF দেখেই বুঝতে পারে কোন শ্রেণির বা বড় অ্যালোকেশন।
 * সব ফেরত দেওয়া ঠিকানা 16-byte aligned। */
#ifndef ROZOS_HEAP_H
#define ROZOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

#define HEAP_CLASS_COUNT 8
#define HEAP_SMALL_MAX   2048

struct heap_stats {
    uint64_t class_size[HEAP_CLASS_COUNT];
    uint64_t class_pages[HEAP_CLASS_COUNT];     /* শ্রেণিতে কতগুলো পেজ */
    uint64_t class_in_use[HEAP_CLASS_COUNT];    /* ব্যবহৃত টুকরো */
    uint64_t large_allocs;                      /* বর্তমানে চালু বড় অ্যালোকেশন */
    uint64_t large_pages;
    uint64_t total_allocs;                      /* বুট থেকে মোট kmalloc */
    uint64_t total_frees;
    uint64_t bad_frees;                         /* ভুল পয়েন্টার বা double free ধরা পড়েছে */
};

int   heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);                    /* শূন্য করা */
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
size_t kmalloc_usable_size(const void *ptr);   /* ptr-এ আসলে কত বাইট ব্যবহারযোগ্য */
void  heap_get_stats(struct heap_stats *out);

/* নিজেকে পরীক্ষা: বিভিন্ন আকারে অ্যালোকেট/লিখে/মিলিয়ে/free; সফল হলে 1 */
int   heap_selftest(void);

#endif
