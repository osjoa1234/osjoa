#include "paging.h"
#include "phys_mem.h"

#define MAX_PT 32U

static u32 page_directory[1024]      __attribute__((aligned(4096)));
static u32 page_tables[MAX_PT][1024] __attribute__((aligned(4096)));
static u32 mapped_mb;
static u32 kernel_pd_phys;

struct e820_entry {
    u32 size;
    u32 addr_low;
    u32 addr_high;
    u32 len_low;
    u32 len_high;
    u32 type;
} __attribute__((packed));

static void flush_tlb(void)
{
    u32 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3));
}

u32 paging_init(u32 mmap_addr, u32 mmap_length)
{
    u32 max_addr = 0U;
    u32 offset   = 0U;
    u32 pt_count;
    u32 i, j;

    while (offset < mmap_length) {
        const struct e820_entry *e =
            (const struct e820_entry *)(mmap_addr + offset);
        if (e->type == 1U && e->addr_high == 0U) {
            u32 end = e->addr_low + e->len_low;
            if (end > max_addr) max_addr = end;
        }
        offset += e->size + 4U;
    }

    pt_count = (max_addr + 0x3FFFFFU) >> 22U;
    if (pt_count > MAX_PT) pt_count = MAX_PT;

    for (i = 0U; i < 1024U; i++) page_directory[i] = 0U;

    for (i = 0U; i < pt_count; i++) {
        for (j = 0U; j < 1024U; j++) {
            page_tables[i][j] = ((i * 1024U + j) * 0x1000U) | 0x07U;
        }
        page_directory[i] = (u32)page_tables[i] | 0x07U;
    }

    mapped_mb     = pt_count * 4U;
    kernel_pd_phys = (u32)page_directory;

    __asm__ volatile ("mov %0, %%cr3" : : "r"((u32)page_directory));
    {
        u32 cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= 0x80000000U;
        __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    }

    return (u32)page_directory;
}

u32 paging_mapped_mb(void)
{
    return mapped_mb;
}

void page_map_frame(u32 vaddr, u32 paddr)
{
    u32 pde_idx = vaddr >> 22U;
    u32 pte_idx = (vaddr >> 12U) & 0x3FFU;
    u32 *pt;
    u32 i;

    if (!(page_directory[pde_idx] & 0x01U)) {
        u32 new_pt = page_alloc();
        pt = (u32 *)new_pt;
        for (i = 0U; i < 1024U; i++) pt[i] = 0U;
        page_directory[pde_idx] = new_pt | 0x03U;
    } else {
        pt = (u32 *)(page_directory[pde_idx] & ~0xFFFU);
    }

    pt[pte_idx] = paddr | 0x03U;
    flush_tlb();
}

u32 paging_clone_dir(void)
{
    u32  pd_phys  = page_alloc();
    u32  pt0_phys = page_alloc();
    u32 *new_pd   = (u32 *)pd_phys;
    u32 *new_pt0  = (u32 *)pt0_phys;
    u32  i;

    for (i = 0U; i < 1024U; i++) new_pt0[i] = page_tables[0][i];
    new_pd[0] = pt0_phys | 0x07U;
    for (i = 1U; i < 1024U; i++) new_pd[i] = page_directory[i];

    return pd_phys;
}

void paging_set_dir(u32 pd_phys)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys));
}

void paging_restore_kernel_dir(void)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pd_phys));
}

void paging_map_user_page(u32 pd_phys, u32 vaddr, u32 paddr)
{
    u32 *pd     = (u32 *)pd_phys;
    u32  pde_idx = vaddr >> 22U;
    u32  pte_idx = (vaddr >> 12U) & 0x3FFU;
    u32 *pt     = (u32 *)(pd[pde_idx] & ~0xFFFU);

    pt[pte_idx] = paddr | 0x07U;
    flush_tlb();
}
