#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "console.h"

enum {
    INTERRUPTS_TIMER_DEMO_LIMIT = 5
};

void interrupts_init(void);
void interrupts_enable(void);
void interrupts_disable(void);
u32 interrupts_timer_ticks(void);

#endif