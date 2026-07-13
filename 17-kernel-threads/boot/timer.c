#include "timer.h"
#include "interrupts.h"

#define PIT_CHANNEL0  0x40U
#define PIT_COMMAND   0x43U
#define PIT_BASE_HZ   1193182U

static volatile u32 tick_count;
static u32          tick_hz;

static void outb(u16 port, u8 val)
{
    __asm__ volatile ("outb %0, %1" : : "a" (val), "Nd" (port));
}

void timer_init(u32 hz)
{
    u32 divisor = PIT_BASE_HZ / hz;

    tick_hz = hz;
    outb(PIT_COMMAND, 0x36U);
    outb(PIT_CHANNEL0, (u8)(divisor & 0xFFU));
    outb(PIT_CHANNEL0, (u8)((divisor >> 8U) & 0xFFU));
    interrupts_unmask_irq(0U);
}

void timer_irq(void)
{
    tick_count++;
}

u32 timer_ticks(void)
{
    return tick_count;
}

void timer_sleep(u32 ms)
{
    u32 target = tick_count + (ms * tick_hz / 1000U);

    while (tick_count < target) {
        __asm__ volatile ("hlt");
    }
}
