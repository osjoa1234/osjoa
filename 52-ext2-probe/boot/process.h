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

#define PROC_USTACK_PAGES 8U
#define PROC_USTACK_TOP   0x01000000ULL
#define PROC_MMAP_TOP     (PROC_USTACK_TOP - (u64)PROC_USTACK_PAGES * 0x1000ULL)
#define PROC_NO_PARENT    ((u32)-1U)

#define PROC_EXEC_ARGMAX 8U
#define PROC_EXEC_ARGLEN 64U

#define MAP_ANONYMOUS    0x20U

#define NSIG 32U

typedef struct {
    u64 rdi;
    u64 rsi;
    u64 rbp;
    u64 rbx;
    u64 rdx;
    u64 rcx;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
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
    u64          user_rsp;
    fork_resume_t fork_ctx;
    u32          pml4_phys;
    u64          heap_start;
    u64          heap_end;
    u64          mmap_next;
    thread_t    *threads;
    wait_queue_t wait_chldexit;
    vfs_file_t  *fds[PROC_FD_MAX];
    u64          sig_handler[NSIG];
    u64          sig_pending;
    u64          sig_blocked;
} process_t;

void       proc_init(void);
u32        proc_spawn(const char *name);
u32        proc_fork(const fork_resume_t *ctx);
void       proc_exec(const char *name, char *const argv[]);
void       proc_exit(u32 code);
void       proc_thread_exit(void);
u32        proc_clone(const fork_resume_t *ctx);
u32        proc_wait(u32 pid, u32 *exit_code);
process_t *proc_get(u32 pid);
u64        proc_brk(u64 new_brk);
u64        proc_mmap(u64 length, u32 prot, u32 flags);

#endif
