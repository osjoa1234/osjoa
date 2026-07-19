#include "process.h"
#include "elf.h"
#include "initrd.h"
#include "paging.h"
#include "timer.h"
#include "console.h"

static process_t proc_table[PROC_MAX];

extern void enter_user_mode(u32 eip, u32 esp);
extern void enter_user_mode_fork_child(u32 eip, u32 user_esp);

void proc_init(void)
{
    u32 i;
    for (i = 0U; i < PROC_MAX; i++) proc_table[i].state = PROC_FREE;
}

static process_t *proc_alloc(void)
{
    u32 i;

    for (i = 0U; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_FREE) {
            proc_table[i].state = PROC_RUNNING;
            proc_table[i].pid   = i;
            return &proc_table[i];
        }
    }
    return 0;
}

static void proc_run_trampoline(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    enter_user_mode(p->entry, PROC_USTACK_TOP);
}

u32 proc_spawn(const char *name)
{
    process_t *p;
    int        fd;
    u32        pd;
    u32        entry;
    thread_t  *t;

    p = proc_alloc();
    if (!p) return (u32)-1U;

    fd = initrd_open(name);
    if (fd < 0) { p->state = PROC_FREE; return (u32)-1U; }

    pd    = paging_clone_dir();
    entry = elf_load_process(initrd_data(fd), initrd_size(fd), pd);
    if (!entry) { p->state = PROC_FREE; return (u32)-1U; }

    p->entry   = entry;
    p->pd_phys = pd;

    t = thread_create_with_data(proc_run_trampoline, p);
    if (!t) { p->state = PROC_FREE; return (u32)-1U; }

    t->pd     = pd;
    p->thread = t;

    return p->pid;
}

void proc_exec(const char *name)
{
    process_t *p = (process_t *)thread_current()->user_data;
    int        fd;
    u32        entry;

    fd = initrd_open(name);
    if (fd < 0) return;

    paging_free_user_pages(p->pd_phys);

    entry = elf_load_process(initrd_data(fd), initrd_size(fd), p->pd_phys);
    if (!entry) return;

    p->entry = entry;
    enter_user_mode(entry, PROC_USTACK_TOP);
    for (;;) { __asm__ volatile ("hlt"); }
}

static void fork_child_trampoline(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    enter_user_mode_fork_child(p->entry, p->fork_esp);
}

u32 proc_fork(struct interrupt_frame *frame)
{
    process_t *parent = (process_t *)thread_current()->user_data;
    process_t *child;
    u32        child_pd;
    thread_t  *t;

    child = proc_alloc();
    if (!child) return (u32)-1U;

    child_pd = paging_fork_dir(parent->pd_phys);

    child->entry    = frame->eip;
    child->fork_esp = frame->user_esp;
    child->pd_phys  = child_pd;

    t = thread_create_with_data(fork_child_trampoline, child);
    if (!t) { child->state = PROC_FREE; return (u32)-1U; }

    t->pd         = child_pd;
    child->thread = t;

    return child->pid;
}

void proc_exit(u32 code)
{
    process_t *p = (process_t *)thread_current()->user_data;

    console_set_color(0x0AU);
    console_printf("process %u exited: code=%u\n", p->pid, code);

    p->state     = PROC_ZOMBIE;
    p->exit_code = code;
    thread_exit();
    for (;;) { __asm__ volatile ("hlt"); }
}

u32 proc_wait(u32 pid, u32 *exit_code)
{
    process_t *p = proc_get(pid);

    if (!p) return (u32)-1U;

    while (p->state != PROC_ZOMBIE) {
        thread_sleep(10U);
    }

    if (exit_code) *exit_code = p->exit_code;
    p->state = PROC_FREE;
    return 0U;
}

process_t *proc_get(u32 pid)
{
    if (pid >= PROC_MAX) return 0;
    if (proc_table[pid].state == PROC_FREE) return 0;
    return &proc_table[pid];
}
