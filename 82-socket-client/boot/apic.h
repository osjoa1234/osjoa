#ifndef APIC_H
#define APIC_H

#include "console.h"

void apic_init(void);
u32  apic_id(void);
void apic_eoi(void);

void ioapic_init(void);
void ioapic_unmask_irq(u8 irq, u8 vector);

#endif
