#include "virtio_net.h"
#include "pci.h"
#include "apic.h"
#include "interrupts.h"
#include "paging.h"
#include "phys_mem.h"

enum {
    VIRTIO_VENDOR_ID     = 0x1AF4U,
    VIRTIO_NET_DEVICE_ID = 0x1041U,

    PCI_CAP_VENDOR = 0x09U,

    VIRTIO_CAP_COMMON = 1U,
    VIRTIO_CAP_NOTIFY = 2U,
    VIRTIO_CAP_ISR    = 3U,
    VIRTIO_CAP_DEVICE = 4U,

    VIRTIO_STATUS_ACKNOWLEDGE = 1U << 0,
    VIRTIO_STATUS_DRIVER      = 1U << 1,
    VIRTIO_STATUS_DRIVER_OK   = 1U << 2,
    VIRTIO_STATUS_FEATURES_OK = 1U << 3,
    VIRTIO_STATUS_FAILED      = 1U << 7,

    VIRTIO_NET_F_MAC_BIT    = 5U,
    VIRTIO_F_VERSION_1_BIT  = 0U,

    VIRTQ_DESC_F_WRITE = 1U << 1,

    VIRTIO_NO_VECTOR = 0xFFFFU,

    VIRTIO_NET_RXQ = 0U,
    VIRTIO_NET_TXQ = 1U,

    VIRTIO_NET_RX_MSIX_ENTRY  = 0U,
    VIRTIO_NET_RX_MSIX_VECTOR = 0x32U,
    VIRTIO_NET_TX_MSIX_ENTRY  = 1U,
    VIRTIO_NET_TX_MSIX_VECTOR = 0x33U,

    VIRTIO_NET_HDR_SIZE  = 12U,
    VIRTIO_NET_FRAME_MAX = 1514U,
    VIRTIO_NET_BUF_SIZE  = VIRTIO_NET_HDR_SIZE + VIRTIO_NET_FRAME_MAX,

    VIRTIO_PAGE_SIZE = 0x1000U,

    ETH_HEADER_SIZE = 14U,
    ETH_TYPE_ARP    = 0x0806U,
    ARP_OP_REQUEST  = 1U,
    ARP_OP_REPLY    = 2U,
    ARP_FRAME_SIZE  = 42U,

    RFLAGS_IF = 1U << 9
};

#define VIRTIO_COMMON_VADDR (KERNEL_OFFSET + 0x41006000ULL)
#define VIRTIO_NOTIFY_VADDR (KERNEL_OFFSET + 0x41007000ULL)
#define VIRTIO_DEVICE_VADDR (KERNEL_OFFSET + 0x41008000ULL)

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

struct virtq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed));

struct virtq_avail {
    volatile u16 flags;
    volatile u16 idx;
    volatile u16 ring[];
} __attribute__((packed));

struct virtq_used_elem {
    volatile u32 id;
    volatile u32 len;
} __attribute__((packed));

