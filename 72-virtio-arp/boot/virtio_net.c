#include "virtio_net.h"
#include "pci.h"
#include "paging.h"
#include "phys_mem.h"
#include "apic.h"
#include "interrupts.h"

enum {
    VIRTIO_VENDOR_ID     = 0x1AF4U,
    VIRTIO_NET_DEVICE_ID = 0x1041U,

    PCI_CAP_VENDOR = 0x09U,

    VIRTIO_CAP_COMMON = 1U,
    VIRTIO_CAP_NOTIFY = 2U,
    VIRTIO_CAP_DEVICE = 4U,

    VIRTIO_REGION_PAGES = 4U,

    VIRTIO_STATUS_ACKNOWLEDGE = 1U << 0,
    VIRTIO_STATUS_DRIVER      = 1U << 1,
    VIRTIO_STATUS_DRIVER_OK   = 1U << 2,
    VIRTIO_STATUS_FEATURES_OK = 1U << 3,
    VIRTIO_STATUS_FAILED      = 1U << 7,

    VIRTIO_NET_F_MAC_BIT    = 5U,
    VIRTIO_F_VERSION_1_BIT  = 0U,

    VIRTQ_DESC_F_WRITE = 1U << 1,

    VIRTIO_NO_VECTOR = 0xFFFFU,

    VIRTIO_RECEIVEQ  = 0U,
    VIRTIO_TRANSMITQ = 1U,

    VIRTIO_RX_MSIX_ENTRY  = 0U,
    VIRTIO_RX_MSIX_VECTOR = 0x32U,
    VIRTIO_TX_MSIX_ENTRY  = 1U,
    VIRTIO_TX_MSIX_VECTOR = 0x33U,

    VIRTIO_PAGE_SIZE      = 0x1000U,
    VIRTIO_PACKET_BUF     = 2048U,
    VIRTIO_RX_BUFFERS     = 16U,
    VIRTIO_RX_BUF_PAGES   = VIRTIO_RX_BUFFERS * VIRTIO_PACKET_BUF / VIRTIO_PAGE_SIZE,

    VIRTIO_STATUS_WAIT_SPINS = 1000000U,

    RFLAGS_IF = 1U << 9,

    VIRTIO_NET_HDR_LEN = 12U,
    ETH_HDR_LEN        = 14U,
    ARP_PACKET_LEN     = 28U,
    ETHERTYPE_ARP      = 0x0806U,
    ARP_HTYPE_ETHERNET = 1U,
    ARP_PTYPE_IPV4     = 0x0800U,
    ARP_OP_REQUEST     = 1U,
    ARP_OP_REPLY       = 2U,

    VIRTIO_ARP_WAIT_SPINS = 2000000U
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
    u32 id;
    u32 len;
} __attribute__((packed));

