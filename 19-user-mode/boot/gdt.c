#include "gdt.h"

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
} __attribute__((packed));

struct gdt_pointer {
    u16 limit;
    u32 base;
} __attribute__((packed));

struct tss_entry {
    u32 prev_tss;
    u32 esp0;
    u32 ss0;
    u32 esp1;
    u32 ss1;
    u32 esp2;
    u32 ss2;
    u32 cr3;
    u32 eip;
    u32 eflags;
    u32 eax;
    u32 ecx;
    u32 edx;
    u32 ebx;
    u32 esp;
    u32 ebp;
    u32 esi;
    u32 edi;
    u32 es;
    u32 cs;
    u32 ss;
    u32 ds;
    u32 fs;
    u32 gs;
    u32 ldt;
    u16 trap;
    u16 iomap_base;
} __attribute__((packed));

static struct gdt_entry  gdt[6];
static struct gdt_pointer gdt_ptr;
static struct tss_entry  tss;

extern void gdt_flush(u32 ptr);
extern void tss_flush(void);

static void gdt_set_entry(u32 idx, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[idx].base_low    = (u16)(base & 0xFFFFU);
    gdt[idx].base_mid    = (u8)((base >> 16U) & 0xFFU);
    gdt[idx].base_high   = (u8)((base >> 24U) & 0xFFU);
    gdt[idx].limit_low   = (u16)(limit & 0xFFFFU);
    gdt[idx].granularity = (u8)(((limit >> 16U) & 0x0FU) | (gran & 0xF0U));
    gdt[idx].access      = access;
}

static void tss_init(u32 kernel_stack)
{
    u32  base  = (u32)&tss;
    u32  limit = (u32)(sizeof(tss) - 1U);
    u32  i;
    u8  *p     = (u8 *)&tss;

    for (i = 0U; i < sizeof(tss); i++) {
        p[i] = 0U;
    }

    tss.ss0        = 0x10U;
    tss.esp0       = kernel_stack;
    tss.iomap_base = (u16)sizeof(tss);

    gdt_set_entry(5U, base, limit, 0x89U, 0x00U);
}

void gdt_init(u32 kernel_stack)
{
    gdt_ptr.limit = (u16)(sizeof(gdt) - 1U);
    gdt_ptr.base  = (u32)&gdt[0];

    gdt_set_entry(0U, 0U, 0U,        0x00U, 0x00U);
    gdt_set_entry(1U, 0U, 0xFFFFFU,  0x9AU, 0xCFU);
    gdt_set_entry(2U, 0U, 0xFFFFFU,  0x92U, 0xCFU);
    gdt_set_entry(3U, 0U, 0xFFFFFU,  0xFAU, 0xCFU);
    gdt_set_entry(4U, 0U, 0xFFFFFU,  0xF2U, 0xCFU);
    tss_init(kernel_stack);

    gdt_flush((u32)&gdt_ptr);
    tss_flush();
}
