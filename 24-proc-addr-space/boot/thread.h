#ifndef THREAD_H
#define THREAD_H

#include "console.h"

typedef void (*thread_fn_t)(void);

#define THREAD_RUNNING  0U
#define THREAD_DEAD     1U
#define THREAD_SLEEPING 2U

typedef struct thread {
    u32           esp;
    u32           id;
    u32           state;
    u32           wake_tick;
    struct thread *next;
} thread_t;

void      threads_init(void);
thread_t *thread_create(thread_fn_t fn);
void      thread_yield(void);
void      thread_sleep(u32 ms);
void      thread_exit(void);
void      scheduler_tick(void);
thread_t *thread_current(void);
thread_t *thread_list(void);
u32       thread_any_runnable(void);

#endif
