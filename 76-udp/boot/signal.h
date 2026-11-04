#ifndef SIGNAL_H
#define SIGNAL_H

#include "process.h"
#include "interrupts.h"

#define SIGINT  2U
#define SIGKILL 9U
#define SIGSEGV 11U
#define SIGTERM 15U

#define SIG_DFL 0ULL
#define SIG_IGN 1ULL

#define SIG_BLOCK   0U
#define SIG_UNBLOCK 1U
#define SIG_SETMASK 2U

typedef struct {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
} sigaction_t;

void signal_setup_trampoline(u32 pml4_phys);
void signal_reset_handlers(process_t *p);
void signal_raise_current(u32 signum);
void signal_deliver_pending(struct interrupt_frame *frame);

u32  sys_rt_sigaction(u32 signum, const sigaction_t *act, sigaction_t *oldact, u64 sigsetsize);
u32  sys_rt_sigprocmask(u32 how, const u64 *set, u64 *oldset, u64 sigsetsize);
void sys_rt_sigreturn(struct interrupt_frame *frame);

#endif
