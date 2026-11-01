#include "acpi.h"

struct acpi_rsdp {
    char sig[8];
    u8   checksum;
    char oem_id[6];
    u8   revision;
    u32  rsdt_address;
    u32  length;
    u64  xsdt_address;
    u8   ext_checksum;
    u8   reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char sig[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    u32 local_apic_address;
    u32 flags;
} __attribute__((packed));

struct madt_entry_header {
    u8 type;
    u8 length;
} __attribute__((packed));

struct madt_local_apic {
    struct madt_entry_header header;
    u8  acpi_processor_id;
    u8  apic_id;
    u32 flags;
} __attribute__((packed));

struct madt_io_apic {
    struct madt_entry_header header;
    u8  ioapic_id;
    u8  reserved;
    u32 ioapic_address;
    u32 gsi_base;
} __attribute__((packed));

struct madt_iso {
    struct madt_entry_header header;
    u8  bus;
    u8  source;
    u32 gsi;
    u16 flags;
} __attribute__((packed));

struct madt_lapic_override {
    struct madt_entry_header header;
    u16 reserved;
    u64 address;
} __attribute__((packed));

enum {
    MADT_TYPE_LOCAL_APIC          = 0U,
    MADT_TYPE_IO_APIC             = 1U,
    MADT_TYPE_INT_SRC_OVERRIDE    = 2U,
    MADT_TYPE_LAPIC_ADDR_OVERRIDE = 5U
};

struct acpi_mcfg_entry {
    u64 base_address;
    u16 pci_segment_group;
    u8  start_bus;
    u8  end_bus;
    u32 reserved;
} __attribute__((packed));

#define ACPI_MAX_CPUS 16U

static u32 g_bsp_apic_id;
static int g_bsp_found;

static u32 g_cpu_apic_id[ACPI_MAX_CPUS];
static u8  g_cpu_enabled[ACPI_MAX_CPUS];
static u32 g_cpu_count;

static u32 g_lapic_address = 0xFEE00000U;

static u32 g_ioapic_id;
static u32 g_ioapic_address;
static u32 g_ioapic_gsi_base;

static u32 g_irq_to_gsi[16];
static u8  g_irq_active_low[16];
static u8  g_irq_level_triggered[16];

static u64 g_mcfg_base;
static u8  g_mcfg_start_bus;
static u8  g_mcfg_end_bus;
static int g_mcfg_found;

static const struct acpi_sdt_header *acpi_table_at(u64 phys)
{
    return (const struct acpi_sdt_header *)(phys + KERNEL_OFFSET);
}

static int acpi_sig_is(const struct acpi_sdt_header *table, const char *sig)
{
    return table->sig[0] == sig[0] && table->sig[1] == sig[1] &&
           table->sig[2] == sig[2] && table->sig[3] == sig[3];
}

static void madt_parse(const struct acpi_sdt_header *header)
{
    const struct acpi_madt *madt = (const struct acpi_madt *)header;
    const u8 *base = (const u8 *)madt;
    const u8 *end  = base + madt->header.length;
    const u8 *ptr  = base + sizeof(struct acpi_madt);

    g_lapic_address = madt->local_apic_address;

    while (ptr < end) {
        const struct madt_entry_header *eh = (const struct madt_entry_header *)ptr;

        if (eh->type == MADT_TYPE_LOCAL_APIC) {
            const struct madt_local_apic *la = (const struct madt_local_apic *)ptr;

            if (g_cpu_count < ACPI_MAX_CPUS) {
                g_cpu_apic_id[g_cpu_count] = la->apic_id;
                g_cpu_enabled[g_cpu_count] = (u8)(la->flags & 1U);
                g_cpu_count++;
            }

            if (!g_bsp_found && (la->flags & 1U)) {
                g_bsp_apic_id = la->apic_id;
                g_bsp_found = 1;
            }
        } else if (eh->type == MADT_TYPE_IO_APIC) {
            const struct madt_io_apic *io = (const struct madt_io_apic *)ptr;

            g_ioapic_id       = io->ioapic_id;
            g_ioapic_address  = io->ioapic_address;
            g_ioapic_gsi_base = io->gsi_base;
        } else if (eh->type == MADT_TYPE_INT_SRC_OVERRIDE) {
            const struct madt_iso *iso = (const struct madt_iso *)ptr;

            if (iso->source < 16U) {
                u8 polarity = (u8)(iso->flags & 0x3U);
                u8 trigger  = (u8)((iso->flags >> 2) & 0x3U);

                g_irq_to_gsi[iso->source]         = iso->gsi;
                g_irq_active_low[iso->source]     = (polarity == 3U) ? 1U : 0U;
                g_irq_level_triggered[iso->source] = (trigger == 3U) ? 1U : 0U;
            }
        } else if (eh->type == MADT_TYPE_LAPIC_ADDR_OVERRIDE) {
            const struct madt_lapic_override *ov = (const struct madt_lapic_override *)ptr;

            g_lapic_address = (u32)ov->address;
        }

        ptr += eh->length;
    }
}

static void mcfg_parse(const struct acpi_sdt_header *header)
{
    const u8 *base = (const u8 *)header;
    const struct acpi_mcfg_entry *entry =
        (const struct acpi_mcfg_entry *)(base + sizeof(struct acpi_sdt_header) + 8U);

    g_mcfg_base      = entry->base_address;
    g_mcfg_start_bus = entry->start_bus;
    g_mcfg_end_bus   = entry->end_bus;
    g_mcfg_found     = 1;
}

static void acpi_scan_tables(const struct acpi_sdt_header *root, int is_xsdt)
{
    const u8 *entries = (const u8 *)root + sizeof(struct acpi_sdt_header);
    u32 stride = is_xsdt ? 8U : 4U;
    u32 count  = (root->length - (u32)sizeof(struct acpi_sdt_header)) / stride;
    u32 i;

    for (i = 0U; i < count; i++) {
        u64 table_phys = is_xsdt ? *(const u64 *)(entries + i * 8U)
                                  : (u64) * (const u32 *)(entries + i * 4U);
        const struct acpi_sdt_header *table = acpi_table_at(table_phys);

        if (acpi_sig_is(table, "APIC")) {
            madt_parse(table);
        } else if (acpi_sig_is(table, "MCFG")) {
            mcfg_parse(table);
        }
    }
}

void acpi_init(const void *rsdp_bytes)
{
    const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)rsdp_bytes;
    u32 i;

