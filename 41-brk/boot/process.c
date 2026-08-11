#include "process.h"
#include "console_dev.h"
#include "elf.h"
#include "initrd.h"
#include "paging.h"
#include "phys_mem.h"
#include "kheap.h"
#include "console.h"

static process_t    proc_table[PROC_MAX];
static wait_queue_t kernel_wait_chldexit;

extern void enter_user_mode(u64 rip, u64 user_rsp);
extern void enter_user_mode_fork(const fork_resume_t *ctx);

void proc_init(void)
{
    u32 i;
    for (i = 0U; i < PROC_MAX; i++) proc_table[i].state = PROC_FREE;
    wq_init(&kernel_wait_chldexit);
}

static process_t *proc_alloc(void)
{
    u32 i;
    u32 j;

    for (i = 0U; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_FREE) {
            proc_table[i].state       = PROC_RUNNING;
            proc_table[i].pid         = i;
            proc_table[i].parent_pid  = PROC_NO_PARENT;
            proc_table[i].threads     = 0;
            wq_init(&proc_table[i].wait_chldexit);
            for (j = 0U; j < PROC_FD_MAX; j++) proc_table[i].fds[j] = 0;
            return &proc_table[i];
        }
    }
    return 0;
}

static process_t *find_zombie_child(u32 parent_pid)
{
    u32 i;

    for (i = 0U; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_ZOMBIE &&
            proc_table[i].parent_pid == parent_pid)
            return &proc_table[i];
    }
    return 0;
}

static void proc_run_trampoline(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    enter_user_mode(p->entry, PROC_USTACK_TOP);
}

static void fork_child_trampoline(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    enter_user_mode_fork(&p->fork_ctx);
}

typedef struct {
    process_t    *proc;
    fork_resume_t resume;
} clone_fork_ctx_t;

static void clone_fork_trampoline(void)
{
    clone_fork_ctx_t *ctx  = (clone_fork_ctx_t *)thread_current()->user_data;
    process_t        *proc = ctx->proc;
    fork_resume_t     r    = ctx->resume;
    thread_current()->user_data = proc;
    kfree(ctx);
    enter_user_mode_fork(&r);
}

u32 proc_spawn(const char *name)
{
    process_t *p;
    int        bin_fd;
    u32        pml4_phys;
    u64        entry;
    u64        brk_start;
    thread_t  *t;

    p = proc_alloc();
    if (!p) return (u32)-1U;

    bin_fd = initrd_open(name);
    if (bin_fd < 0) { p->state = PROC_FREE; return (u32)-1U; }

    pml4_phys = paging_clone_dir();
    entry     = elf_load_process(initrd_data(bin_fd), initrd_size(bin_fd), pml4_phys, &brk_start);
    if (!entry) { p->state = PROC_FREE; return (u32)-1U; }

    p->entry      = entry;
    p->pml4_phys  = pml4_phys;
    p->heap_start = brk_start;
    p->heap_end   = brk_start;
    p->parent_pid = PROC_NO_PARENT;

    p->fds[0] = console_dev_open();
    p->fds[1] = console_dev_open();
    p->fds[2] = console_dev_open();

    t = thread_create_with_data(proc_run_trampoline, p);
    if (!t) { p->state = PROC_FREE; return (u32)-1U; }

    t->pd        = pml4_phys;
    t->proc_next = 0;
    p->threads   = t;

    return p->pid;
}

u32 proc_fork(const fork_resume_t *ctx)
{
    process_t *parent = (process_t *)thread_current()->user_data;
    process_t *child;
    u32        child_pml4_phys;
    u32        j;
    thread_t  *t;

    child = proc_alloc();
    if (!child) return (u32)-1U;

    for (j = 0U; j < PROC_FD_MAX; j++) {
        if (parent->fds[j]) child->fds[j] = vfs_dup(parent->fds[j]);
    }

    child_pml4_phys = paging_clone_dir();
    paging_copy_user_pages(parent->pml4_phys, child_pml4_phys);

    child->fork_ctx   = *ctx;
    child->entry      = ctx->rip;
    child->pml4_phys  = child_pml4_phys;
    child->heap_start = parent->heap_start;
    child->heap_end   = parent->heap_end;
    child->parent_pid = parent->pid;

    t = thread_create_with_data(fork_child_trampoline, child);
    if (!t) { child->state = PROC_FREE; return (u32)-1U; }

    t->pd         = child_pml4_phys;
    t->proc_next  = 0;
    child->threads = t;

    return child->pid;
}