struct virtq_used {
    volatile u16 flags;
    volatile u16 idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

struct virtq {
    u16 size;
    u16 index;
    u16 last_used;
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    u8  *buf;
    u32 buf_phys;
    volatile u16 *notify;
    volatile u32 irq_count;
};

static struct virtio_common_cfg *g_common;
static u8  *g_device_cfg;
static u64 g_notify_base;
static u32 g_notify_multiplier;
static u8  g_mac[6];
static struct virtq g_rxq;
static struct virtq g_txq;

static volatile u32 g_rx_frames;
static volatile u32 g_tx_done;
static volatile u32 g_arp_reply_ready;
static u8 g_arp_reply_mac[6];
static u8 g_arp_reply_ip[4];
static volatile u32 g_arp_reply_length;

static const u8 g_our_ip[4]     = { 10U, 0U, 2U, 15U };
static const u8 g_gateway_ip[4] = { 10U, 0U, 2U, 2U };

static void *virtio_phys_to_virt(u32 phys)
{
    return (void *)((u64)phys + KERNEL_OFFSET);
}

static u8 *virtio_map_region(u64 vaddr, u64 phys)
{
    page_map_mmio(vaddr, (u32)(phys & ~0xFFFULL));

    return (u8 *)(vaddr + (phys & 0xFFFULL));
}

static void virtio_zero(void *ptr, u32 bytes)
{
    u8 *p = (u8 *)ptr;
    u32 i;

    for (i = 0U; i < bytes; i++) p[i] = 0U;
}

static u32 virtio_alloc_zeroed(u32 bytes)
{
    u32 pages = (bytes + VIRTIO_PAGE_SIZE - 1U) / VIRTIO_PAGE_SIZE;
    u32 phys  = page_alloc_contig(pages);

    if (phys == 0U) return 0U;

    virtio_zero(virtio_phys_to_virt(phys), pages * VIRTIO_PAGE_SIZE);

    return phys;
}

static void virtio_notify(struct virtq *q)
{
    __asm__ volatile ("" : : : "memory");
    *q->notify = q->index;
}

static void virtio_wait(volatile u32 *flag)
{
    u64 flags;

    __asm__ volatile ("pushfq; pop %0" : "=r" (flags) : : "memory");

    for (;;) {
        __asm__ volatile ("cli" : : : "memory");
        if (*flag) break;
        __asm__ volatile ("sti; hlt" : : : "memory");
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");
}

static void virtio_net_rx_irq(void)
{
    struct virtq *q = &g_rxq;

    while (q->last_used != q->used->idx) {
        struct virtq_used_elem *elem = &q->used->ring[q->last_used % q->size];
        const u8 *frame = q->buf + (u64)elem->id * VIRTIO_NET_BUF_SIZE + VIRTIO_NET_HDR_SIZE;
        u32 length = elem->len > VIRTIO_NET_HDR_SIZE ? elem->len - VIRTIO_NET_HDR_SIZE : 0U;

        g_rx_frames++;

        if (length >= ARP_FRAME_SIZE && frame[12] == (ETH_TYPE_ARP >> 8U) && frame[13] == (ETH_TYPE_ARP & 0xFFU)
            && frame[20] == 0U && frame[21] == ARP_OP_REPLY) {
            u32 i;

            for (i = 0U; i < 6U; i++) g_arp_reply_mac[i] = frame[22U + i];
            for (i = 0U; i < 4U; i++) g_arp_reply_ip[i] = frame[28U + i];
            g_arp_reply_length = length;
            g_arp_reply_ready = 1U;
        }

        q->last_used++;
        q->avail->idx++;
    }

    q->irq_count++;
    virtio_notify(q);
}

static void virtio_net_tx_irq(void)
{
    struct virtq *q = &g_txq;

    while (q->last_used != q->used->idx) {
        q->last_used++;
        g_tx_done++;
    }

    q->irq_count++;
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
            g_common = (struct virtio_common_cfg *)virtio_map_region(VIRTIO_COMMON_VADDR, phys);
            have_common = 1;
            console_printf("virtio-net: common cfg bar=%u offset=0x%X length=0x%X\n", bar, offset, length);
        } else if (cfg_type == VIRTIO_CAP_NOTIFY) {
            g_notify_multiplier = pci_config_read32(bus, device, function, (u8)(cap + 16U));
            g_notify_base       = (u64)virtio_map_region(VIRTIO_NOTIFY_VADDR, phys);
            have_notify = 1;
            console_printf("virtio-net: notify bar=%u offset=0x%X length=0x%X multiplier=%u\n",
                           bar, offset, length, g_notify_multiplier);
        } else if (cfg_type == VIRTIO_CAP_DEVICE) {
            g_device_cfg = virtio_map_region(VIRTIO_DEVICE_VADDR, phys);
            have_device = 1;
            console_printf("virtio-net: device cfg bar=%u offset=0x%X length=0x%X\n", bar, offset, length);
        }
    }

    return have_common && have_notify && have_device ? 0 : -1;
}

static int virtio_negotiate_features(void)
{
    u32 device_low;
    u32 device_high;

    g_common->device_feature_select = 0U;
    device_low = g_common->device_feature;
    g_common->device_feature_select = 1U;
    device_high = g_common->device_feature;

    console_printf("virtio-net: device features low=0x%08X high=0x%08X\n", device_low, device_high);

    if (!(device_high & (1U << VIRTIO_F_VERSION_1_BIT))) return -1;
    if (!(device_low & (1U << VIRTIO_NET_F_MAC_BIT))) return -1;

    g_common->driver_feature_select = 0U;
    g_common->driver_feature = 1U << VIRTIO_NET_F_MAC_BIT;
    g_common->driver_feature_select = 1U;
    g_common->driver_feature = 1U << VIRTIO_F_VERSION_1_BIT;

    return 0;
}

