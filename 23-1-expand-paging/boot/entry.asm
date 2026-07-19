BITS 32

KERNEL_OFFSET equ 0xC0000000

SECTION .boot

align 4
dd 0x1BADB002
dd 0x00000003
dd -(0x1BADB002 + 0x00000003)

global start
extern kernel_main

start:
    cli
    cld

    mov esp, stack_top
    sub esp, KERNEL_OFFSET
    push eax
    push ebx

    mov edi, boot_pt
    sub edi, KERNEL_OFFSET
    mov eax, 0x00000007
    mov ecx, 1024
.fill_pt:
    mov [edi], eax
    add eax, 0x1000
    add edi, 4
    loop .fill_pt

    mov eax, boot_pt
    sub eax, KERNEL_OFFSET
    or  eax, 0x07
    mov edi, boot_pd
    sub edi, KERNEL_OFFSET
    mov [edi + 0*4],   eax
    mov [edi + 768*4], eax

    mov cr3, edi

    mov ecx, cr0
    or  ecx, 0x80000000
    mov cr0, ecx

    lea ecx, [higher_half]
    jmp ecx

SECTION .bss
align 16
stack_bottom:
    resb 16384
global stack_top
stack_top:

align 4096
boot_pd: resd 1024
boot_pt: resd 1024

SECTION .text

higher_half:
    add esp, KERNEL_OFFSET

    pop ebx
    pop eax

    mov esp, stack_top

    push ebx
    push eax
    call kernel_main
    add esp, 8

.halt:
    hlt
    jmp .halt

SECTION .note.GNU-stack noalloc noexec nowrite progbits
