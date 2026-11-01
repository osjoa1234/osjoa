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
    u64 base;
} __attribute__((packed));

struct tss_entry {
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

static struct gdt_entry   gdt[7];
static struct gdt_pointer gdt_ptr;
static struct tss_entry   tss;

extern void gdt_flush(void *ptr);
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

static void tss_init(u64 kernel_stack)
{
    u64  base  = (u64)&tss;
    u32  limit = (u32)(sizeof(tss) - 1U);
    u32  i;
    u8  *p     = (u8 *)&tss;

    for (i = 0U; i < sizeof(tss); i++) {
        p[i] = 0U;
    }

    tss.rsp0       = kernel_stack;
    tss.iomap_base = (u16)sizeof(tss);

    gdt_set_entry(5U, (u32)(base & 0xFFFFFFFFU), limit, 0x89U, 0x00U);
    *(u32 *)&gdt[6] = (u32)(base >> 32);
}

void gdt_set_kernel_stack(u64 rsp0)
{
    tss.rsp0 = rsp0;
}

#define IA32_FS_BASE 0xC0000100U

void gdt_set_fs_base(u64 base)
{
    u32 lo = (u32)(base & 0xFFFFFFFFULL);
    u32 hi = (u32)(base >> 32);
    __asm__ volatile ("wrmsr" : : "c"(IA32_FS_BASE), "a"(lo), "d"(hi));
}

#define IA32_EFER  0xC0000080U
#define IA32_STAR  0xC0000081U
#define IA32_LSTAR 0xC0000082U
#define IA32_FMASK 0xC0000084U

void gdt_enable_syscall(u64 handler)
{
    u32 efer_lo, efer_hi;
    u64 star = (u64)0x08ULL << 32;
    u32 lo, hi;

    __asm__ volatile ("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(IA32_EFER));
    efer_lo |= 1U;
    __asm__ volatile ("wrmsr" : : "c"(IA32_EFER), "a"(efer_lo), "d"(efer_hi));

    lo = (u32)(star & 0xFFFFFFFFULL);
    hi = (u32)(star >> 32);
    __asm__ volatile ("wrmsr" : : "c"(IA32_STAR), "a"(lo), "d"(hi));

    lo = (u32)(handler & 0xFFFFFFFFULL);
    hi = (u32)(handler >> 32);
    __asm__ volatile ("wrmsr" : : "c"(IA32_LSTAR), "a"(lo), "d"(hi));

    __asm__ volatile ("wrmsr" : : "c"(IA32_FMASK), "a"(0x200U), "d"(0U));
}

void gdt_init(u64 kernel_stack)
{
    gdt_ptr.limit = (u16)(sizeof(gdt) - 1U);
    gdt_ptr.base  = (u64)&gdt[0];

    gdt_set_entry(0U, 0U, 0U,        0x00U, 0x00U);
    gdt_set_entry(1U, 0U, 0xFFFFFU,  0x9AU, 0xAFU);
    gdt_set_entry(2U, 0U, 0xFFFFFU,  0x92U, 0xCFU);
    gdt_set_entry(3U, 0U, 0xFFFFFU,  0xFAU, 0xAFU);
    gdt_set_entry(4U, 0U, 0xFFFFFU,  0xF2U, 0xCFU);
    tss_init(kernel_stack);

    gdt_flush(&gdt_ptr);
    tss_flush();
}
