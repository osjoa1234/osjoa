#ifndef SYSCALL_H
#define SYSCALL_H

#include "console.h"
#include "interrupts.h"

enum {
    SYS_EXIT         = 1,
    SYS_FORK         = 2,
    SYS_READ         = 3,
    SYS_WRITE        = 4,
    SYS_OPEN         = 5,
    SYS_CLOSE        = 6,
    SYS_WAITPID      = 7,
    SYS_EXECVE       = 11,
    SYS_LSEEK        = 19,
    SYS_CLONE        = 120,
    SYS_SPAWN        = 200,
    SYS_THREAD_EXIT  = 201
};

void syscall_dispatch(struct interrupt_frame *frame);

#endif
