typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static volatile u16 *const vga = (volatile u16 *)0xB8000;
static const u32 multiboot_magic = 0x2BADB002U;

static void vga_clear(void)
{
    u32 index;

    for (index = 0; index < 80U * 25U; ++index) {
        vga[index] = 0x0F20;
    }
}

static void vga_write_at(const char *text, u32 cell, u8 color)
{
    u32 index;

    index = 0;
    while (text[index] != '\0') {
        vga[cell + index] = (u16)(((u16)color << 8) | (u8)text[index]);
        ++index;
    }
}

static void vga_write(const char *text)
{
    vga_write_at(text, 0, 0x0F);
}

static void vga_write_hex(u32 value, u32 cell, u8 color)
{
    static const char digits[] = "0123456789ABCDEF";
    u32 shift;

    for (shift = 0; shift < 8U; ++shift) {
        u32 nibble = (value >> ((7U - shift) * 4U)) & 0xFU;
        vga[cell + shift] = (u16)(((u16)color << 8) | (u8)digits[nibble]);
    }
}

void kernel_main(u32 magic, u32 multiboot_info)
{
    vga_clear();

    if (magic != multiboot_magic) {
        vga_write("Hello world -- protected mode (32-bit), GRUB2 handoff failed");
        vga_write_at("magic=0x", 80U, 0x0C);
        vga_write_hex(magic, 88U, 0x0C);

        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    vga_write("Hello world -- protected mode (32-bit), C kernel loaded by GRUB2 via Multiboot");
    vga_write_at("multiboot info at 0x", 80U, 0x0A);
    vga_write_hex(multiboot_info, 100U, 0x0A);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}