struct virtq_used {
    volatile u16 flags;
    volatile u16 idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

struct virtqueue {
    u16 index;
    u16 size;
    u16 notify_off;
    u16 avail_idx;
    u16 used_seen;
    u32 phys;
    u32 pages;
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    volatile u32 irq_count;
};

static struct virtio_common_cfg *g_common;
static u8  *g_device_cfg;
static u64 g_notify_base;
static u32 g_notify_multiplier;

static struct virtqueue g_rx;
static struct virtqueue g_tx;
static u32 g_rx_buf_phys;
static u32 g_tx_buf_phys;
static u8  g_mac[6];
static int g_arp_reply_seen;

static const u8 g_guest_ip[4]   = { 10U, 0U, 2U, 15U };
static const u8 g_gateway_ip[4] = { 10U, 0U, 2U, 2U };

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

static void virtio_rx_irq(void)
{
    g_rx.irq_count++;
}

static void virtio_tx_irq(void)
{
    g_tx.irq_count++;
}

static u32 virtio_align_up(u32 value, u32 align)
{
    return (value + align - 1U) & ~(align - 1U);
}

static int virtio_wait_status(u8 want)
{
    u32 spins = 0U;

    while (g_common->device_status != want) {
        spins++;
        if (spins > VIRTIO_STATUS_WAIT_SPINS) return -1;
    }

    return 0;
}

static void virtio_add_status(u8 bits)
{
    g_common->device_status = (u8)(g_common->device_status | bits);
}

static int virtio_negotiate_features(void)
{
    u32 low;
    u32 high;

    g_common->device_feature_select = 0U;
    low = g_common->device_feature;
    g_common->device_feature_select = 1U;
    high = g_common->device_feature;

    console_printf("virtio-net: device features low=0x%08X high=0x%08X\n", low, high);

    if (!(low & (1U << VIRTIO_NET_F_MAC_BIT)) || !(high & (1U << VIRTIO_F_VERSION_1_BIT))) return -1;

    g_common->driver_feature_select = 0U;
    g_common->driver_feature = 1U << VIRTIO_NET_F_MAC_BIT;
    g_common->driver_feature_select = 1U;
    g_common->driver_feature = 1U << VIRTIO_F_VERSION_1_BIT;

    virtio_add_status(VIRTIO_STATUS_FEATURES_OK);

    return (g_common->device_status & VIRTIO_STATUS_FEATURES_OK) ? 0 : -1;
}

static int virtqueue_setup(struct virtqueue *q, u16 index, u16 msix_entry)
{
    u32 size;
    u32 avail_off;
    u32 used_off;
    u32 total;
    u32 i;
    u8 *bytes;

    g_common->queue_select = index;
    size = g_common->queue_size;
    if (size == 0U) return -1;

    avail_off = size * (u32)sizeof(struct virtq_desc);
    used_off  = virtio_align_up(avail_off + 6U + 2U * size, VIRTIO_PAGE_SIZE);
    total     = used_off + 6U + size * (u32)sizeof(struct virtq_used_elem);

    q->index = index;
    q->size  = (u16)size;
    q->pages = virtio_align_up(total, VIRTIO_PAGE_SIZE) / VIRTIO_PAGE_SIZE;
    q->phys  = page_alloc_contig(q->pages);
    if (q->phys == 0U) return -1;

    bytes = (u8 *)((u64)q->phys + KERNEL_OFFSET);
    for (i = 0U; i < q->pages * VIRTIO_PAGE_SIZE; i++) bytes[i] = 0U;

    q->desc  = (struct virtq_desc *)bytes;
    q->avail = (struct virtq_avail *)(bytes + avail_off);
    q->used  = (struct virtq_used *)(bytes + used_off);
    q->avail_idx = 0U;
    q->used_seen = 0U;
    q->irq_count = 0U;

    g_common->queue_desc_lo   = q->phys;
    g_common->queue_desc_hi   = 0U;
    g_common->queue_driver_lo = q->phys + avail_off;
    g_common->queue_driver_hi = 0U;
    g_common->queue_device_lo = q->phys + used_off;
    g_common->queue_device_hi = 0U;

    g_common->queue_msix_vector = msix_entry;
    if (g_common->queue_msix_vector != msix_entry) return -1;

    q->notify_off = g_common->queue_notify_off;
    g_common->queue_enable = 1U;

    console_printf("virtio-net: queue %u size=%u pages=%u desc=0x%X avail=0x%X used=0x%X notify_off=%u msix entry=%u\n",
                   index, size, q->pages, q->phys, q->phys + avail_off, q->phys + used_off,
                   q->notify_off, msix_entry);

    return 0;
}

static void virtqueue_notify(struct virtqueue *q)
{
    volatile u16 *doorbell = (volatile u16 *)(g_notify_base + (u64)q->notify_off * g_notify_multiplier);

    *doorbell = q->index;
}

static int virtio_post_rx_buffers(void)
{
    u32 i;
    u8 *bytes;

    g_rx_buf_phys = page_alloc_contig(VIRTIO_RX_BUF_PAGES);
    if (g_rx_buf_phys == 0U) return -1;

    bytes = (u8 *)((u64)g_rx_buf_phys + KERNEL_OFFSET);
    for (i = 0U; i < VIRTIO_RX_BUF_PAGES * VIRTIO_PAGE_SIZE; i++) bytes[i] = 0U;

    for (i = 0U; i < VIRTIO_RX_BUFFERS && i < g_rx.size; i++) {
        g_rx.desc[i].addr  = (u64)g_rx_buf_phys + (u64)i * VIRTIO_PACKET_BUF;
        g_rx.desc[i].len   = VIRTIO_PACKET_BUF;
        g_rx.desc[i].flags = VIRTQ_DESC_F_WRITE;
        g_rx.desc[i].next  = 0U;
        g_rx.avail->ring[g_rx.avail_idx % g_rx.size] = (u16)i;
        g_rx.avail_idx++;
    }

    __asm__ volatile("" ::: "memory");
    g_rx.avail->idx = g_rx.avail_idx;
    __asm__ volatile("" ::: "memory");

    virtqueue_notify(&g_rx);

    return (int)g_rx.avail_idx;
}

static void put_be16(u8 *p, u16 value)
{
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static u16 get_be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static void virtio_print_ip(const u8 *ip)
{
    console_printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void virtio_print_mac(const u8 *mac)
{
    u32 i;

    console_printf("%02X", mac[0]);
    for (i = 1U; i < 6U; i++) {
        console_printf(":%02X", mac[i]);
    }
}

static int virtio_send_arp_request(void)
{
    u32 frame_len = VIRTIO_NET_HDR_LEN + ETH_HDR_LEN + ARP_PACKET_LEN;
    u8 *buf;
    u8 *eth;
    u8 *arp;
    u32 i;

    g_tx_buf_phys = page_alloc();
    if (g_tx_buf_phys == 0U) return -1;

    buf = (u8 *)((u64)g_tx_buf_phys + KERNEL_OFFSET);
    for (i = 0U; i < VIRTIO_PAGE_SIZE; i++) buf[i] = 0U;

    eth = buf + VIRTIO_NET_HDR_LEN;
    for (i = 0U; i < 6U; i++) {
        eth[i]      = 0xFFU;
        eth[6U + i] = g_mac[i];
    }
    put_be16(eth + 12U, ETHERTYPE_ARP);

    arp = eth + ETH_HDR_LEN;
    put_be16(arp + 0U, ARP_HTYPE_ETHERNET);
    put_be16(arp + 2U, ARP_PTYPE_IPV4);
    arp[4] = 6U;
    arp[5] = 4U;
    put_be16(arp + 6U, ARP_OP_REQUEST);
    for (i = 0U; i < 6U; i++) arp[8U + i] = g_mac[i];
    for (i = 0U; i < 4U; i++) arp[14U + i] = g_guest_ip[i];
    for (i = 0U; i < 4U; i++) arp[24U + i] = g_gateway_ip[i];

    g_tx.desc[0].addr  = g_tx_buf_phys;
    g_tx.desc[0].len   = frame_len;
    g_tx.desc[0].flags = 0U;
    g_tx.desc[0].next  = 0U;
    g_tx.avail->ring[g_tx.avail_idx % g_tx.size] = 0U;
    g_tx.avail_idx++;

    __asm__ volatile("" ::: "memory");
    g_tx.avail->idx = g_tx.avail_idx;
    __asm__ volatile("" ::: "memory");

    virtqueue_notify(&g_tx);

    console_printf("virtio-net: tx arp who-has ");
    virtio_print_ip(g_gateway_ip);
    console_printf(" tell ");
    virtio_print_ip(g_guest_ip);
    console_printf(" (%u bytes)\n", frame_len);

    return 0;
}

static void virtio_handle_rx_frame(const u8 *frame, u32 len)
{
    const u8 *eth;
    const u8 *arp;
    u16 ethertype;

    if (len < VIRTIO_NET_HDR_LEN + ETH_HDR_LEN) return;

    eth       = frame + VIRTIO_NET_HDR_LEN;
    ethertype = get_be16(eth + 12U);

    if (ethertype == ETHERTYPE_ARP && len >= VIRTIO_NET_HDR_LEN + ETH_HDR_LEN + ARP_PACKET_LEN) {
        arp = eth + ETH_HDR_LEN;

        if (get_be16(arp + 6U) == ARP_OP_REPLY) {
            console_printf("virtio-net: rx arp reply ");
            virtio_print_ip(arp + 14U);
            console_printf(" is-at ");
            virtio_print_mac(arp + 8U);
            console_printf(" (%u bytes)\n", len);
            g_arp_reply_seen = 1;
            return;
        }
    }

    console_printf("virtio-net: rx frame ethertype=0x%04X (%u bytes)\n", ethertype, len);
}

static void virtio_consume_rx(void)
{
    int reposted = 0;

    while (g_rx.used_seen != g_rx.used->idx) {
        struct virtq_used_elem *elem = &g_rx.used->ring[g_rx.used_seen % g_rx.size];
        u32 id  = elem->id;
        u32 len = elem->len;

        __asm__ volatile("" ::: "memory");

        virtio_handle_rx_frame((const u8 *)((u64)g_rx_buf_phys + KERNEL_OFFSET + (u64)id * VIRTIO_PACKET_BUF), len);

        g_rx.used_seen++;
        g_rx.avail->ring[g_rx.avail_idx % g_rx.size] = (u16)id;
        g_rx.avail_idx++;
        reposted = 1;
    }

    if (reposted) {
        __asm__ volatile("" ::: "memory");
        g_rx.avail->idx = g_rx.avail_idx;
        __asm__ volatile("" ::: "memory");

        virtqueue_notify(&g_rx);
    }
}

static int virtio_wait_arp_reply(void)
{
    u64 flags;
    u32 spins;

    __asm__ volatile ("pushfq; pop %0" : "=r" (flags) : : "memory");

    for (spins = 0U; spins < VIRTIO_ARP_WAIT_SPINS; spins++) {
        __asm__ volatile ("sti; nop; nop; nop; nop; cli" : : : "memory");
        virtio_consume_rx();
        if (g_arp_reply_seen && g_tx.used->idx == g_tx.avail_idx && g_rx.irq_count != 0U && g_tx.irq_count != 0U) break;
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return g_arp_reply_seen ? 0 : -1;
}

void virtio_net_init(void)
{
    u8  bus;
    u8  device;
    u8  function;
    struct pci_msix msix;
    int posted;
    u32 i;

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

    console_set_color(0x0BU);

    if (pci_msix_open(bus, device, function, &msix) != 0) {
        console_set_color(0x0CU);
        console_printf("virtio-net: no MSI-X capability\n");
        return;
    }

    interrupts_register_handler(VIRTIO_RX_MSIX_VECTOR, virtio_rx_irq);
    interrupts_register_handler(VIRTIO_TX_MSIX_VECTOR, virtio_tx_irq);
    pci_msix_set_entry(&msix, VIRTIO_RX_MSIX_ENTRY, VIRTIO_RX_MSIX_VECTOR, apic_id());
    pci_msix_set_entry(&msix, VIRTIO_TX_MSIX_ENTRY, VIRTIO_TX_MSIX_VECTOR, apic_id());
    pci_msix_start(&msix);

    console_printf("virtio-net: msix entry %u -> vector 0x%02X apic id=%u\n",
                   VIRTIO_RX_MSIX_ENTRY, VIRTIO_RX_MSIX_VECTOR, apic_id());
    console_printf("virtio-net: msix entry %u -> vector 0x%02X apic id=%u\n",
                   VIRTIO_TX_MSIX_ENTRY, VIRTIO_TX_MSIX_VECTOR, apic_id());

    g_common->device_status = 0U;
    if (virtio_wait_status(0U) != 0) {
        console_set_color(0x0CU);
        console_printf("virtio-net: reset did not complete\n");
        return;
    }

    virtio_add_status(VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_add_status(VIRTIO_STATUS_DRIVER);

    if (virtio_negotiate_features() != 0) {
        virtio_add_status(VIRTIO_STATUS_FAILED);
        console_set_color(0x0CU);
        console_printf("virtio-net: feature negotiation failed status=0x%02X\n", g_common->device_status);
        return;
    }

    console_printf("virtio-net: features negotiated VERSION_1 + MAC status=0x%02X\n", g_common->device_status);

    g_common->msix_config = VIRTIO_NO_VECTOR;

    if (virtqueue_setup(&g_rx, VIRTIO_RECEIVEQ, VIRTIO_RX_MSIX_ENTRY) != 0 ||
        virtqueue_setup(&g_tx, VIRTIO_TRANSMITQ, VIRTIO_TX_MSIX_ENTRY) != 0) {
        virtio_add_status(VIRTIO_STATUS_FAILED);
        console_set_color(0x0CU);
        console_printf("virtio-net: virtqueue setup failed\n");
        return;
    }

    posted = virtio_post_rx_buffers();
    if (posted < 0) {
        virtio_add_status(VIRTIO_STATUS_FAILED);
        console_set_color(0x0CU);
        console_printf("virtio-net: rx buffer allocation failed\n");
        return;
    }

    console_printf("virtio-net: receiveq posted %d packet buffers of %u bytes at 0x%X\n",
                   posted, VIRTIO_PACKET_BUF, g_rx_buf_phys);

    for (i = 0U; i < 6U; i++) g_mac[i] = g_device_cfg[i];

    console_printf("virtio-net: mac ");
    virtio_print_mac(g_mac);
    console_printf("\n");

    virtio_add_status(VIRTIO_STATUS_DRIVER_OK);

    console_set_color(0x0AU);
    console_printf("virtio-net: driver ready status=0x%02X rx used idx=%u tx used idx=%u\n",
                   g_common->device_status, g_rx.used->idx, g_tx.used->idx);

    if (virtio_send_arp_request() != 0) {
        console_set_color(0x0CU);
        console_printf("virtio-net: tx buffer allocation failed\n");
        return;
    }

    if (virtio_wait_arp_reply() != 0) {
        console_set_color(0x0CU);
        console_printf("virtio-net: no arp reply\n");
    }

    console_printf("virtio-net: tx used idx=%u rx used idx=%u\n", g_tx.used->idx, g_rx.used->idx);
    console_printf("virtio-net: msix interrupts rx=%u tx=%u\n", g_rx.irq_count, g_tx.irq_count);
}
