ENTRY(start)

KERNEL_OFFSET = 0xC0000000;

PHDRS
{
    boot PT_LOAD FLAGS(5);
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00100000;

    .boot ALIGN(4096) :
    {
        KEEP(*(.boot))
    } :boot

    . = ALIGN(0x1000) + KERNEL_OFFSET;

    kernel_start = .;

    .text ALIGN(16) : AT(ADDR(.text) - KERNEL_OFFSET)
    {
        *(.text*)
    } :text

    .rodata ALIGN(16) : AT(ADDR(.rodata) - KERNEL_OFFSET)
    {
        *(.rodata*)
    } :text

    .data ALIGN(16) : AT(ADDR(.data) - KERNEL_OFFSET)
    {
        *(.data*)
    } :data

    .bss ALIGN(16) : AT(ADDR(.bss) - KERNEL_OFFSET)
    {
        *(COMMON)
        *(.bss*)
    } :data

    kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
