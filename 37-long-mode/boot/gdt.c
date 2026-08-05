#include "gdt.h"

struct gdt_descriptor {
    u16 limit;
    u64 base;
} __attribute__((packed));

struct tss64 {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

static u64               gdt[7];
static struct gdt_descriptor gdt_ptr;
static struct tss64      tss;

extern void gdt_flush(void *ptr);
extern void tss_flush(void);

static void tss_set_desc(u64 base, u32 limit)
{
    gdt[5] = ((u64)(limit & 0xFFFFU))
           | ((u64)(base  & 0x00FFFFFFULL) << 16)
           | ((u64)0x89ULL                 << 40)
           | ((u64)((limit >> 16) & 0xFU)  << 48)
           | ((u64)((base >> 24) & 0xFFULL)<< 56);
    gdt[6] = (base >> 32) & 0xFFFFFFFFULL;
}

static void tss_init(u64 kernel_stack)
{
    u32 i;
    u8 *p = (u8 *)&tss;

    for (i = 0U; i < (u32)sizeof(tss); i++) p[i] = 0U;

    tss.rsp0       = kernel_stack;
    tss.iomap_base = (u16)sizeof(tss);

    tss_set_desc((u64)&tss, (u32)(sizeof(tss) - 1U));
}

void gdt_set_kernel_stack(u64 rsp0)
{
    tss.rsp0 = rsp0;
}

void gdt_init(u64 kernel_stack)
{
    gdt[0] = 0x0000000000000000ULL;
    gdt[1] = 0x00AF9A000000FFFFULL;
    gdt[2] = 0x00CF92000000FFFFULL;
    gdt[3] = 0x00AFFA000000FFFFULL;
    gdt[4] = 0x00CFF2000000FFFFULL;

    tss_init(kernel_stack);

    gdt_ptr.limit = (u16)(sizeof(gdt) - 1U);
    gdt_ptr.base  = (u64)&gdt[0];

    gdt_flush(&gdt_ptr);
    tss_flush();
}
