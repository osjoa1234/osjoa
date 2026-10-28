#ifndef ACPI_H
#define ACPI_H

#include "console.h"

void acpi_init(const void *rsdp_bytes);

u32 acpi_bsp_apic_id(void);
u32 acpi_lapic_address(void);

u32 acpi_cpu_count(void);
u32 acpi_cpu_apic_id(u32 index);
u8  acpi_cpu_enabled(u32 index);

u32 acpi_ioapic_id(void);
u32 acpi_ioapic_address(void);
u32 acpi_ioapic_gsi_base(void);

u32 acpi_irq_to_gsi(u8 isa_irq);
u8  acpi_irq_active_low(u8 isa_irq);
u8  acpi_irq_level_triggered(u8 isa_irq);

int acpi_mcfg_found(void);
u64 acpi_mcfg_base(void);
u8  acpi_mcfg_start_bus(void);
u8  acpi_mcfg_end_bus(void);

#endif
