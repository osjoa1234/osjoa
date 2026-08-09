BITS 64

SECTION .text

global gdt_flush
global tss_flush
global enter_user_mode
global enter_user_mode_fork

gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    pop  rax
    push qword 0x08
    push rax
    retfq

tss_flush:
    mov ax, 0x28
    ltr ax
    ret

enter_user_mode:
    ret

enter_user_mode_fork:
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
