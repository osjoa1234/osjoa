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
    SYS_GETPID       = 20,
    SYS_GETUID       = 24,
    SYS_BRK          = 45,
    SYS_FSTAT        = 108,
    SYS_CLONE        = 120,
    SYS_UNAME        = 122,
    SYS_MPROTECT     = 125,
    SYS_ARCH_PRCTL   = 172,
    SYS_MMAP2        = 192,
    SYS_SPAWN        = 200,
    SYS_THREAD_EXIT  = 201
};

enum {
    SYS64_READ            = 0,
    SYS64_WRITE           = 1,
    SYS64_CLOSE           = 3,
    SYS64_MMAP            = 9,
    SYS64_MPROTECT        = 10,
    SYS64_MUNMAP          = 11,
    SYS64_BRK             = 12,
    SYS64_IOCTL           = 16,
    SYS64_WRITEV          = 20,
    SYS64_UNAME           = 63,
    SYS64_EXIT            = 60,
    SYS64_GETPID          = 39,
    SYS64_GETUID          = 102,
    SYS64_ARCH_PRCTL      = 158,
    SYS64_SET_TID_ADDRESS = 218,
    SYS64_EXIT_GROUP      = 231
};

extern u64 syscall_kernel_rsp;

void syscall_dispatch(struct interrupt_frame *frame);
void syscall64_dispatch(struct interrupt_frame *frame);
void syscall_entry(void);

#endif
