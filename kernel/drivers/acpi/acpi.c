/* rozOS — ACPI টেবিল আবিষ্কার
 *
 * PROTOCOL.md (base revision 4+): RSDP, RSDT/XSDT ও তাদের নির্দেশিত সব টেবিল
 * ACPI reclaimable / ACPI NVS / reserved-mapped অঞ্চলে থাকে — এগুলো VMM-এর HHDM-এ
 * ম্যাপ করা। তবু সাবধানতার জন্য প্রতিটি টেবিল পড়ার আগে ম্যাপিং নিশ্চিত করা হয়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/acpi.h>
#include <rozos/bootinfo.h>
#include <rozos/vmm.h>
#include <rozos/string.h>

#define ACPI_MAX_TABLES 64

struct rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* revision >= 2 */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} ROZ_PACKED;

static const struct acpi_sdt_header *tables[ACPI_MAX_TABLES];
static size_t table_count;
static uint8_t rsdp_revision;
static char oem_id[7];
static int available;

static int checksum_ok(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    while (len--) {
        sum = (uint8_t)(sum + *p++);
    }
    return sum == 0;
}

/* ফিজিক্যাল ঠিকানার টেবিল পড়ার উপযোগী করা: হেডার ম্যাপ → দৈর্ঘ্য জেনে পুরোটা */
static const struct acpi_sdt_header *map_table(uint64_t phys)
{
    const struct acpi_sdt_header *h;
    if (!phys || !vmm_map_mmio(phys, sizeof(struct acpi_sdt_header))) {
        return NULL;
    }
    h = (const struct acpi_sdt_header *)(uintptr_t)(phys + bootinfo()->hhdm_offset);
    if (h->length < sizeof(*h) || !vmm_map_mmio(phys, h->length)) {
        return NULL;
    }
    return checksum_ok(h, h->length) ? h : NULL;
}

/* vmm_map_mmio ইতিমধ্যে HHDM-এ ম্যাপ করা (cacheable) পেজ বদলায় না — শুধু যেগুলো নেই
 * সেগুলো uncached হিসেবে যোগ করে, যা টেবিল পড়ার জন্য নিরাপদ। */

int acpi_init(void)
{
    const struct bootinfo *bi = bootinfo();
    const struct rsdp *r;
    const struct acpi_sdt_header *root;
    size_t entries, i;
    int use_xsdt;

    available = 0;
    table_count = 0;
    if (!bi->rsdp_phys || !vmm_map_mmio(bi->rsdp_phys, sizeof(struct rsdp))) {
        return 0;
    }
    r = (const struct rsdp *)(uintptr_t)(bi->rsdp_phys + bi->hhdm_offset);
    if (memcmp(r->signature, "RSD PTR ", 8) != 0 || !checksum_ok(r, 20)) {
        return 0;
    }
    rsdp_revision = r->revision;
    memcpy(oem_id, r->oem_id, 6);
    oem_id[6] = '\0';

    use_xsdt = (r->revision >= 2 && r->xsdt_address != 0);
    root = map_table(use_xsdt ? r->xsdt_address : r->rsdt_address);
    if (!root) {
        return 0;
    }
    tables[table_count++] = root;

    entries = (root->length - sizeof(*root)) / (use_xsdt ? 8 : 4);
    for (i = 0; i < entries && table_count < ACPI_MAX_TABLES; i++) {
        const uint8_t *base = (const uint8_t *)root + sizeof(*root);
        uint64_t phys;
        const struct acpi_sdt_header *t;
        if (use_xsdt) {
            memcpy(&phys, base + i * 8, 8);           /* unaligned হতে পারে */
        } else {
            uint32_t p32;
            memcpy(&p32, base + i * 4, 4);
            phys = p32;
        }
        t = map_table(phys);
        if (t) {
            tables[table_count++] = t;
        }
    }
    available = 1;
    return 1;
}

int acpi_available(void)          { return available; }
uint8_t acpi_revision(void)       { return rsdp_revision; }
const char *acpi_oem_id(void)     { return oem_id; }
size_t acpi_table_count(void)     { return table_count; }

const struct acpi_sdt_header *acpi_table(size_t index)
{
    return index < table_count ? tables[index] : NULL;
}

const struct acpi_sdt_header *acpi_find_table(const char *signature, size_t n)
{
    size_t i;
    for (i = 0; i < table_count; i++) {
        if (memcmp(tables[i]->signature, signature, 4) == 0) {
            if (n == 0) {
                return tables[i];
            }
            n--;
        }
    }
    return NULL;
}
