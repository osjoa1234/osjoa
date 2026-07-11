#include "paging.h"
#include "phys_mem.h"

static u32 page_directory[1024] __attribute__((aligned(4096)));
static u32 page_table_0[1024] __attribute__((aligned(4096)));

static void cr3_load(u32 addr)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r" (addr));
}

static void cr0_set_paging(void)
{
    u32 cr0;

    __asm__ volatile ("mov %%cr0, %0" : "=r" (cr0));
    cr0 |= 0x80000000U;
    __asm__ volatile ("mov %0, %%cr0" : : "r" (cr0));
}

u32 paging_init(void)
{
    u32 i;

    for (i = 0U; i < 1024U; ++i) {
        page_table_0[i] = (i * 0x1000U) | 0x03U;
    }

    page_directory[0] = (u32)page_table_0 | 0x03U;
    for (i = 1U; i < 1024U; ++i) {
        page_directory[i] = 0U;
    }

    cr3_load((u32)page_directory);
    cr0_set_paging();

    return (u32)page_directory;
}

void page_map_frame(u32 vaddr, u32 paddr)
{
    u32 pde_idx = vaddr >> 22U;
    u32 pte_idx = (vaddr >> 12U) & 0x3FFU;
    u32 *pt;
    u32 cr3;
    u32 i;

    if (!(page_directory[pde_idx] & 0x01U)) {
        u32 new_pt = page_alloc();
        pt = (u32 *)new_pt;
        for (i = 0U; i < 1024U; i++) {
            pt[i] = 0U;
        }
        page_directory[pde_idx] = new_pt | 0x03U;
    } else {
        pt = (u32 *)(page_directory[pde_idx] & ~0xFFFU);
    }

    pt[pte_idx] = paddr | 0x03U;

    __asm__ volatile ("mov %%cr3, %0" : "=r" (cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r" (cr3));
}
