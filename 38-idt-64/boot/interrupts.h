#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "console.h"

struct interrupt_frame {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rdi;
    u64 rsi;
    u64 rbp;
    u64 rbx;
    u64 rdx;
    u64 rcx;
    u64 rax;
    u64 vector;
    u64 error_code;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 user_rsp;
    u64 user_ss;
};

void interrupts_init(void);
void interrupts_enable(void);
void interrupts_disable(void);
void interrupts_unmask_irq(u8 irq);

#endif
