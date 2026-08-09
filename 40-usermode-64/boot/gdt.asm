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
    mov rax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword 0x23
    push rsi
    push qword 0x202
    push qword 0x1B
    push rdi
    iretq

enter_user_mode_fork:
    mov r11, rdi

    mov rax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov r8,  [r11 + 48]
    mov r9,  [r11 + 56]
    mov r10, [r11 + 64]

    push qword 0x23
    push r9
    push r10
    push qword 0x1B
    push r8

    mov rdi, [r11 + 0]
    mov rsi, [r11 + 8]
    mov rbp, [r11 + 16]
    mov rbx, [r11 + 24]
    mov rdx, [r11 + 32]
    mov rcx, [r11 + 40]
    xor rax, rax

    iretq

SECTION .note.GNU-stack noalloc noexec nowrite progbits
