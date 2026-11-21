/* rozOS — লোড করা যায় এমন কার্নেল মডিউল (ধাপ ১৫ক)
 *
 * মডিউল = সাধারণ ELF relocatable ফাইল (.ko = gcc/tcc -c-এর .o)। কার্নেল চলার মধ্যেই
 * insmod দিয়ে ঢোকানো, rmmod দিয়ে বের করা, lsmod দিয়ে তালিকা। মডিউলের ভেতরে:
 *
 *   #include <rozos/module.h>
 *   #include <rozos/printf.h>
 *   ROZMOD_INFO("hello", "says hello");          // নাম আর এক লাইনের বিবরণ (ঐচ্ছিক)
 *   int rozmod_init(void) { kprintf("hello!\n"); return 0; }   // <0 = লোড বাতিল
 *   void rozmod_exit(void) { kprintf("bye\n"); }               // ঐচ্ছিক
 *
 * মডিউল শুধু কার্নেলের "রপ্তানি করা" ফাংশন/চলক ব্যবহার করতে পারে (kernel/lib/ksyms.c-র
 * তালিকা; `lsmod -k` দেখায়)। জায়গা: 0xffffffffc0000000 থেকে, কার্নেলের ২ GiB-এর ভেতরে —
 * তাই -mcmodel=kernel (gcc) বা tcc-র সাধারণ কোডই চলে। */
#ifndef ROZOS_MODULE_H
#define ROZOS_MODULE_H

#include <stdint.h>
#include <stddef.h>

#define ROZMOD_INFO(name, desc) \
    const char rozmod_name[] = name; \
    const char rozmod_desc[] = desc

struct ksym {
    const char *name;
    uint64_t addr;
};
extern const struct ksym ksym_table[];
extern const size_t ksym_count;
uint64_t ksym_lookup(const char *name);          /* 0 = নেই */

/* image = পুরো .ko ফাইল (কার্নেল মেমোরিতে কপি); name = নাম (মডিউলে ROZMOD_INFO থাকলে সেটা);
 * ভুল হলে err-এ কারণ (মানুষের জন্য) */
int module_load(const void *image, size_t len, const char *name, char *err, size_t errlen);
int module_unload(const char *name);
/* "নাম আকার(বাইট) বিবরণ\n" সারি; ফেরত: লেখা বাইট */
int64_t module_list(char *buf, size_t len, int kernel_symbols);

#endif
