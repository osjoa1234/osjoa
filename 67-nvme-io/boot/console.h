#ifndef CONSOLE_H
#define CONSOLE_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define KERNEL_OFFSET 0xFFFFFFFF80000000ULL

enum {
    VGA_WIDTH = 80,
    VGA_HEIGHT = 25
};

void console_clear(void);
void console_set_color(u8 color);
void console_putchar(u8 c);
void console_printf(const char *format, ...);

#endif