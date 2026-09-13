#include "paging.h"
#include "phys_mem.h"

extern u64 boot_pml4[512];

#define PTE_P  0x001ULL
#define PTE_RW 0x002ULL
#define PTE_US 0x004ULL
#define ADDR_MASK 0x000FFFFFFFFFF000ULL

static u64 kernel_pml4_phys;
static u32 mapped_mb;

struct e820_entry {
    u32 size;
    u32 addr_low;
    u32 addr_high;
    u32 len_low;
    u32 len_high;
    u32 type;
} __attribute__((packed));

static u64 *tbl(u64 phys)
{
    return (u64 *)(phys + KERNEL_OFFSET);
}

static u32 phys_of(const void *virt)
{
    return (u32)((u64)virt - KERNEL_OFFSET);
}

static void flush_tlb(void)
{
    u64 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3));
}

static u64 *next_level(u64 *table, u32 index, int user, int create)
{
    u64 entry = table[index];
    u64 phys;

    if (entry & PTE_P) {
        if (user && !(entry & PTE_US)) table[index] = entry | PTE_US;
        return tbl(entry & ADDR_MASK);
    }

    if (!create) return 0;

    phys = page_alloc();
    {
        u64 *nt = tbl(phys);
        u32  i;
        for (i = 0U; i < 512U; i++) nt[i] = 0U;
    }
    table[index] = phys | PTE_P | PTE_RW | (user ? PTE_US : 0U);
    return tbl(phys);
}

static u64 *walk_pt(u64 *pml4, u64 vaddr, int user, int create)
{
    u32  i4 = (u32)((vaddr >> 39) & 0x1FFULL);
    u32  i3 = (u32)((vaddr >> 30) & 0x1FFULL);
    u32  i2 = (u32)((vaddr >> 21) & 0x1FFULL);
    u64 *pdpt;
    u64 *pd;

    pdpt = next_level(pml4, i4, user, create);
    if (!pdpt) return 0;

    pd = next_level(pdpt, i3, user, create);
    if (!pd) return 0;

    return next_level(pd, i2, user, create);
}

void paging_init(u64 mmap_vaddr, u32 mmap_length)
{
    u32 max_addr = 0U;
    u32 offset   = 0U;

    while (offset < mmap_length) {
        const struct e820_entry *e =
            (const struct e820_entry *)(mmap_vaddr + offset);
        if (e->type == 1U && e->addr_high == 0U) {
            u32 end = e->addr_low + e->len_low;
            if (end > max_addr) max_addr = end;
        }
        offset += e->size + 4U;
    }

    mapped_mb = (max_addr + 0xFFFFFU) >> 20U;

    kernel_pml4_phys = (u64)phys_of(boot_pml4);
}

u32 paging_mapped_mb(void)
{
    return mapped_mb;
}

void page_map_frame(u64 vaddr, u32 paddr)
{
    u64 *pml4 = tbl(kernel_pml4_phys);
    u64 *pt   = walk_pt(pml4, vaddr, 0, 1);
    u32  pt_i = (u32)((vaddr >> 12) & 0x1FFULL);

    pt[pt_i] = (u64)paddr | PTE_P | PTE_RW;
    flush_tlb();
}

u32 paging_clone_dir(void)
{
    u32  pml4_phys = page_alloc();
    u64 *new_pml4  = tbl(pml4_phys);
    u64 *kpml4     = tbl(kernel_pml4_phys);
    u32  i;

    for (i = 0U; i < 512U; i++) new_pml4[i] = 0U;
    new_pml4[511] = kpml4[511];

    return pml4_phys;
}

void paging_set_dir(u32 pml4_phys)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"((u64)pml4_phys));
}

void paging_restore_kernel_dir(void)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pml4_phys));
}

void paging_map_user_page(u32 pml4_phys, u64 vaddr, u32 paddr)
{
    u64 *pml4 = tbl(pml4_phys);
    u64 *pt   = walk_pt(pml4, vaddr, 1, 1);
    u32  pt_i = (u32)((vaddr >> 12) & 0x1FFULL);

    pt[pt_i] = (u64)paddr | PTE_P | PTE_RW | PTE_US;
    flush_tlb();
}

