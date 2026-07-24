#include "process.h"
#include "elf.h"
#include "initrd.h"
#include "paging.h"
#include "console.h"

static process_t proc_table[PROC_MAX];

extern void enter_user_mode(u32 eip, u32 esp);

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
            proc_table[i].state  = PROC_RUNNING;
            proc_table[i].pid    = i;
            proc_table[i].waiter = 0;
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

void proc_exit(u32 code)
{
    process_t *p = (process_t *)thread_current()->user_data;

    console_set_color(0x0AU);
    console_printf("process %u exited: code=%u\n", p->pid, code);

    p->state     = PROC_ZOMBIE;
    p->exit_code = code;
    if (p->waiter)
        thread_unpark(p->waiter);
    thread_exit();
    for (;;) { __asm__ volatile ("hlt"); }
}

u32 proc_wait(u32 pid, u32 *exit_code)
{
    process_t *p = proc_get(pid);

    if (!p) return (u32)-1U;

    p->waiter = thread_current();
    if (p->state != PROC_ZOMBIE)
        thread_park();

    if (exit_code) *exit_code = p->exit_code;
    p->state  = PROC_FREE;
    p->waiter = 0;
    return 0U;
}

process_t *proc_get(u32 pid)
{
    if (pid >= PROC_MAX) return 0;
    if (proc_table[pid].state == PROC_FREE) return 0;
    return &proc_table[pid];
}
