#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_ISO     16

typedef struct {
    uint32_t lapic_addr;             /* physical address of the local APIC registers */
    int      pcat_compat;            /* the board also has the legacy 8259 PICs */
    int      cpu_count;              /* enabled processors listed in the MADT */
    int      ioapic_count;
    struct { uint8_t id; uint32_t addr; uint32_t gsi_base; } ioapic[ACPI_MAX_IOAPICS];
    int      iso_count;              /* interrupt source overrides (ISA IRQ -> GSI) */
    struct { uint8_t source; uint32_t gsi; uint16_t flags; } iso[ACPI_MAX_ISO];
} acpi_info_t;

/* Read the ACPI tables (RSDP -> RSDT/XSDT -> MADT). Returns 0 on success,
 * negative if there is no usable ACPI or MADT; `why` names the problem. */
int acpi_init(acpi_info_t *info, const char **why);

#endif