static int virtq_setup(struct virtq *q, u16 index, u16 msix_entry)
{
    u32 base_phys;
    u32 desc_phys;
    u32 avail_phys;
    u32 used_phys;
    u32 avail_off;
    u32 used_off;
    u32 buf_off;
    u32 total;
    u32 notify_off;

    q->index = index;
    g_common->queue_select = index;
    q->size = g_common->queue_size;

    if (q->size == 0U) return -1;

    avail_off = 16U * q->size;
    used_off  = (avail_off + 6U + 2U * q->size + 3U) & ~3U;
    buf_off   = (used_off + 6U + 8U * q->size + 7U) & ~7U;
    total     = buf_off + (u32)VIRTIO_NET_BUF_SIZE * q->size;

    base_phys = virtio_alloc_zeroed(total);

    if (base_phys == 0U) return -1;

    desc_phys   = base_phys;
    avail_phys  = base_phys + avail_off;
    used_phys   = base_phys + used_off;
    q->buf_phys = base_phys + buf_off;

    q->desc  = (struct virtq_desc *)virtio_phys_to_virt(desc_phys);
    q->avail = (struct virtq_avail *)virtio_phys_to_virt(avail_phys);
    q->used  = (struct virtq_used *)virtio_phys_to_virt(used_phys);
    q->buf   = (u8 *)virtio_phys_to_virt(q->buf_phys);
    q->last_used = 0U;

    g_common->queue_desc_lo   = desc_phys;
    g_common->queue_desc_hi   = 0U;
    g_common->queue_driver_lo = avail_phys;
    g_common->queue_driver_hi = 0U;
    g_common->queue_device_lo = used_phys;
    g_common->queue_device_hi = 0U;

    notify_off = g_common->queue_notify_off;
    q->notify  = (volatile u16 *)(g_notify_base + (u64)notify_off * g_notify_multiplier);

    g_common->queue_enable = 1U;

    g_common->queue_msix_vector = msix_entry;
    if (g_common->queue_msix_vector == VIRTIO_NO_VECTOR) return -1;

    console_printf("virtio-net: queue %u size=%u desc=0x%08X avail=0x%08X used=0x%08X buf=0x%08X notify_off=%u msix entry=%u\n",
                   index, q->size, desc_phys, avail_phys, used_phys, q->buf_phys, notify_off, msix_entry);

    return 0;
}

static void virtq_fill_ring(struct virtq *q, u16 flags, u32 posted)
{
    u32 i;

    for (i = 0U; i < q->size; i++) {
        q->desc[i].addr  = (u64)q->buf_phys + (u64)i * VIRTIO_NET_BUF_SIZE;
        q->desc[i].len   = VIRTIO_NET_BUF_SIZE;
        q->desc[i].flags = flags;
        q->desc[i].next  = 0U;
        q->avail->ring[i] = (u16)i;
    }

    __asm__ volatile ("" : : : "memory");
    q->avail->idx = (u16)posted;
}

static int virtio_net_send(const u8 *frame, u32 length)
{
    struct virtq *q = &g_txq;
    u16 slot;
    u8 *buf;
    u32 i;

    if ((u16)(q->avail->idx - q->last_used) >= q->size) return -1;
    if (length > VIRTIO_NET_FRAME_MAX) return -1;

    slot = q->avail->idx % q->size;
    buf  = q->buf + (u64)slot * VIRTIO_NET_BUF_SIZE;

    virtio_zero(buf, VIRTIO_NET_HDR_SIZE);
    for (i = 0U; i < length; i++) buf[VIRTIO_NET_HDR_SIZE + i] = frame[i];

    q->desc[slot].len = VIRTIO_NET_HDR_SIZE + length;

    __asm__ volatile ("" : : : "memory");
    q->avail->idx++;
    virtio_notify(q);

    return 0;
}

static void virtio_net_arp_probe(void)
{
    u8 frame[ARP_FRAME_SIZE];
    u32 i;

    for (i = 0U; i < 6U; i++) {
        frame[i]      = 0xFFU;
        frame[6U + i] = g_mac[i];
    }
    frame[12] = ETH_TYPE_ARP >> 8U;
    frame[13] = ETH_TYPE_ARP & 0xFFU;

    frame[14] = 0x00U;
    frame[15] = 0x01U;
    frame[16] = 0x08U;
    frame[17] = 0x00U;
    frame[18] = 6U;
    frame[19] = 4U;
    frame[20] = 0x00U;
    frame[21] = ARP_OP_REQUEST;

    for (i = 0U; i < 6U; i++) {
        frame[22U + i] = g_mac[i];
        frame[32U + i] = 0x00U;
    }
    for (i = 0U; i < 4U; i++) {
        frame[28U + i] = g_our_ip[i];
        frame[38U + i] = g_gateway_ip[i];
    }

    console_printf("virtio-net: tx arp who-has %u.%u.%u.%u tell %u.%u.%u.%u\n",
                   g_gateway_ip[0], g_gateway_ip[1], g_gateway_ip[2], g_gateway_ip[3],
                   g_our_ip[0], g_our_ip[1], g_our_ip[2], g_our_ip[3]);

    if (virtio_net_send(frame, ARP_FRAME_SIZE) != 0) {
        console_set_color(0x0CU);
        console_printf("virtio-net: tx failed\n");
        return;
    }

    virtio_wait(&g_tx_done);
    virtio_wait(&g_arp_reply_ready);

    console_set_color(0x0AU);
    console_printf("virtio-net: rx arp reply %u.%u.%u.%u is-at %02X:%02X:%02X:%02X:%02X:%02X (%u bytes)\n",
                   g_arp_reply_ip[0], g_arp_reply_ip[1], g_arp_reply_ip[2], g_arp_reply_ip[3],
                   g_arp_reply_mac[0], g_arp_reply_mac[1], g_arp_reply_mac[2],
                   g_arp_reply_mac[3], g_arp_reply_mac[4], g_arp_reply_mac[5],
                   g_arp_reply_length);
}

