BITS 32

SECTION .multiboot
align 4

multiboot_magic    equ 0x1BADB002
multiboot_flags    equ 0x00000003
multiboot_checksum equ -(multiboot_magic + multiboot_flags)

    dd multiboot_magic
    dd multiboot_flags
    dd multiboot_checksum

SECTION .bss
align 16
stack_bottom:
    resb 16384
global stack_top
stack_top:

SECTION .text

global start
extern kernel_main

start:
    cli
    cld
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
    add esp, 8

.halt:
    hlt
    jmp .halt

SECTION .note.GNU-stack noalloc noexec nowrite progbits