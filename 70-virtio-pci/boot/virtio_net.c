#include "virtio_net.h"
#include "pci.h"
#include "paging.h"

enum {
    VIRTIO_VENDOR_ID     = 0x1AF4U,
    VIRTIO_NET_DEVICE_ID = 0x1041U,

    PCI_CAP_VENDOR = 0x09U,

    VIRTIO_CAP_COMMON = 1U,
    VIRTIO_CAP_NOTIFY = 2U,
    VIRTIO_CAP_DEVICE = 4U,

    VIRTIO_REGION_PAGES = 4U
};

#define VIRTIO_COMMON_VADDR (KERNEL_OFFSET + 0x41006000ULL)
#define VIRTIO_NOTIFY_VADDR (KERNEL_OFFSET + 0x4100A000ULL)
#define VIRTIO_DEVICE_VADDR (KERNEL_OFFSET + 0x4100E000ULL)

struct virtio_common_cfg {
    volatile u32 device_feature_select;
    volatile u32 device_feature;
    volatile u32 driver_feature_select;
    volatile u32 driver_feature;
    volatile u16 msix_config;
    volatile u16 num_queues;
    volatile u8  device_status;
    volatile u8  config_generation;
    volatile u16 queue_select;
    volatile u16 queue_size;
    volatile u16 queue_msix_vector;
    volatile u16 queue_enable;
    volatile u16 queue_notify_off;
    volatile u32 queue_desc_lo;
    volatile u32 queue_desc_hi;
    volatile u32 queue_driver_lo;
    volatile u32 queue_driver_hi;
    volatile u32 queue_device_lo;
    volatile u32 queue_device_hi;
} __attribute__((packed));

static struct virtio_common_cfg *g_common;
static u8  *g_device_cfg;
static u64 g_notify_base;
static u32 g_notify_multiplier;

static u8 *virtio_map_region(u64 vaddr, u64 phys, u32 length)
{
    u32 pages = (u32)(((phys & 0xFFFULL) + length + 0xFFFULL) >> 12);
    u32 i;

    if (pages > VIRTIO_REGION_PAGES) {
        return 0;
    }

    for (i = 0U; i < pages; i++) {
        page_map_mmio(vaddr + ((u64)i << 12), (u32)((phys & ~0xFFFULL) + ((u64)i << 12)));
    }

    return (u8 *)(vaddr + (phys & 0xFFFULL));
}

static int virtio_find_capabilities(u8 bus, u8 device, u8 function)
{
    u8 cap = 0U;
    int have_common = 0;
    int have_notify = 0;
    int have_device = 0;

    while ((cap = pci_next_capability(bus, device, function, cap, PCI_CAP_VENDOR)) != 0U) {
        u8  cfg_type = pci_config_read8(bus, device, function, (u8)(cap + 3U));
        u8  bar      = pci_config_read8(bus, device, function, (u8)(cap + 4U));
        u32 offset   = pci_config_read32(bus, device, function, (u8)(cap + 8U));
        u32 length   = pci_config_read32(bus, device, function, (u8)(cap + 12U));
        u64 phys     = pci_bar_address(bus, device, function, bar) + offset;

        if (cfg_type == VIRTIO_CAP_COMMON) {
            g_common = (struct virtio_common_cfg *)virtio_map_region(VIRTIO_COMMON_VADDR, phys, length);
            have_common = g_common != 0;
            console_printf("virtio-net: common cfg bar=%u offset=0x%X length=0x%X\n", bar, offset, length);
        } else if (cfg_type == VIRTIO_CAP_NOTIFY) {
            g_notify_multiplier = pci_config_read32(bus, device, function, (u8)(cap + 16U));
            g_notify_base       = (u64)virtio_map_region(VIRTIO_NOTIFY_VADDR, phys, length);
            have_notify = g_notify_base != 0ULL;
            console_printf("virtio-net: notify bar=%u offset=0x%X length=0x%X multiplier=%u\n",
                           bar, offset, length, g_notify_multiplier);
        } else if (cfg_type == VIRTIO_CAP_DEVICE) {
            g_device_cfg = virtio_map_region(VIRTIO_DEVICE_VADDR, phys, length);
            have_device = g_device_cfg != 0;
            console_printf("virtio-net: device cfg bar=%u offset=0x%X length=0x%X\n", bar, offset, length);
        }
    }

    return have_common && have_notify && have_device ? 0 : -1;
}

void virtio_net_init(void)
{
    u8  bus;
    u8  device;
    u8  function;

    console_set_color(0x0BU);

    if (!pci_find_by_id(VIRTIO_VENDOR_ID, VIRTIO_NET_DEVICE_ID, &bus, &device, &function)) {
        console_set_color(0x0CU);
        console_printf("virtio-net: no virtio-net device found\n");
        return;
    }

    console_printf("virtio-net: found device at %02X:%02X.%X\n", bus, device, function);

    pci_enable_device(bus, device, function);

    if (virtio_find_capabilities(bus, device, function) != 0) {
        console_set_color(0x0CU);
        console_printf("virtio-net: missing vendor capability\n");
        return;
    }

    console_set_color(0x0AU);
    console_printf("virtio-net: common cfg readable num_queues=%u\n", g_common->num_queues);
}
