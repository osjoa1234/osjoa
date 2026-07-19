#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "console.h"

struct interrupt_frame {
    u32 edi;
    u32 esi;
    u32 ebp;
    u32 esp;
    u32 ebx;
    u32 edx;
    u32 ecx;
    u32 eax;
    u32 vector;
    u32 error_code;
    u32 eip;
    u32 cs;
    u32 eflags;
};

void interrupts_init(void);
void interrupts_enable(void);
void interrupts_disable(void);
void interrupts_unmask_irq(u8 irq);

#endif
