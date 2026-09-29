#include "pci.h"

enum {
    PCI_CONFIG_ADDRESS = 0xCF8U,
    PCI_CONFIG_DATA    = 0xCFCU,

    PCI_VENDOR_INVALID = 0xFFFFU,

    PCI_OFF_VENDOR_ID   = 0x00U,
    PCI_OFF_DEVICE_ID   = 0x02U,
    PCI_OFF_STATUS      = 0x06U,
    PCI_OFF_PROG_IF     = 0x09U,
    PCI_OFF_SUBCLASS    = 0x0AU,
    PCI_OFF_CLASS       = 0x0BU,
    PCI_OFF_HEADER_TYPE = 0x0EU,
    PCI_OFF_BAR0        = 0x10U,
    PCI_OFF_CAP_POINTER = 0x34U,

    PCI_STATUS_CAP_LIST  = 1U << 4,
    PCI_HEADER_MULTIFUNC = 1U << 7,

    PCI_BAR_COUNT    = 6U,
    PCI_MAX_BUS      = 256U,
    PCI_MAX_DEVICE   = 32U,
    PCI_MAX_FUNCTION = 8U
};

static u32 device_count;

static void outl(u16 port, u32 value)
{
    __asm__ volatile ("outl %0, %1" : : "a" (value), "Nd" (port));
}

static u32 inl(u16 port)
{
    u32 value;

    __asm__ volatile ("inl %1, %0" : "=a" (value) : "Nd" (port));

    return value;
}

static u32 pci_config_read32(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 address = (1U << 31) | ((u32)bus << 16) | ((u32)device << 11) |
                  ((u32)function << 8) | (offset & 0xFCU);

    outl(PCI_CONFIG_ADDRESS, address);

    return inl(PCI_CONFIG_DATA);
}

static u16 pci_config_read16(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 value = pci_config_read32(bus, device, function, (u8)(offset & 0xFCU));

    return (u16)((value >> ((offset & 2U) * 8U)) & 0xFFFFU);
}

static u8 pci_config_read8(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 value = pci_config_read32(bus, device, function, (u8)(offset & 0xFCU));

    return (u8)((value >> ((offset & 3U) * 8U)) & 0xFFU);
}

static const char *pci_class_name(u8 class_code)
{
    switch (class_code) {
        case 0x01U: return "storage";
        case 0x02U: return "network";
        case 0x03U: return "display";
        case 0x06U: return "bridge";
        case 0x0CU: return "serial bus";
        default:    return "other";
    }
}

static const char *pci_capability_name(u8 cap_id)
{
    switch (cap_id) {
        case 0x01U: return "power management";
        case 0x05U: return "MSI";
        case 0x10U: return "PCI Express";
        case 0x11U: return "MSI-X";
        default:    return "unknown";
    }
}

static void pci_scan_bars(u8 bus, u8 device, u8 function)
{
    u32 bar_index;

    for (bar_index = 0U; bar_index < PCI_BAR_COUNT; bar_index++) {
        u8  offset = (u8)(PCI_OFF_BAR0 + bar_index * 4U);
        u32 bar    = pci_config_read32(bus, device, function, offset);

        if (bar == 0U) continue;

        if (bar & 1U) {
            console_printf("        BAR%u: I/O port base=0x%04X\n",
                            bar_index, bar & 0xFFFFFFFCU);
        } else {
            u32 type          = (bar >> 1U) & 0x3U;
            u32 prefetchable  = (bar >> 3U) & 0x1U;
            u64 base          = bar & 0xFFFFFFF0U;

            if (type == 2U && bar_index + 1U < PCI_BAR_COUNT) {
                u32 upper = pci_config_read32(bus, device, function, (u8)(offset + 4U));

                base |= (u64)upper << 32U;
                console_printf("        BAR%u: MMIO64 base=0x%016lX prefetch=%u\n",
                                bar_index, base, prefetchable);
                bar_index++;
            } else {
                console_printf("        BAR%u: MMIO32 base=0x%08X prefetch=%u\n",
                                bar_index, (u32)base, prefetchable);
            }
        }
    }
}

static void pci_scan_capabilities(u8 bus, u8 device, u8 function)
{
    u16 status  = pci_config_read16(bus, device, function, PCI_OFF_STATUS);
    u8  pointer;

    if (!(status & PCI_STATUS_CAP_LIST)) return;

    pointer = pci_config_read8(bus, device, function, PCI_OFF_CAP_POINTER) & 0xFCU;

    while (pointer != 0U) {
        u8 cap_id = pci_config_read8(bus, device, function, pointer);
        u8 next   = pci_config_read8(bus, device, function, (u8)(pointer + 1U));

        console_printf("        cap 0x%02X: %s\n", cap_id, pci_capability_name(cap_id));

        pointer = next & 0xFCU;
    }
}

static int pci_scan_function(u8 bus, u8 device, u8 function)
{
    u16 vendor_id = pci_config_read16(bus, device, function, PCI_OFF_VENDOR_ID);
    u16 device_id;
    u8  prog_if;
    u8  subclass;
    u8  class_code;

    if (vendor_id == PCI_VENDOR_INVALID) return 0;

    device_id  = pci_config_read16(bus, device, function, PCI_OFF_DEVICE_ID);
    prog_if    = pci_config_read8(bus, device, function, PCI_OFF_PROG_IF);
    subclass   = pci_config_read8(bus, device, function, PCI_OFF_SUBCLASS);
    class_code = pci_config_read8(bus, device, function, PCI_OFF_CLASS);

    console_printf("    %02X:%02X.%X vendor=0x%04X device=0x%04X class=0x%02X/%02X/%02X (%s)\n",
                   bus, device, function, vendor_id, device_id,
                   class_code, subclass, prog_if, pci_class_name(class_code));

    pci_scan_bars(bus, device, function);
    pci_scan_capabilities(bus, device, function);

    device_count++;

    return 1;
}

static void pci_scan_device(u8 bus, u8 device)
{
    u8 header_type;
    u32 function;

    if (!pci_scan_function(bus, device, 0U)) return;

    header_type = pci_config_read8(bus, device, 0U, PCI_OFF_HEADER_TYPE);

    if (!(header_type & PCI_HEADER_MULTIFUNC)) return;

    for (function = 1U; function < PCI_MAX_FUNCTION; function++) {
        pci_scan_function(bus, device, (u8)function);
    }
}

void pci_scan(void)
{
    u32 bus;
    u32 device;

    device_count = 0U;

    console_set_color(0x0BU);
    console_printf("pci: scanning configuration space via 0x%03X/0x%03X\n",
                   PCI_CONFIG_ADDRESS, PCI_CONFIG_DATA);

    for (bus = 0U; bus < PCI_MAX_BUS; bus++) {
        for (device = 0U; device < PCI_MAX_DEVICE; device++) {
            pci_scan_device((u8)bus, (u8)device);
        }
    }

    console_set_color(0x0BU);
    console_printf("pci: %u device(s) found\n", device_count);
}
