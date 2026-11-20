/* rozOS — বুটলোডার (Limine) থেকে পাওয়া তথ্যের ইন্টারফেস।
 * কার্নেলের বাকি অংশ সরাসরি limine.h ব্যবহার না করে এই হেডারের মাধ্যমে তথ্য নেবে,
 * যাতে পরে নিজস্ব বুটলোডার যোগ হলে শুধু boot/ ফোল্ডার বদলাতে হয়। */
#ifndef ROZOS_BOOT_H
#define ROZOS_BOOT_H

#include <stdint.h>
#include <boot/limine.h>

/* কার্নেল যে base revision চায় (PROTOCOL.md: 0-5 deprecated, 6 সর্বশেষ) */
#define ROZOS_LIMINE_BASE_REVISION   6
/* x86-64-এর জন্য revision 5 ও 6-এর মধ্যে কোনো পার্থক্য নেই (6-এর সব পরিবর্তন
 * aarch64/riscv64/loongarch64-এর), তাই 5 দিয়ে লোড হলেও কার্নেল নিরাপদে চলবে। */
#define ROZOS_LIMINE_MIN_REVISION    5

/* limine_requests.c-তে সংজ্ঞায়িত request-গুলো */
extern volatile uint64_t limine_base_revision[3];
extern volatile struct limine_bootloader_info_request     limine_bootloader_info_req;
extern volatile struct limine_firmware_type_request       limine_firmware_type_req;
extern volatile struct limine_executable_cmdline_request  limine_cmdline_req;
extern volatile struct limine_stack_size_request          limine_stack_size_req;
extern volatile struct limine_hhdm_request                limine_hhdm_req;
extern volatile struct limine_framebuffer_request         limine_framebuffer_req;
extern volatile struct limine_flanterm_fb_init_params_request limine_flanterm_req;
extern volatile struct limine_paging_mode_request         limine_paging_mode_req;
extern volatile struct limine_memmap_request              limine_memmap_req;
extern volatile struct limine_executable_address_request  limine_exec_addr_req;
extern volatile struct limine_module_request              limine_module_req;
extern volatile struct limine_rsdp_request                limine_rsdp_req;
extern volatile struct limine_date_at_boot_request        limine_date_req;

/* বুটলোডার কোন base revision দিয়ে কার্নেল লোড করেছে তা যাচাই করে।
 * ফেরত দেয়: ব্যবহৃত revision (>= 0), অথবা -1 যদি নির্ণয় করা না যায়।
 * *ok = 1 হলে কার্নেল নিরাপদে চলতে পারে। */
int boot_base_revision(int *ok);

/* HHDM offset (ফিজিক্যাল → ভার্চুয়াল); response না থাকলে 0 */
uint64_t boot_hhdm_offset(void);

/* memmap entry type-এর মানুষের-পড়ার-যোগ্য নাম */
const char *boot_memmap_type_name(uint64_t type);

/* firmware type-এর নাম */
const char *boot_firmware_name(uint64_t type);

#endif
