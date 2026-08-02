#ifndef PROCESS_H
#define PROCESS_H

#include "console.h"
#include "thread.h"

#define PROC_FREE    0U
#define PROC_RUNNING 1U
#define PROC_ZOMBIE  2U
#define PROC_MAX     8U

#define PROC_USTACK_TOP 0x00400000U

typedef struct {
    u32 edi;
    u32 esi;
    u32 ebp;
    u32 ebx;
    u32 edx;
    u32 ecx;
    u32 eip;
    u32 user_esp;
    u32 eflags;
} fork_resume_t;

typedef struct {
    u32          pid;
    u32          state;
    u32          exit_code;
    u32          entry;
    fork_resume_t fork_ctx;
    u32          pd_phys;
    thread_t    *thread;
    thread_t    *waiter;
} process_t;

void       proc_init(void);
u32        proc_spawn(const char *name);
u32        proc_fork(const fork_resume_t *ctx);
void       proc_exec(const char *name);
void       proc_exit(u32 code);
u32        proc_wait(u32 pid, u32 *exit_code);
process_t *proc_get(u32 pid);

#endif