    for (i = 0U; i < 16U; i++) {
        g_irq_to_gsi[i]          = i;
        g_irq_active_low[i]      = 0U;
        g_irq_level_triggered[i] = 0U;
    }

    if (!rsdp) return;

    if (rsdp->revision >= 2U) {
        acpi_scan_tables(acpi_table_at(rsdp->xsdt_address), 1);
    } else {
        acpi_scan_tables(acpi_table_at((u64)rsdp->rsdt_address), 0);
    }
}

u32 acpi_bsp_apic_id(void)   { return g_bsp_apic_id; }
u32 acpi_lapic_address(void) { return g_lapic_address; }

u32 acpi_cpu_count(void)            { return g_cpu_count; }
u32 acpi_cpu_apic_id(u32 index)     { return (index < g_cpu_count) ? g_cpu_apic_id[index] : 0U; }
u8  acpi_cpu_enabled(u32 index)     { return (index < g_cpu_count) ? g_cpu_enabled[index] : 0U; }

u32 acpi_ioapic_id(void)        { return g_ioapic_id; }
u32 acpi_ioapic_address(void)   { return g_ioapic_address; }
u32 acpi_ioapic_gsi_base(void)  { return g_ioapic_gsi_base; }

u32 acpi_irq_to_gsi(u8 isa_irq)          { return g_irq_to_gsi[isa_irq]; }
u8  acpi_irq_active_low(u8 isa_irq)      { return g_irq_active_low[isa_irq]; }
u8  acpi_irq_level_triggered(u8 isa_irq) { return g_irq_level_triggered[isa_irq]; }

int acpi_mcfg_found(void)     { return g_mcfg_found; }
u64 acpi_mcfg_base(void)      { return g_mcfg_base; }
u8  acpi_mcfg_start_bus(void) { return g_mcfg_start_bus; }
u8  acpi_mcfg_end_bus(void)   { return g_mcfg_end_bus; }