void virtio_net_init(void)
{
    u8  bus;
    u8  device;
    u8  function;
    u32 i;
    u8  status;
    struct pci_msix msix;

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

    if (pci_msix_open(bus, device, function, &msix) != 0) {
        console_set_color(0x0CU);
        console_printf("virtio-net: no MSI-X capability\n");
        return;
    }

    interrupts_register_handler(VIRTIO_NET_RX_MSIX_VECTOR, virtio_net_rx_irq);
    interrupts_register_handler(VIRTIO_NET_TX_MSIX_VECTOR, virtio_net_tx_irq);
    pci_msix_set_entry(&msix, VIRTIO_NET_RX_MSIX_ENTRY, VIRTIO_NET_RX_MSIX_VECTOR, apic_id());
    pci_msix_set_entry(&msix, VIRTIO_NET_TX_MSIX_ENTRY, VIRTIO_NET_TX_MSIX_VECTOR, apic_id());
    pci_msix_start(&msix);

    console_printf("virtio-net: msix entry %u -> vector 0x%02X, entry %u -> vector 0x%02X apic id=%u\n",
                   VIRTIO_NET_RX_MSIX_ENTRY, VIRTIO_NET_RX_MSIX_VECTOR,
                   VIRTIO_NET_TX_MSIX_ENTRY, VIRTIO_NET_TX_MSIX_VECTOR, apic_id());

    g_common->device_status = 0U;
    g_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    g_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    if (virtio_negotiate_features() != 0) {
        g_common->device_status = VIRTIO_STATUS_FAILED;
        console_set_color(0x0CU);
        console_printf("virtio-net: feature negotiation failed\n");
        return;
    }

    g_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    status = g_common->device_status;

    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        g_common->device_status = VIRTIO_STATUS_FAILED;
        console_set_color(0x0CU);
        console_printf("virtio-net: device rejected features status=0x%02X\n", status);
        return;
    }

    console_printf("virtio-net: features ok (VERSION_1 + MAC) status=0x%02X num_queues=%u\n",
                   status, g_common->num_queues);

    g_common->msix_config = VIRTIO_NO_VECTOR;

    if (virtq_setup(&g_rxq, VIRTIO_NET_RXQ, VIRTIO_NET_RX_MSIX_ENTRY) != 0 ||
        virtq_setup(&g_txq, VIRTIO_NET_TXQ, VIRTIO_NET_TX_MSIX_ENTRY) != 0) {
        g_common->device_status = VIRTIO_STATUS_FAILED;
        console_set_color(0x0CU);
        console_printf("virtio-net: queue setup failed\n");
        return;
    }

    virtq_fill_ring(&g_rxq, VIRTQ_DESC_F_WRITE, g_rxq.size);
    virtq_fill_ring(&g_txq, 0U, 0U);

    for (i = 0U; i < 6U; i++) g_mac[i] = g_device_cfg[i];

    console_set_color(0x0AU);
    console_printf("virtio-net: mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    g_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                              VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;

    virtio_notify(&g_rxq);

    console_printf("virtio-net: driver ok status=0x%02X, rxq posted %u buffers\n",
                   g_common->device_status, g_rxq.size);

    virtio_net_arp_probe();
}

void virtio_net_report(void)
{
    console_set_color(0x0AU);
    console_printf("virtio-net: completions via msix: rx %u interrupt(s) (%u frame(s)), tx %u interrupt(s)\n",
                   g_rxq.irq_count, g_rx_frames, g_txq.irq_count);
}
