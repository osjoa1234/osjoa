#include "net_rx.h"
#include "ethernet.h"
#include "thread.h"
#include "interrupts.h"

enum {
    NET_RX_SLOTS     = 16,
    NET_RX_FRAME_MAX = ETH_HDR_LEN + ETH_MTU
};

struct net_rx_slot {
    u32 len;
    u8  data[NET_RX_FRAME_MAX];
};

static struct net_rx_slot g_ring[NET_RX_SLOTS];
static volatile u32 g_head;
static volatile u32 g_tail;
static volatile u32 g_dropped;
static thread_t *volatile g_rx_thread;

int net_rx_enqueue(const u8 *frame, u32 len)
{
    struct net_rx_slot *slot;
    u32 i;

    if (len > NET_RX_FRAME_MAX || g_head - g_tail >= NET_RX_SLOTS) {
        g_dropped++;
        return -1;
    }

    slot = &g_ring[g_head % NET_RX_SLOTS];
    for (i = 0U; i < len; i++) slot->data[i] = frame[i];
    slot->len = len;

    __asm__ volatile ("" ::: "memory");
    g_head++;

    if (g_rx_thread) thread_unpark(g_rx_thread);

    return 0;
}

static void net_rx_thread(void)
{
    for (;;) {
        interrupts_disable();

        if (g_head == g_tail) {
            thread_park();
            continue;
        }

        interrupts_enable();

        ethernet_rx(g_ring[g_tail % NET_RX_SLOTS].data, g_ring[g_tail % NET_RX_SLOTS].len);

        __asm__ volatile ("" ::: "memory");
        g_tail++;
    }
}

void net_rx_init(void)
{
    g_rx_thread = thread_create(net_rx_thread);
    if (g_rx_thread == 0) {
        console_set_color(0x0CU);
        console_printf("net_rx: thread create failed\n");
        return;
    }

    console_set_color(0x0BU);
    console_printf("net_rx: ring %u slots, rx thread started\n", NET_RX_SLOTS);
}

u32 net_rx_dropped(void)
{
    return g_dropped;
}
