ENTRY(start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00100000;

    .multiboot ALIGN(4) :
    {
        KEEP(*(.multiboot))
    } :text

    .text ALIGN(16) :
    {
        *(.text*)
    } :text

    .rodata ALIGN(16) :
    {
        *(.rodata*)
    } :text

    .data ALIGN(16) :
    {
        *(.data*)
    } :data

    .bss ALIGN(16) :
    {
        *(COMMON)
        *(.bss*)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}