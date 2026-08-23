#include "pipe.h"
#include "wait_queue.h"
#include "thread.h"
#include "kheap.h"

#define PIPE_MAX      4U
#define PIPE_BUF_SIZE 1024U

typedef struct {
    u8           buf[PIPE_BUF_SIZE];
    u32          head;
    u32          tail;
    u32          count;
    u32          read_refs;
    u32          write_refs;
    u32          used;
    wait_queue_t wait_read;
    wait_queue_t wait_write;
} pipe_t;

static pipe_t pipes[PIPE_MAX];

void pipe_init(void)
{
    u32 i;
    for (i = 0U; i < PIPE_MAX; i++) pipes[i].used = 0U;
}

static u32 pipe_read(int bfd, u8 *buf, u32 len, u32 pos)
{
    pipe_t *p = &pipes[bfd];
    u32     n;

    (void)pos;

    for (;;) {
        if (p->count > 0U) break;
        if (p->write_refs == 0U) return 0U;
        wq_add(&p->wait_read, thread_current());
        thread_park();
    }

    n = 0U;
    while (n < len && p->count > 0U) {
        buf[n++] = p->buf[p->head];
        p->head  = (p->head + 1U) % PIPE_BUF_SIZE;
        p->count--;
    }
    wq_wake_all(&p->wait_write);
    return n;
}

static u32 pipe_write(int bfd, const u8 *buf, u32 len, u32 pos)
{
    pipe_t *p = &pipes[bfd];
    u32     n = 0U;

    (void)pos;

    while (n < len) {
        for (;;) {
            if (p->count < PIPE_BUF_SIZE) break;
            if (p->read_refs == 0U) return n;
            wq_add(&p->wait_write, thread_current());
            thread_park();
        }
        while (n < len && p->count < PIPE_BUF_SIZE) {
            p->buf[p->tail] = buf[n++];
            p->tail         = (p->tail + 1U) % PIPE_BUF_SIZE;
            p->count++;
        }
        wq_wake_all(&p->wait_read);
    }
    return n;
}

static void pipe_maybe_free(pipe_t *p)
{
    if (p->read_refs == 0U && p->write_refs == 0U) p->used = 0U;
}

static void pipe_read_close(int bfd)
{
    pipe_t *p = &pipes[bfd];
    if (p->read_refs > 0U) p->read_refs--;
    if (p->read_refs == 0U) wq_wake_all(&p->wait_write);
    pipe_maybe_free(p);
}

static void pipe_write_close(int bfd)
{
    pipe_t *p = &pipes[bfd];
    if (p->write_refs > 0U) p->write_refs--;
    if (p->write_refs == 0U) wq_wake_all(&p->wait_read);
    pipe_maybe_free(p);
}

static void pipe_read_dup(int bfd)  { pipes[bfd].read_refs++; }
static void pipe_write_dup(int bfd) { pipes[bfd].write_refs++; }

static vfs_ops_t pipe_read_ops  = { 0, pipe_read, 0, 0, pipe_read_close,  pipe_read_dup };
static vfs_ops_t pipe_write_ops = { 0, 0, pipe_write, 0, pipe_write_close, pipe_write_dup };

int pipe_create(vfs_file_t **out_read, vfs_file_t **out_write)
{
    pipe_t     *p = 0;
    vfs_file_t *rf;
    vfs_file_t *wf;
    u32         i;

    for (i = 0U; i < PIPE_MAX; i++) {
        if (!pipes[i].used) { p = &pipes[i]; break; }
    }
    if (!p) return -1;

    rf = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!rf) return -1;
    wf = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!wf) { kfree(rf); return -1; }

    p->head       = 0U;
    p->tail       = 0U;
    p->count      = 0U;
    p->read_refs  = 1U;
    p->write_refs = 1U;
    p->used       = 1U;
    wq_init(&p->wait_read);
    wq_init(&p->wait_write);

    rf->ops        = &pipe_read_ops;
    rf->backend_fd = (int)i;
    rf->pos        = 0U;

    wf->ops        = &pipe_write_ops;
    wf->backend_fd = (int)i;
    wf->pos        = 0U;

    *out_read  = rf;
    *out_write = wf;
    return 0;
}
