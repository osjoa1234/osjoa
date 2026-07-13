#include "thread.h"
#include "kheap.h"

#define THREAD_STACK_SIZE 4096U

extern void switch_context(u32 *old_esp, u32 new_esp);

static thread_t  idle_task;
static thread_t *current;
static u32       next_id;

void threads_init(void)
{
    idle_task.esp   = 0U;
    idle_task.id    = next_id++;
    idle_task.state = THREAD_RUNNING;
    idle_task.next  = &idle_task;
    current = &idle_task;
}

thread_t *thread_create(thread_fn_t fn)
{
    thread_t *t;
    u32      *stack;
    u32      *sp;
    thread_t *last;

    t = (thread_t *)kmalloc(sizeof(thread_t));
    if (!t) return 0;

    stack = (u32 *)kmalloc(THREAD_STACK_SIZE);
    if (!stack) { kfree(t); return 0; }

    sp = (u32 *)((u8 *)stack + THREAD_STACK_SIZE);

    *--sp = (u32)thread_exit;
    *--sp = (u32)fn;
    *--sp = 0x00000202U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;

    t->esp   = (u32)sp;
    t->id    = next_id++;
    t->state = THREAD_RUNNING;

    last = &idle_task;
    while (last->next != &idle_task) last = last->next;
    last->next = t;
    t->next    = &idle_task;

    return t;
}

void thread_yield(void)
{
    thread_t *prev = current;
    thread_t *next = prev->next;

    while (next != prev && next->state == THREAD_DEAD) {
        next = next->next;
    }

    if (next == prev) return;

    current = next;
    switch_context(&prev->esp, next->esp);
}

void thread_exit(void)
{
    current->state = THREAD_DEAD;
    thread_yield();
}

thread_t *thread_current(void)
{
    return current;
}

thread_t *thread_list(void)
{
    return &idle_task;
}

u32 thread_any_runnable(void)
{
    const thread_t *t = idle_task.next;

    while (t != &idle_task) {
        if (t->state == THREAD_RUNNING) return 1U;
        t = t->next;
    }
    return 0U;
}
