#ifndef TIMER_H
#define TIMER_H

#include "console.h"

void timer_init(u32 hz);
void timer_irq(void);
u32  timer_ticks(void);
void timer_sleep(u32 ms);

#endif
