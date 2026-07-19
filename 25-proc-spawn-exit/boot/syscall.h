#ifndef SYSCALL_H
#define SYSCALL_H

#include "console.h"
#include "interrupts.h"

enum {
    SYS_WRITE = 0,
    SYS_EXIT  = 1,
    SYS_OPEN  = 2,
    SYS_READ  = 3,
    SYS_SPAWN = 4
};

void syscall_dispatch(struct interrupt_frame *frame);

#endif
