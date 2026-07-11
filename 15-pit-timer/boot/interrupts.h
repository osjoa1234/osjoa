#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "console.h"

void interrupts_init(void);
void interrupts_enable(void);
void interrupts_disable(void);
void interrupts_unmask_irq(u8 irq);

#endif
