#ifndef ACPI_H
#define ACPI_H

#include "console.h"

void acpi_init(const void *rsdp_bytes);

u32 acpi_bsp_apic_id(void);
u32 acpi_lapic_address(void);

u32 acpi_ioapic_id(void);
u32 acpi_ioapic_address(void);
u32 acpi_ioapic_gsi_base(void);

u32 acpi_irq_to_gsi(u8 isa_irq);
u8  acpi_irq_active_low(u8 isa_irq);
u8  acpi_irq_level_triggered(u8 isa_irq);

#endif
