#include "phys_mem.h"

#define PAGE_SIZE    0x1000U
#define TOTAL_PAGES  0x100000U
#define BITMAP_WORDS (TOTAL_PAGES / 32U)

static u32 bitmap[BITMAP_WORDS];
static u32 free_count;

struct mmap_entry {
    u32 size;
    u32 addr_low;
    u32 addr_high;
    u32 len_low;
    u32 len_high;
    u32 type;
} __attribute__((packed));

static void range_mark_free(u32 first, u32 last)
{
    u32 i;

    for (i = first; i < last; i++) {
        if (!(bitmap[i / 32U] & (1U << (i % 32U)))) {
            bitmap[i / 32U] |= 1U << (i % 32U);
            free_count++;
        }
    }
}

static void range_mark_used(u32 first, u32 last)
{
    u32 i;

    for (i = first; i < last; i++) {
        if (bitmap[i / 32U] & (1U << (i % 32U))) {
            bitmap[i / 32U] &= ~(1U << (i % 32U));
            free_count--;
        }
    }
}

void phys_mem_init(u32 mmap_vaddr, u32 mmap_length, u32 kernel_phys_end)
{
    u32 offset = 0U;
    u32 i;

    for (i = 0U; i < BITMAP_WORDS; i++) {
        bitmap[i] = 0U;
    }
    free_count = 0U;

    while (offset < mmap_length) {
        const struct mmap_entry *e =
            (const struct mmap_entry *)(mmap_vaddr + offset);

        if (e->type == 1U && e->addr_high == 0U) {
            u32 start = (e->addr_low + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
            u32 end   = (e->addr_low + e->len_low) & ~(PAGE_SIZE - 1U);

            if (end > start) {
                range_mark_free(start / PAGE_SIZE, end / PAGE_SIZE);
            }
        }

        offset += e->size + 4U;
    }

    range_mark_used(0U, 1U);

    range_mark_used(0x100000U / PAGE_SIZE,
                    (kernel_phys_end + PAGE_SIZE - 1U) / PAGE_SIZE);
}

u32 page_alloc(void)
{
    u32 i;
    u32 bit;

    for (i = 0U; i < BITMAP_WORDS; i++) {
        if (bitmap[i]) {
            for (bit = 0U; bit < 32U; bit++) {
                if (bitmap[i] & (1U << bit)) {
                    bitmap[i] &= ~(1U << bit);
                    free_count--;
                    return (i * 32U + bit) * PAGE_SIZE;
                }
            }
        }
    }

    return 0U;
}

void page_free(u32 addr)
{
    u32 page = addr / PAGE_SIZE;

    if (!(bitmap[page / 32U] & (1U << (page % 32U)))) {
        bitmap[page / 32U] |= 1U << (page % 32U);
        free_count++;
    }
}

u32 phys_mem_free_count(void)
{
    return free_count;
}