void proc_exec(const char *name)
{
    process_t *p = (process_t *)thread_current()->user_data;
    int        fd;
    u64        entry;
    u64        brk_start;
    u32        j;

    for (j = 3U; j < PROC_FD_MAX; j++) {
        if (p->fds[j]) { vfs_close(p->fds[j]); p->fds[j] = 0; }
    }

    fd = initrd_open(name);
    if (fd < 0) return;

    paging_free_user_pages(p->pml4_phys);

    entry = elf_load_process(initrd_data(fd), initrd_size(fd), p->pml4_phys, &brk_start);
    if (!entry) return;

    p->entry      = entry;
    p->heap_start = brk_start;
    p->heap_end   = brk_start;
    enter_user_mode(entry, PROC_USTACK_TOP);
    for (;;) { __asm__ volatile ("hlt"); }
}

void proc_exit(u32 code)
{
    process_t *p      = (process_t *)thread_current()->user_data;
    process_t *parent;
    thread_t  *t;
    u32        j;

    for (j = 0U; j < PROC_FD_MAX; j++) {
        if (p->fds[j]) { vfs_close(p->fds[j]); p->fds[j] = 0; }
    }

    console_set_color(0x0AU);
    console_printf("process %u exited: code=%u\n", p->pid, code);

    p->state     = PROC_ZOMBIE;
    p->exit_code = code;

    for (t = p->threads; t; t = t->proc_next) {
        if (t != thread_current())
            t->state = THREAD_DEAD;
    }

    if (p->parent_pid != PROC_NO_PARENT) {
        parent = proc_get(p->parent_pid);
        if (parent)
            wq_wake_all(&parent->wait_chldexit);
    } else {
        wq_wake_all(&kernel_wait_chldexit);
    }

    thread_exit();
    for (;;) { __asm__ volatile ("hlt"); }
}

void proc_thread_exit(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    thread_t  *t;
    u32        alive = 0U;

    for (t = p->threads; t; t = t->proc_next) {
        if (t != thread_current() && t->state != THREAD_DEAD)
            alive++;
    }

    if (alive == 0U) {
        proc_exit(0U);
        return;
    }

    thread_exit();
    for (;;) { __asm__ volatile ("hlt"); }
}

u32 proc_clone(const fork_resume_t *ctx)
{
    process_t        *p    = (process_t *)thread_current()->user_data;
    clone_fork_ctx_t *cctx;
    thread_t         *t;
    thread_t         *last;

    cctx = (clone_fork_ctx_t *)kmalloc(sizeof(clone_fork_ctx_t));
    if (!cctx) return (u32)-1U;
    cctx->proc   = p;
    cctx->resume = *ctx;

    t = thread_create_with_data(clone_fork_trampoline, cctx);
    if (!t) { kfree(cctx); return (u32)-1U; }
    t->pd = p->pml4_phys;

    last = p->threads;
    while (last->proc_next) last = last->proc_next;
    last->proc_next = t;

    return t->id;
}

u32 proc_wait(u32 pid, u32 *exit_code)
{
    if (pid == (u32)-1U) {
        process_t *caller = (process_t *)thread_current()->user_data;
        process_t *zombie;
        u32        child_pid;

        for (;;) {
            zombie = find_zombie_child(caller->pid);
            if (zombie) break;
            wq_add(&caller->wait_chldexit, thread_current());
            thread_park();
        }

        child_pid = zombie->pid;
        if (exit_code) *exit_code = zombie->exit_code;
        zombie->state = PROC_FREE;
        return child_pid;
    }

    {
        process_t    *caller = (process_t *)thread_current()->user_data;
        wait_queue_t *wq     = caller ? &caller->wait_chldexit : &kernel_wait_chldexit;
        process_t    *p;

        for (;;) {
            p = proc_get(pid);
            if (!p || p->state == PROC_ZOMBIE) break;
            wq_add(wq, thread_current());
            thread_park();
        }

        if (!p) return (u32)-1U;
        if (exit_code) *exit_code = p->exit_code;
        p->state = PROC_FREE;
        return pid;
    }
}

process_t *proc_get(u32 pid)
{
    if (pid >= PROC_MAX) return 0;
    if (proc_table[pid].state == PROC_FREE) return 0;
    return &proc_table[pid];
}

u64 proc_brk(u64 new_brk)
{
    process_t *p = (process_t *)thread_current()->user_data;
    u64        old_top;
    u64        new_top;
    u64        va;

    if (new_brk < p->heap_start) return p->heap_end;

    old_top = (p->heap_end + 0xFFFULL) & ~0xFFFULL;
    new_top = (new_brk    + 0xFFFULL) & ~0xFFFULL;

    for (va = old_top; va < new_top; va += 0x1000ULL) {
        u32 frame = page_alloc();
        u8 *fdst  = (u8 *)((u64)frame + KERNEL_OFFSET);
        u32 k;
        for (k = 0U; k < 0x1000U; k++) fdst[k] = 0U;
        paging_map_user_page(p->pml4_phys, va, frame);
    }

    p->heap_end = new_brk;
    return p->heap_end;
}
