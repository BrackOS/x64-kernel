#include "acpi.h"
#include "multiboot.h"
#include "vmm.h"
#include "string.h"

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;

#define PHYS_LIMIT 0x100000000ULL      /* the kernel identity-maps the first 4 GB */

static int checksum_ok(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

static const sdt_header_t *map_table(uint64_t phys) {
    if (phys == 0 || phys + sizeof(sdt_header_t) > PHYS_LIMIT) return NULL;
    const sdt_header_t *h = (const sdt_header_t *)PHYS_TO_VIRT(phys);
    if (h->length < sizeof(sdt_header_t) || phys + h->length > PHYS_LIMIT) return NULL;
    if (!checksum_ok(h, h->length)) return NULL;
    return h;
}

/* GRUB hands us a copy of the RSDP; otherwise search the BIOS area */
static const rsdp_t *find_rsdp(void) {
    const uint8_t *tag = (const uint8_t *)multiboot_find_tag(MB_TAG_ACPI_NEW);
    if (!tag) tag = (const uint8_t *)multiboot_find_tag(MB_TAG_ACPI_OLD);
    if (tag) {
        const rsdp_t *r = (const rsdp_t *)(tag + 8);       /* after the tag's type and size */
        if (memcmp(r->signature, "RSD PTR ", 8) == 0 && checksum_ok(r, 20)) return r;
    }
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        const rsdp_t *r = (const rsdp_t *)PHYS_TO_VIRT(a);
        if (memcmp(r->signature, "RSD PTR ", 8) == 0 && checksum_ok(r, 20)) return r;
    }
    return NULL;
}

int acpi_init(acpi_info_t *info, const char **why) {
    memset(info, 0, sizeof(*info));
    const char *dummy;
    if (!why) why = &dummy;

    const rsdp_t *rsdp = find_rsdp();
    if (!rsdp) { *why = "no ACPI RSDP"; return -1; }

    /* Collect the table pointers: XSDT (64-bit) if present, else RSDT (32-bit) */
    const sdt_header_t *root = NULL;
    int wide = 0;
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        root = map_table(rsdp->xsdt_addr);
        wide = (root != NULL);
    }
    if (!root) root = map_table(rsdp->rsdt_addr);
    if (!root) { *why = "bad RSDT/XSDT"; return -1; }

    size_t entry_size = wide ? 8 : 4;
    size_t count = (root->length - sizeof(sdt_header_t)) / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(sdt_header_t);

    const sdt_header_t *madt = NULL;
    for (size_t i = 0; i < count; i++) {
        uint64_t p = 0;
        memcpy(&p, entries + i * entry_size, entry_size);   /* little endian: low bytes first */
        const sdt_header_t *h = map_table(p);
        if (h && memcmp(h->signature, "APIC", 4) == 0) { madt = h; break; }
    }
    if (!madt) { *why = "no MADT"; return -1; }
    if (madt->length < sizeof(sdt_header_t) + 8) { *why = "short MADT"; return -1; }

    const uint8_t *m = (const uint8_t *)madt;
    uint32_t lapic, flags;
    memcpy(&lapic, m + 36, 4);
    memcpy(&flags, m + 40, 4);
    info->lapic_addr = lapic;
    info->pcat_compat = flags & 1;

    for (uint32_t off = 44; off + 2 <= madt->length; ) {
        uint8_t type = m[off], len = m[off + 1];
        if (len < 2 || off + len > madt->length) break;
        if (type == 0 && len >= 8) {                         /* processor local APIC */
            if (m[off + 4] & 1) info->cpu_count++;           /* enabled */
        } else if (type == 1 && len >= 12) {                 /* I/O APIC */
            if (info->ioapic_count < ACPI_MAX_IOAPICS) {
                int k = info->ioapic_count++;
                info->ioapic[k].id = m[off + 2];
                memcpy(&info->ioapic[k].addr, m + off + 4, 4);
                memcpy(&info->ioapic[k].gsi_base, m + off + 8, 4);
            }
        } else if (type == 2 && len >= 10) {                 /* interrupt source override */
            if (info->iso_count < ACPI_MAX_ISO) {
                int k = info->iso_count++;
                info->iso[k].source = m[off + 3];
                memcpy(&info->iso[k].gsi, m + off + 4, 4);
                memcpy(&info->iso[k].flags, m + off + 8, 2);
            }
        } else if (type == 5 && len >= 12) {                 /* local APIC address override (64-bit) */
            uint64_t a; memcpy(&a, m + off + 4, 8);
            if (a < PHYS_LIMIT) info->lapic_addr = (uint32_t)a;
        }
        off += len;
    }

    if (info->ioapic_count == 0) { *why = "no I/O APIC in the MADT"; return -1; }
    if (info->lapic_addr == 0) { *why = "no local APIC address"; return -1; }
    return 0;
}
