#include "kheap.h"
#include "paging.h"
#include "phys_mem.h"

#define KHEAP_MAX  0x800000U
#define PAGE_SIZE  0x1000U

struct block_hdr {
    u32              size;
    u32              free;
    struct block_hdr *next;
};

#define HDR_SIZE  ((u32)sizeof(struct block_hdr))

static struct block_hdr *heap_head;
static u32               heap_top;

static struct block_hdr *heap_last(void)
{
    struct block_hdr *c = heap_head;
    while (c && c->next) c = c->next;
    return c;
}

static void heap_grow(void)
{
    struct block_hdr *last;
    struct block_hdr *nb;
    u32 pa;

    if (heap_top >= KHEAP_MAX) return;

    pa = page_alloc();
    if (!pa) return;

    page_map_frame(heap_top, pa);
    nb = (struct block_hdr *)heap_top;
    heap_top += PAGE_SIZE;

    last = heap_last();

    if (last && last->free) {
        last->size += HDR_SIZE + (PAGE_SIZE - HDR_SIZE);
    } else {
        nb->size = PAGE_SIZE - HDR_SIZE;
        nb->free = 1U;
        nb->next = 0;
        if (last) last->next = nb;
        else heap_head = nb;
    }
}

void kheap_init(void)
{
    heap_top = KHEAP_START;
    heap_grow();
}

static struct block_hdr *find_free(u32 size)
{
    struct block_hdr *cur = heap_head;
    while (cur) {
        if (cur->free && cur->size >= size) return cur;
        cur = cur->next;
    }
    return 0;
}

void *kmalloc(u32 size)
{
    struct block_hdr *blk;

    size = (size + 3U) & ~3U;

    blk = find_free(size);
    if (!blk) {
        heap_grow();
        blk = find_free(size);
        if (!blk) return 0;
    }

    if (blk->size >= size + HDR_SIZE + 4U) {
        struct block_hdr *split =
            (struct block_hdr *)((u8 *)blk + HDR_SIZE + size);
        split->size = blk->size - size - HDR_SIZE;
        split->free = 1U;
        split->next = blk->next;
        blk->size   = size;
        blk->next   = split;
    }

    blk->free = 0U;
    return (void *)((u8 *)blk + HDR_SIZE);
}

void kfree(void *ptr)
{
    struct block_hdr *blk;

    if (!ptr) return;

    blk = (struct block_hdr *)((u8 *)ptr - HDR_SIZE);
    blk->free = 1U;

    while (blk->next && blk->next->free) {
        blk->size += HDR_SIZE + blk->next->size;
        blk->next  = blk->next->next;
    }
}

u32 kheap_used(void)
{
    struct block_hdr *cur = heap_head;
    u32 used = 0U;
    while (cur) {
        if (!cur->free) used += cur->size;
        cur = cur->next;
    }
    return used;
}
