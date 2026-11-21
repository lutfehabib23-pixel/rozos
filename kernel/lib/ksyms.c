/* rozOS — মডিউলের জন্য রপ্তানি করা কার্নেল প্রতীক (ধাপ ১৫ক)
 * মডিউল (insmod) শুধু এই তালিকার নামগুলো ব্যবহার করতে পারে। নতুন কিছু দরকার হলে
 * এখানে KSYM(নাম) যোগ করে কার্নেল আবার বিল্ড করুন। `lsmod -k` পুরো তালিকা দেখায়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/module.h>
#include <rozos/printf.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/devfs.h>
#include <rozos/timer.h>
#include <rozos/clock.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/pci.h>
#include <rozos/block.h>
#include <rozos/sched.h>
#include "arch/x86_64/pic.h"

#define KSYM(f) { #f, (uint64_t)(uintptr_t)&f }

const struct ksym ksym_table[] = {
    /* লেখা */
    KSYM(kprintf), KSYM(ksnprintf), KSYM(kvsnprintf), KSYM(kvprintf),
    /* মেমোরি */
    KSYM(kmalloc), KSYM(kzalloc), KSYM(kcalloc), KSYM(krealloc), KSYM(kfree),
    KSYM(pmm_alloc_page), KSYM(pmm_alloc_zeroed_page), KSYM(pmm_alloc_contiguous),
    KSYM(pmm_free_page), KSYM(pmm_free_pages), KSYM(pmm_phys_to_virt),
    KSYM(vmm_map_mmio), KSYM(vmm_kernel_space), KSYM(vmm_virt_to_phys),
    /* string */
    KSYM(memcpy), KSYM(memmove), KSYM(memset), KSYM(memcmp), KSYM(strlen), KSYM(strnlen),
    KSYM(strcmp), KSYM(strncmp), KSYM(strcpy), KSYM(strncpy), KSYM(strchr),
    /* ডিভাইস ফাইল (/dev) */
    KSYM(devfs_register), KSYM(devfs_unregister), KSYM(devfs_set_poll),
    /* সময় */
    KSYM(timer_ticks), KSYM(timer_uptime_ms), KSYM(timer_sleep_ms), KSYM(timer_frequency),
    KSYM(timer_add_tick_hook), KSYM(timer_remove_tick_hook), KSYM(clock_unix_time),
    /* PCI */
    KSYM(pci_device_count), KSYM(pci_device_at), KSYM(pci_find_class), KSYM(pci_find_id),
    KSYM(pci_read32), KSYM(pci_read16), KSYM(pci_read8), KSYM(pci_write32), KSYM(pci_write16),
    KSYM(pci_enable_device), KSYM(pci_class_name),
    /* ইন্টারাপ্ট */
    KSYM(irq_register), KSYM(pic_mask), KSYM(pic_unmask),
    /* ব্লক ডিভাইস (ডিস্ক ড্রাইভার) */
    KSYM(block_register), KSYM(block_register_partition), KSYM(partition_scan), KSYM(block_find),
    KSYM(block_read), KSYM(block_write), KSYM(block_device_count), KSYM(block_device_at),
    /* শিডিউলার */
    KSYM(sched_yield), KSYM(kmutex_lock), KSYM(kmutex_unlock),
};

const size_t ksym_count = sizeof(ksym_table) / sizeof(ksym_table[0]);
