#include <stdarg.h>

#include "console.h"

struct console_state {
    u32 row;
    u32 column;
    u8 color;
};

static volatile u16 *const vga = (volatile u16 *)(0xB8000U + KERNEL_OFFSET);
static struct console_state console = { 0U, 0U, 0x0FU };

static u16 vga_entry(u8 character, u8 color)
{
    return (u16)(((u16)color << 8) | character);
}

static void console_clear_row(u32 row)
{
    u32 column;

    for (column = 0; column < VGA_WIDTH; ++column) {
        vga[row * VGA_WIDTH + column] = vga_entry(' ', console.color);
    }
}

static void console_scroll(void)
{
    u32 row;
    u32 column;

    for (row = 1U; row < VGA_HEIGHT; ++row) {
        for (column = 0; column < VGA_WIDTH; ++column) {
            vga[(row - 1U) * VGA_WIDTH + column] = vga[row * VGA_WIDTH + column];
        }
    }

    console_clear_row(VGA_HEIGHT - 1U);
    console.row = VGA_HEIGHT - 1U;
}

static void console_newline(void)
{
    console.column = 0U;
    ++console.row;

    if (console.row >= VGA_HEIGHT) {
        console_scroll();
    }
}

void console_clear(void)
{
    u32 row;

    for (row = 0; row < VGA_HEIGHT; ++row) {
        console_clear_row(row);
    }

    console.row = 0U;
    console.column = 0U;
}

void console_set_color(u8 color)
{
    console.color = color;
}

static void console_debug_put_char(char value)
{
    __asm__ volatile ("outb %0, %1" : : "a" ((u8)value), "Nd" ((u16)0xE9));
}

static void console_put_char(char value)
{
    console_debug_put_char(value);

    if (value == '\n') {
        console_newline();
        return;
    }

    if (value == '\r') {
        console.column = 0U;
        return;
    }

    if (value == '\b') {
        if (console.column > 0U) {
            --console.column;
        }
        return;
    }

    vga[console.row * VGA_WIDTH + console.column] = vga_entry((u8)value, console.color);
    ++console.column;

    if (console.column >= VGA_WIDTH) {
        console_newline();
    }
}

static void console_write(const char *text)
{
    u32 index;

    if (text == (const char *)0) {
        text = "(null)";
    }

    index = 0U;
    while (text[index] != '\0') {
        console_put_char(text[index]);
        ++index;
    }
}

static void console_write_unsigned_padded(u32 value, u32 base, const char *digits, u32 width, char pad)
{
    char buffer[32];
    u32 length;
    u32 max_length;

    max_length = (u32)sizeof(buffer);
    if (width > max_length) {
        width = max_length;
    }

    length = 0U;
    do {
        buffer[length] = digits[value % base];
        value /= base;
        ++length;
    } while (value != 0U && length < max_length);

    while (length < width) {
        buffer[length] = pad;
        ++length;
    }

    while (length > 0U) {
        --length;
        console_put_char(buffer[length]);
    }
}

static void console_write_signed_padded(int value, u32 width, char pad)
{
    u32 magnitude;

    if (value < 0) {
        console_put_char('-');
        magnitude = 0U - (u32)value;
        if (width > 0U) {
            --width;
        }
    } else {
        magnitude = (u32)value;
    }

    console_write_unsigned_padded(magnitude, 10U, "0123456789", width, pad);
}

static void console_vprintf(const char *format, va_list arguments)
{
    u32 index;
    u32 width;
    char pad;

    index = 0U;
    while (format[index] != '\0') {
        if (format[index] != '%') {
            console_put_char(format[index]);
            ++index;
            continue;
        }

        ++index;
        if (format[index] == '\0') {
            console_put_char('%');
            break;
        }

        pad = ' ';
        width = 0U;
        if (format[index] == '0') {
            pad = '0';
        }

        while (format[index] >= '0' && format[index] <= '9') {
            width = width * 10U + (u32)(format[index] - '0');
            ++index;
        }

        if (format[index] == '\0') {
            console_put_char('%');
            break;
        }

        switch (format[index]) {
        case 'c':
            console_put_char((char)va_arg(arguments, int));
            break;
        case 'd':
            console_write_signed_padded(va_arg(arguments, int), width, pad);
            break;
        case 's':
            console_write(va_arg(arguments, const char *));
            break;
        case 'u':
            console_write_unsigned_padded(va_arg(arguments, u32), 10U, "0123456789", width, pad);
            break;
        case 'x':
            console_write_unsigned_padded(va_arg(arguments, u32), 16U, "0123456789abcdef", width, pad);
            break;
        case 'X':
            console_write_unsigned_padded(va_arg(arguments, u32), 16U, "0123456789ABCDEF", width, pad);
            break;
        case '%':
            console_put_char('%');
            break;
        default:
            console_put_char('%');
            console_put_char(format[index]);
            break;
        }

        ++index;
    }
}

void console_printf(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    console_vprintf(format, arguments);
    va_end(arguments);
}