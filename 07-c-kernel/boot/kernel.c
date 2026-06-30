typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static volatile u16 *const vga = (volatile u16 *)0xB8000;

static void vga_clear(void)
{
    u32 index;

    for (index = 0; index < 80U * 25U; ++index) {
        vga[index] = 0x0F20;
    }
}

static void vga_write(const char *text)
{
    u32 index;

    index = 0;
    while (text[index] != '\0') {
        vga[index] = (u16)0x0F00 | (u8)text[index];
        ++index;
    }
}

void kernel_main(void)
{
    vga_clear();
    vga_write("Hello world -- protected mode (32-bit), C kernel loaded from FAT16 via BIOS LBA");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}