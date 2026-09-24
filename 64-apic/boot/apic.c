#include "apic.h"
#include "acpi.h"
#include "paging.h"

enum {
    IA32_APIC_BASE_MSR = 0x1BU,

    XAPIC_REG_ID  = 0x020U,
    XAPIC_REG_EOI = 0x0B0U,
    XAPIC_REG_SVR = 0x0F0U,

    APIC_SVR_SOFT_ENABLE = 0x100U,
    APIC_SPURIOUS_VECTOR = 0xFFU,

    IOAPIC_REG_IOREGSEL = 0x00U,
    IOAPIC_REG_IOWIN    = 0x10U,
    IOAPIC_REG_VER      = 0x01U,
    IOAPIC_REG_REDTBL   = 0x10U,

    IOAPIC_REDTBL_MASKED     = 1U << 16,
    IOAPIC_REDTBL_LEVEL      = 1U << 15,
    IOAPIC_REDTBL_ACTIVE_LOW = 1U << 13
};

#define LAPIC_MMIO_VADDR  (KERNEL_OFFSET + 0x41000000ULL)
#define IOAPIC_MMIO_VADDR (KERNEL_OFFSET + 0x41001000ULL)

static int use_x2apic;
static volatile u32 *lapic_mmio;
static volatile u32 *ioapic_mmio;

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf));
}

static u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | (u64)lo;
}

static void wrmsr(u32 msr, u64 value)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((u32)value), "d"((u32)(value >> 32)));
}

static volatile u32 *map_mmio(u64 vaddr, u32 paddr)
{
    page_map_mmio(vaddr, paddr);
    return (volatile u32 *)vaddr;
}

static u32 lapic_read(u32 reg)
{
    if (use_x2apic) return (u32)rdmsr(0x800U + (reg >> 4));
    return lapic_mmio[reg / 4U];
}

static void lapic_write(u32 reg, u32 value)
{
    if (use_x2apic) {
        wrmsr(0x800U + (reg >> 4), value);
        return;
    }
    lapic_mmio[reg / 4U] = value;
}

void apic_init(void)
{
    u32 a, b, c, d;

    cpuid(1U, &a, &b, &c, &d);
    use_x2apic = (c & (1U << 21)) ? 1 : 0;

    if (use_x2apic) {
        u64 base = rdmsr(IA32_APIC_BASE_MSR);
        wrmsr(IA32_APIC_BASE_MSR, base | (1ULL << 10) | (1ULL << 11));
    } else {
        lapic_mmio = map_mmio(LAPIC_MMIO_VADDR, acpi_lapic_address());
    }

    lapic_write(XAPIC_REG_SVR, APIC_SVR_SOFT_ENABLE | APIC_SPURIOUS_VECTOR);

    console_set_color(0x0BU);
    console_printf("apic: %s enabled, local apic id=%u\n",
                   use_x2apic ? "x2APIC (MSR)" : "xAPIC (MMIO)",
                   apic_id());
}

u32 apic_id(void)
{
    if (use_x2apic) return lapic_read(XAPIC_REG_ID);
    return lapic_read(XAPIC_REG_ID) >> 24;
}

void apic_eoi(void)
{
    lapic_write(XAPIC_REG_EOI, 0U);
}

static u32 ioapic_read(u8 reg)
{
    ioapic_mmio[IOAPIC_REG_IOREGSEL / 4U] = reg;
    return ioapic_mmio[IOAPIC_REG_IOWIN / 4U];
}

static void ioapic_write(u8 reg, u32 value)
{
    ioapic_mmio[IOAPIC_REG_IOREGSEL / 4U] = reg;
    ioapic_mmio[IOAPIC_REG_IOWIN / 4U] = value;
}

void ioapic_init(void)
{
    u32 ver;
    u32 max_entry;
    u32 i;

    ioapic_mmio = map_mmio(IOAPIC_MMIO_VADDR, acpi_ioapic_address());

    ver = ioapic_read(IOAPIC_REG_VER);
    max_entry = (ver >> 16) & 0xFFU;

    for (i = 0U; i <= max_entry; i++) {
        ioapic_write((u8)(IOAPIC_REG_REDTBL + i * 2U), IOAPIC_REDTBL_MASKED);
        ioapic_write((u8)(IOAPIC_REG_REDTBL + i * 2U + 1U), 0U);
    }

    console_set_color(0x0BU);
    console_printf("ioapic: base=0x%08X gsi_base=%u entries=%u masked\n",
                   acpi_ioapic_address(), acpi_ioapic_gsi_base(), max_entry + 1U);
}

void ioapic_unmask_irq(u8 irq, u8 vector)
{
    u32 gsi   = acpi_irq_to_gsi(irq);
    u32 index = gsi - acpi_ioapic_gsi_base();
    u32 low   = vector;
    u32 high  = apic_id() << 24;

    if (acpi_irq_active_low(irq))      low |= IOAPIC_REDTBL_ACTIVE_LOW;
    if (acpi_irq_level_triggered(irq)) low |= IOAPIC_REDTBL_LEVEL;

    ioapic_write((u8)(IOAPIC_REG_REDTBL + index * 2U + 1U), high);
    ioapic_write((u8)(IOAPIC_REG_REDTBL + index * 2U), low);
}
