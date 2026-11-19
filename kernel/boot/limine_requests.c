/* rozOS — Limine request-সমূহ
 *
 * PROTOCOL.md অনুযায়ী:
 *  - request-গুলো 8-byte aligned হতে হবে এবং start/end marker-এর মাঝে থাকতে হবে
 *    (base revision 2+ এ marker মানা বাধ্যতামূলক)।
 *  - একই request দুবার থাকলে বুটলোডার বুট করবে না।
 *  - response ফিল্ড বুটলোডার লেখে, তাই request-গুলো volatile — নইলে কম্পাইলার
 *    ধরে নিতে পারে response সবসময় NULL।
 *  - linker.ld তিনটে section (.limine_requests_start / .limine_requests /
 *    .limine_requests_end) এই ক্রমে পাশাপাশি রাখে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>
#include <rozos/boot.h>

/* ---------- শুরুর marker ---------- */
ROZ_USED ROZ_SECTION(".limine_requests_start") ROZ_ALIGNED(8)
static volatile uint64_t limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

/* ---------- base revision ---------- */
ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(ROZOS_LIMINE_BASE_REVISION);

/* ---------- request-সমূহ ---------- */
ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_bootloader_info_request limine_bootloader_info_req = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = NULL
};

ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_firmware_type_request limine_firmware_type_req = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0, .response = NULL
};

ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_executable_cmdline_request limine_cmdline_req = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0, .response = NULL
};

/* কার্নেল নিজের স্ট্যাক ব্যবহার করে (entry.S), তবে পরে MP (একাধিক CPU) চালু হলে
 * AP-গুলো বুটলোডারের স্ট্যাক পায় — তার আকার এখান থেকে নির্ধারিত হয়। */
ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_stack_size_request limine_stack_size_req = {
    .id = LIMINE_STACK_SIZE_REQUEST_ID, .revision = 0, .response = NULL,
    .stack_size = 65536
};

ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_hhdm_request limine_hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = NULL
};

ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_framebuffer_request limine_framebuffer_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0, .response = NULL
};

/* বুটলোডারের নিজের ফন্ট ও রঙ — framebuffer কনসোল এখান থেকে ফন্ট নেয় */
ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_flanterm_fb_init_params_request limine_flanterm_req = {
    .id = LIMINE_FLANTERM_FB_INIT_PARAMS_REQUEST_ID, .revision = 0, .response = NULL
};

/* ৪-লেভেল পেজিং (PML4) বাধ্যতামূলক — আমাদের VMM PML4-ভিত্তিক।
 * request revision 1: mode/max_mode/min_mode তিনটেই 4LVL। */
ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_paging_mode_request limine_paging_mode_req = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID, .revision = 1, .response = NULL,
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL
};

ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_memmap_request limine_memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = NULL
};

ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_executable_address_request limine_exec_addr_req = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = NULL
};

/* ধাপ ৪-এ initrd এই request দিয়ে আসবে; এখন শুধু মডিউলের সংখ্যা দেখানো হয় */
ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_module_request limine_module_req = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0, .response = NULL
};

/* ধাপ ২-এ APIC চালু করতে ACPI (MADT) লাগবে */
ROZ_USED ROZ_SECTION(".limine_requests") ROZ_ALIGNED(8)
volatile struct limine_rsdp_request limine_rsdp_req = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0, .response = NULL
};

/* ---------- শেষের marker ---------- */
ROZ_USED ROZ_SECTION(".limine_requests_end") ROZ_ALIGNED(8)
static volatile uint64_t limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

/* ---------- সহায়ক ফাংশন ---------- */

int boot_base_revision(int *ok)
{
    /* limine.h-এর macro-গুলো volatile পয়েন্টার নেয় না, তাই সরাসরি পড়া হয় */
    uint64_t requested_slot = limine_base_revision[2];
    uint64_t loaded_slot    = limine_base_revision[1];
    int used = -1;

    if (requested_slot == 0) {
        /* অনুরোধ করা revision সাপোর্টেড — বুটলোডার [2]-কে 0 করেছে */
        used = ROZOS_LIMINE_BASE_REVISION;
    }
    /* revision 3+ বুটলোডার [1]-এ আসল ব্যবহৃত revision লেখে */
    if (loaded_slot != 0x6a7b384944536bdcULL) {
        used = (int)loaded_slot;
    }

    if (ok) {
        *ok = (used >= ROZOS_LIMINE_MIN_REVISION);
    }
    return used;
}

uint64_t boot_hhdm_offset(void)
{
    struct limine_hhdm_response *r = limine_hhdm_req.response;
    return r ? r->offset : 0;
}

const char *boot_memmap_type_name(uint64_t type)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "Usable";
    case LIMINE_MEMMAP_RESERVED:               return "Reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "Bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "Bootloader reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "Kernel and modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "Framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "Reserved (mapped)";
    default:                                   return "Unknown";
    }
}

const char *boot_firmware_name(uint64_t type)
{
    switch (type) {
    case LIMINE_FIRMWARE_TYPE_X86BIOS: return "BIOS";
    case LIMINE_FIRMWARE_TYPE_EFI32:   return "UEFI (32-bit)";
    case LIMINE_FIRMWARE_TYPE_EFI64:   return "UEFI (64-bit)";
    case LIMINE_FIRMWARE_TYPE_SBI:     return "SBI";
    default:                           return "Unknown";
    }
}
