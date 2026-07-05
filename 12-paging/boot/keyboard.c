#include "keyboard.h"
#include "interrupts.h"

static const char sc_lower[128] = {
    0,    0,    '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=',  0,    0,
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  0,    ' ',  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    '7',  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0
};

static const char sc_upper[128] = {
    0,    0,    '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',  '_',  '+',  0,    0,
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  0,    ' ',  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    '7',  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0
};

static u32 shift_held;

static u8 inb(u16 port)
{
    u8 value;

    __asm__ volatile ("inb %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

void keyboard_init(void)
{
    shift_held = 0U;
    interrupts_unmask_irq(1U);
    console_set_color(0x0EU);
    console_printf("keyboard ready: IRQ1 unmasked, type to echo\n");
}

void keyboard_irq(void)
{
    u8 sc;
    char ch;

    sc = inb(0x60U);

    if (sc == 0x2AU || sc == 0x36U) {
        shift_held = 1U;
        return;
    }

    if (sc == 0xAAU || sc == 0xB6U) {
        shift_held = 0U;
        return;
    }

    if (sc & 0x80U) {
        return;
    }

    ch = shift_held ? sc_upper[sc] : sc_lower[sc];

    if (ch == 0) {
        return;
    }

    console_set_color(0x0FU);
    console_printf("%c", ch);
}
