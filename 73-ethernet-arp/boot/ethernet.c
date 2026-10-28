#include "ethernet.h"
#include "arp.h"
#include "virtio_net.h"

const u8 ethernet_broadcast_mac[ETH_ALEN] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU };

int ethernet_mac_equal(const u8 *a, const u8 *b)
{
    u32 i;

    for (i = 0U; i < ETH_ALEN; i++) {
        if (a[i] != b[i]) return 0;
    }

    return 1;
}

void ethernet_print_mac(const u8 *mac)
{
    u32 i;

    console_printf("%02X", mac[0]);
    for (i = 1U; i < ETH_ALEN; i++) {
        console_printf(":%02X", mac[i]);
    }
}

void ethernet_rx(const u8 *frame, u32 len)
{
    const struct eth_hdr *eth = (const struct eth_hdr *)frame;
    u16 ethertype;

    if (len < ETH_HDR_LEN) return;

    if (!ethernet_mac_equal(eth->dst, virtio_net_mac()) && !ethernet_mac_equal(eth->dst, ethernet_broadcast_mac)) return;

    ethertype = ntohs(eth->ethertype);

    if (ethertype == ETHERTYPE_ARP) {
        arp_rx(frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
        return;
    }

    console_printf("ethernet: rx frame ethertype=0x%04X (%u bytes)\n", ethertype, len);
}

int ethernet_send(const u8 *dst_mac, u16 ethertype, const u8 *payload, u32 len)
{
    struct eth_hdr *eth;
    u8 *frame;
    u32 i;

    if (len > ETH_MTU) return -1;

    frame = virtio_net_tx_alloc();
    if (frame == 0) return -1;

    eth = (struct eth_hdr *)frame;
    for (i = 0U; i < ETH_ALEN; i++) {
        eth->dst[i] = dst_mac[i];
        eth->src[i] = virtio_net_mac()[i];
    }
    eth->ethertype = htons(ethertype);

    for (i = 0U; i < len; i++) frame[ETH_HDR_LEN + i] = payload[i];

    return virtio_net_tx_submit(frame, ETH_HDR_LEN + len);
}
