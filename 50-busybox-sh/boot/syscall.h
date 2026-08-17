#ifndef SYSCALL_H
#define SYSCALL_H

#include "console.h"
#include "interrupts.h"

enum {
    SYS_READ         = 0,
    SYS_WRITE        = 1,
    SYS_OPEN         = 2,
    SYS_CLOSE        = 3,
    SYS_FSTAT        = 5,
    SYS_LSEEK        = 8,
    SYS_GETCWD       = 79,
    SYS_ACCESS       = 21,
    SYS_RT_SIGACTION   = 13,
    SYS_RT_SIGPROCMASK = 14,
    SYS_RT_SIGRETURN   = 15,
    SYS_MMAP         = 9,
    SYS_MPROTECT     = 10,
    SYS_MUNMAP       = 11,
    SYS_BRK          = 12,
    SYS_IOCTL        = 16,
    SYS_WRITEV       = 20,
    SYS_PIPE         = 22,
    SYS_DUP2         = 33,
    SYS_GETPID       = 39,
    SYS_CLONE        = 56,
    SYS_FORK         = 57,
    SYS_EXECVE       = 59,
    SYS_EXIT         = 60,
    SYS_WAIT4        = 61,
    SYS_UNAME        = 63,
    SYS_GETUID       = 102,
    SYS_CHDIR        = 80,
    SYS_ARCH_PRCTL   = 158,
    SYS_SET_TID_ADDRESS = 218,
    SYS_EXIT_GROUP   = 231
};

extern u64 syscall_kernel_rsp;

void syscall_dispatch(struct interrupt_frame *frame);
void syscall_entry(void);

#endif
