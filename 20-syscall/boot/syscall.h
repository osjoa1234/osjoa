#ifndef SYSCALL_H
#define SYSCALL_H

#include "console.h"
#include "interrupts.h"

enum {
    SYS_WRITE = 0,
    SYS_EXIT  = 1
};

void syscall_dispatch(struct interrupt_frame *frame);

#endif
