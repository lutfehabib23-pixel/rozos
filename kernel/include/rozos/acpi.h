/* rozOS — ACPI টেবিল খোঁজা (RSDP → XSDT/RSDT → বাকি টেবিল)
 *
 * এখন শুধু টেবিল খোঁজা ও যাচাই (checksum)। MADT পার্স করে apic.c।
 * AML/DSDT ইন্টারপ্রেটার (পাওয়ার-অফ, ঘুম ইত্যাদি) এই প্রজেক্টের পরের ধাপের বিষয়। */
#ifndef ROZOS_ACPI_H
#define ROZOS_ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/compiler.h>

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} ROZ_PACKED;

/* bootinfo-র RSDP থেকে শুরু; সফল হলে 1 (ACPI না থাকলে 0) */
int acpi_init(void);
int acpi_available(void);
uint8_t acpi_revision(void);              /* RSDP revision: 0 = ACPI 1.0, 2+ = XSDT */
const char *acpi_oem_id(void);

size_t acpi_table_count(void);
const struct acpi_sdt_header *acpi_table(size_t index);
/* signature (৪ অক্ষর, যেমন "APIC") অনুযায়ী n-তম টেবিল; না থাকলে NULL */
const struct acpi_sdt_header *acpi_find_table(const char *signature, size_t n);
/* ধাপ ১১খ: ACPI S5 দিয়ে বিদ্যুৎ বন্ধ — সফল হলে ফেরে না, না পারলে -1 */
int acpi_poweroff(void);
/* ধাপ ২৪ক: ACPI reset register (না থাকলে/কাজ না হলে -1) আর পুরো রিস্টার্ট-ক্রম
 * (ACPI → 0xCF9 → 8042 → triple fault) — machine_restart ফেরে না */
int acpi_reset(void);
void machine_restart(void);

#endif
