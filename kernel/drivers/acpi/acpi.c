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
#include <arch/x86_64/io.h>
#include <rozos/ps2.h>

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

/* ---------- ধাপ ১১খ: ACPI দিয়ে বিদ্যুৎ বন্ধ (S5) ----------
 * FADT ("FACP") থেকে PM1a/PM1b control register; DSDT-র AML-এ "_S5_" প্যাকেজ খুঁজে
 * SLP_TYP মান (পুরো AML interpreter ছাড়া প্রচলিত সরল পদ্ধতি)। তারপর
 * SLP_TYP << 10 | SLP_EN (bit 13) লেখা। ACPI mode বন্ধ থাকলে SMI_CMD দিয়ে চালু। */

static int s5_value(const uint8_t *p, const uint8_t *end, uint16_t *out)
{
    if (p >= end) return -1;
    if (*p == 0x0A && p + 1 < end) { *out = p[1]; return 2; }   /* BytePrefix */
    if (*p == 0x00) { *out = 0; return 1; }                     /* ZeroOp */
    if (*p == 0x01) { *out = 1; return 1; }                     /* OneOp */
    return -1;
}

int acpi_poweroff(void)
{
    const uint8_t *fadt = (const uint8_t *)acpi_find_table("FACP", 0);
    const struct acpi_sdt_header *dsdt;
    const uint8_t *p, *end;
    uint32_t pm1a, pm1b, smi_cmd, dsdt_phys;
    uint8_t acpi_enable;
    uint16_t typa = 0, typb = 0;
    int found = 0, i;

    if (!fadt || ((const struct acpi_sdt_header *)fadt)->length < 76) return -1;
    dsdt_phys = *(const uint32_t *)(fadt + 40);
    smi_cmd = *(const uint32_t *)(fadt + 48);
    acpi_enable = fadt[52];
    pm1a = *(const uint32_t *)(fadt + 64);
    pm1b = *(const uint32_t *)(fadt + 68);
    if (!pm1a) return -1;
    dsdt = map_table(dsdt_phys);
    if (dsdt) {
        p = (const uint8_t *)dsdt + sizeof(*dsdt);
        end = (const uint8_t *)dsdt + dsdt->length;
        for (; p + 8 < end; p++) {
            if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_' &&
                (p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\')) && p[4] == 0x12) {
                const uint8_t *q = p + 5;
                int n;
                q += ((*q & 0xC0) >> 6) + 1;                     /* PkgLength */
                q++;                                             /* NumElements */
                n = s5_value(q, end, &typa);
                if (n > 0 && s5_value(q + n, end, &typb) > 0) found = 1;
                break;
            }
        }
    }
    if (!found) {
        typa = typb = 0;                                         /* QEMU q35/piix-এ S5 = 0 */
    }
    if (smi_cmd && acpi_enable && !(inw((uint16_t)pm1a) & 1)) {
        outb((uint16_t)smi_cmd, acpi_enable);
        for (i = 0; i < 300 && !(inw((uint16_t)pm1a) & 1); i++) io_wait();
    }
    outw((uint16_t)pm1a, (uint16_t)((typa << 10) | (1 << 13)));
    if (pm1b) outw((uint16_t)pm1b, (uint16_t)((typb << 10) | (1 << 13)));
    return -1;                                                   /* এখানে এলে কাজ হয়নি */
}

/* ---------- ধাপ ২৪ক: রিস্টার্ট ----------
 * আগে শুধু 8042 (0xFE) দিয়ে রিস্টার্ট হতো — নতুন মাদারবোর্ডে (যেমন B550) সেটা কাজ করে না,
 * পিসি আটকে থাকে। এখন ক্রম: (১) FADT-এর ACPI reset register, (২) port 0xCF9 (চিপসেটের
 * reset control), (৩) 8042, (৪) triple fault (খালি IDT-তে interrupt)। প্রতিটার পরে একটু
 * অপেক্ষা, কাজ না হলে পরেরটা। */
static void reset_delay(void)
{
    int i;
    for (i = 0; i < 50000; i++) io_wait();            /* ~৫০ ms */
}

/* FADT flags বিট ১০ (RESET_REG_SUP): offset 116-এ Generic Address (১২ বাইট), 128-এ মান */
int acpi_reset(void)
{
    const uint8_t *fadt = (const uint8_t *)acpi_find_table("FACP", 0);
    uint32_t flags;
    uint8_t space, value;
    uint64_t addr;

    if (!fadt || ((const struct acpi_sdt_header *)fadt)->length < 129) return -1;
    flags = *(const uint32_t *)(fadt + 112);
    space = fadt[116];
    memcpy(&addr, fadt + 120, 8);
    value = fadt[128];
    if (!(flags & (1u << 10)) || !addr) return -1;
    if (space == 1) {                                  /* System I/O */
        outb((uint16_t)addr, value);
    } else if (space == 0) {                           /* System Memory */
        volatile uint8_t *m = (volatile uint8_t *)vmm_map_mmio(addr & ~0xFFFull, 0x1000);
        if (!m) return -1;
        m[addr & 0xFFF] = value;
    } else if (space == 2) {                           /* PCI config: bus 0, dev:func:offset */
        uint32_t dev = (uint32_t)((addr >> 32) & 0x1F);
        uint32_t fn = (uint32_t)((addr >> 16) & 0x7);
        uint32_t off = (uint32_t)(addr & 0xFF);
        outl(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | (off & 0xFC));
        outb((uint16_t)(0xCFC + (off & 3)), value);
    } else {
        return -1;
    }
    reset_delay();
    return -1;                                         /* এখানে এলে কাজ হয়নি */
}

void machine_restart(void)
{
    __asm__ volatile("cli");
    acpi_reset();
    /* 0xCF9: বিট ১ = system reset, বিট ২ = reset শুরু; বিট ৩ = full (cold) reset */
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    reset_delay();
    outb(0xCF9, 0x0E);
    reset_delay();
    if (ps2_present()) {
        ps2_reset_cpu();
        reset_delay();
    }
    {
        /* শেষ উপায়: শূন্য-দৈর্ঘ্যের IDT লোড করে interrupt → triple fault → CPU reset */
        struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = { 0, 0 };
        __asm__ volatile("lidt %0; int3" : : "m"(idtr));
    }
    for (;;) __asm__ volatile("hlt");
}