void paging_unmap_user_page(u32 pml4_phys, u64 vaddr)
{
    u64 *pml4 = tbl(pml4_phys);
    u64 *pt   = walk_pt(pml4, vaddr, 1, 0);
    u32  pt_i = (u32)((vaddr >> 12) & 0x1FFULL);

    if (!pt) return;
    if (!(pt[pt_i] & PTE_P)) return;

    page_free((u32)(pt[pt_i] & ADDR_MASK));
    pt[pt_i] = 0U;
    flush_tlb();
}

void paging_free_user_pages(u32 pml4_phys)
{
    u64 *pml4 = tbl(pml4_phys);
    u32  i4;

    for (i4 = 0U; i4 < 511U; i4++) {
        u64  e4 = pml4[i4];
        u64 *pdpt;
        u32  i3;

        if (!(e4 & PTE_P)) continue;
        pdpt = tbl(e4 & ADDR_MASK);

        for (i3 = 0U; i3 < 512U; i3++) {
            u64  e3 = pdpt[i3];
            u64 *pd;
            u32  i2;

            if (!(e3 & PTE_P)) continue;
            pd = tbl(e3 & ADDR_MASK);

            for (i2 = 0U; i2 < 512U; i2++) {
                u64  e2 = pd[i2];
                u64 *pt;
                u32  i1;

                if (!(e2 & PTE_P)) continue;
                pt = tbl(e2 & ADDR_MASK);

                for (i1 = 0U; i1 < 512U; i1++) {
                    if (pt[i1] & PTE_P) {
                        page_free((u32)(pt[i1] & ADDR_MASK));
                        pt[i1] = 0U;
                    }
                }
                page_free((u32)(e2 & ADDR_MASK));
                pd[i2] = 0U;
            }
            page_free((u32)(e3 & ADDR_MASK));
            pdpt[i3] = 0U;
        }
        page_free((u32)(e4 & ADDR_MASK));
        pml4[i4] = 0U;
    }

    flush_tlb();
}

void paging_copy_user_pages(u32 src_pml4_phys, u32 dst_pml4_phys)
{
    u64 *src_pml4 = tbl(src_pml4_phys);
    u64 *dst_pml4 = tbl(dst_pml4_phys);
    u32  i4;

    for (i4 = 0U; i4 < 511U; i4++) {
        u64  e4 = src_pml4[i4];
        u64 *src_pdpt;
        u32  i3;

        if (!(e4 & PTE_P)) continue;
        src_pdpt = tbl(e4 & ADDR_MASK);

        for (i3 = 0U; i3 < 512U; i3++) {
            u64  e3 = src_pdpt[i3];
            u64 *src_pd;
            u32  i2;

            if (!(e3 & PTE_P)) continue;
            src_pd = tbl(e3 & ADDR_MASK);

            for (i2 = 0U; i2 < 512U; i2++) {
                u64  e2 = src_pd[i2];
                u64 *src_pt;
                u32  i1;

                if (!(e2 & PTE_P)) continue;
                src_pt = tbl(e2 & ADDR_MASK);

                for (i1 = 0U; i1 < 512U; i1++) {
                    u64 e1 = src_pt[i1];
                    u64 vaddr;
                    u32 dst_frame;
                    u64 *dst_pt;
                    u8  *sdata, *ddata;
                    u32  k;

                    if (!(e1 & PTE_P)) continue;

                    vaddr = ((u64)i4 << 39) | ((u64)i3 << 30) |
                            ((u64)i2 << 21) | ((u64)i1 << 12);

                    dst_frame = page_alloc();
                    dst_pt    = walk_pt(dst_pml4, vaddr, 1, 1);

                    sdata = (u8 *)tbl(e1 & ADDR_MASK);
                    ddata = (u8 *)tbl((u64)dst_frame);
                    for (k = 0U; k < 0x1000U; k++) ddata[k] = sdata[k];

                    dst_pt[i1] = (u64)dst_frame | (e1 & 0xFFFULL);
                }
            }
        }
    }

    flush_tlb();
}
