#include "interrupts.h"

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8 zero;
    u8 type_attr;
    u16 offset_high;
} __attribute__((packed));

struct idt_pointer {
    u16 limit;
    u32 base;
} __attribute__((packed));

struct interrupt_frame {
    u32 edi;
    u32 esi;
    u32 ebp;
    u32 esp;
    u32 ebx;
    u32 edx;
    u32 ecx;
    u32 eax;
    u32 vector;
    u32 error_code;
    u32 eip;
    u32 cs;
    u32 eflags;
};

enum {
    PIC1_COMMAND = 0x20,
    PIC1_DATA = 0x21,
    PIC2_COMMAND = 0xA0,
    PIC2_DATA = 0xA1,
    PIC_EOI = 0x20,
    PIC_ICW1_INIT = 0x10,
    PIC_ICW1_ICW4 = 0x01,
    PIC_ICW4_8086 = 0x01
};

static struct idt_entry idt[256];
static struct idt_pointer idt_descriptor;
static u8 pic1_mask = 0xFFU;
static u8 pic2_mask = 0xFFU;

extern void *interrupt_stub_table[];
extern void timer_irq(void);
extern void keyboard_irq(void);

static const char *const exception_names[32] = {
    "Divide error",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "BOUND range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 floating-point exception",
    "Alignment check",
    "Machine check",
    "SIMD floating-point exception",
    "Virtualization exception",
    "Control protection exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor injection exception",
    "VMM communication exception",
    "Security exception",
    "Reserved"
};

static void outb(u16 port, u8 value)
{
    __asm__ volatile ("outb %0, %1" : : "a" (value), "Nd" (port));
}

static void io_wait(void)
{
    outb(0x80U, 0U);
}

static u16 read_cs(void)
{
    u16 selector;

    __asm__ volatile ("mov %%cs, %0" : "=r" (selector));
    return selector;
}

static void idt_set_entry(u8 vector, u32 handler, u16 selector, u8 type_attr)
{
    idt[vector].offset_low = (u16)(handler & 0xFFFFU);
    idt[vector].selector = selector;
    idt[vector].zero = 0U;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_high = (u16)((handler >> 16) & 0xFFFFU);
}

static void idt_load(void)
{
    __asm__ volatile ("lidt %0" : : "m" (idt_descriptor));
}

static void pic_write_masks(void)
{
    outb(PIC1_DATA, pic1_mask);
    outb(PIC2_DATA, pic2_mask);
}

static void pic_unmask_irq(u8 irq)
{
    if (irq < 8U) {
        pic1_mask = (u8)(pic1_mask & (u8)~(1U << irq));
    } else {
        pic1_mask = (u8)(pic1_mask & (u8)~(1U << 2U));
        pic2_mask = (u8)(pic2_mask & (u8)~(1U << (irq - 8U)));
    }

    pic_write_masks();
}

static void pic_remap(u8 master_offset, u8 slave_offset)
{
    outb(PIC1_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, master_offset);
    io_wait();
    outb(PIC2_DATA, slave_offset);
    io_wait();

    outb(PIC1_DATA, 0x04U);
    io_wait();
    outb(PIC2_DATA, 0x02U);
    io_wait();

    outb(PIC1_DATA, PIC_ICW4_8086);
    io_wait();
    outb(PIC2_DATA, PIC_ICW4_8086);
    io_wait();

    pic1_mask = 0xFFU;
    pic2_mask = 0xFFU;
    pic_write_masks();
}

static void pic_send_eoi(u8 irq)
{
    if (irq >= 8U) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}

static u32 read_cr2(void)
{
    u32 cr2;

    __asm__ volatile ("mov %%cr2, %0" : "=r" (cr2));
    return cr2;
}

static void qemu_debug_exit(u8 value)
{
    outb(0xF4U, value);
}

static void halt_after_exception(void)
{
    interrupts_disable();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void handle_exception(const struct interrupt_frame *frame)
{
    console_set_color(frame->vector == 3U ? 0x0EU : 0x0CU);
    console_printf("interrupt 0x%02X %s err=0x%08X eip=0x%08X\n",
                   frame->vector,
                   exception_names[frame->vector],
                   frame->error_code,
                   frame->eip);

    if (frame->vector == 14U) {
        console_printf("page fault at cr2=0x%08X\n", read_cr2());
    }

    if (frame->vector != 3U && frame->vector != 4U) {
        console_printf("system halted after exception\n");
        qemu_debug_exit(0x01U);
        halt_after_exception();
    }
}

static void handle_irq(const struct interrupt_frame *frame)
{
    const u32 irq = frame->vector - 0x20U;

    pic_send_eoi((u8)irq);

    if (irq == 0U) {
        timer_irq();
    }

    if (irq == 1U) {
        keyboard_irq();
    }
}

void interrupt_dispatch(const struct interrupt_frame *frame)
{
    if (frame->vector < 32U) {
        handle_exception(frame);
        return;
    }

    if (frame->vector < 48U) {
        handle_irq(frame);
        return;
    }

    console_set_color(0x0DU);
    console_printf("interrupt 0x%02X\n", frame->vector);
}

void interrupts_init(void)
{
    u32 index;
    const u16 selector = read_cs();

    for (index = 0U; index < 256U; ++index) {
        idt_set_entry((u8)index, (u32)interrupt_stub_table[index], selector, 0x8EU);
    }

    idt_descriptor.limit = (u16)(sizeof(idt) - 1U);
    idt_descriptor.base = (u32)&idt[0];

    pic_remap(0x20U, 0x28U);
    idt_load();
}

void interrupts_enable(void)
{
    __asm__ volatile ("sti");
}

void interrupts_disable(void)
{
    __asm__ volatile ("cli");
}

void interrupts_unmask_irq(u8 irq)
{
    pic_unmask_irq(irq);
}
