#ifndef PROCESS_H
#define PROCESS_H

#include "console.h"
#include "thread.h"
#include "wait_queue.h"
#include "vfs.h"

#define PROC_FREE      0U
#define PROC_RUNNING   1U
#define PROC_ZOMBIE    2U
#define PROC_MAX       8U
#define PROC_FD_MAX    8U

#define PROC_USTACK_TOP  0x00400000U
#define PROC_NO_PARENT   ((u32)-1U)

typedef struct {
    u64 rdi;
    u64 rsi;
    u64 rbp;
    u64 rbx;
    u64 rdx;
    u64 rcx;
    u64 rip;
    u64 user_rsp;
    u64 rflags;
} fork_resume_t;

typedef struct {
    u32          pid;
    u32          parent_pid;
    u32          state;
    u32          exit_code;
    u64          entry;
    fork_resume_t fork_ctx;
    u32          pd_phys;
    thread_t    *threads;
    wait_queue_t wait_chldexit;
    vfs_file_t  *fds[PROC_FD_MAX];
} process_t;

void       proc_init(void);
u32        proc_spawn(const char *name);
u32        proc_fork(const fork_resume_t *ctx);
void       proc_exec(const char *name);
void       proc_exit(u32 code);
void       proc_thread_exit(void);
u32        proc_clone(const fork_resume_t *ctx);
u32        proc_wait(u32 pid, u32 *exit_code);
process_t *proc_get(u32 pid);

#endif
