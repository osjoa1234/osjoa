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
    mov gs, ax
    xor eax, eax
    mov fs, ax

    push qword 0x23
    push rsi
    push qword 0x202
    push qword 0x1B
    push rdi

    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq

enter_user_mode_fork:
    mov rax, 0x23
    mov ds, ax
    mov es, ax
    mov gs, ax

    push qword 0x23
    push qword [rdi + 120]
    push qword [rdi + 128]
    push qword 0x1B
    push qword [rdi + 112]

    mov rsi, [rdi + 8]
    mov rbp, [rdi + 16]
    mov rbx, [rdi + 24]
    mov rdx, [rdi + 32]
    mov rcx, [rdi + 40]
    mov r8,  [rdi + 48]
    mov r9,  [rdi + 56]
    mov r10, [rdi + 64]
    mov r11, [rdi + 72]
    mov r12, [rdi + 80]
    mov r13, [rdi + 88]
    mov r14, [rdi + 96]
    mov r15, [rdi + 104]
    xor rax, rax
    mov rdi, [rdi + 0]

    iretq

SECTION .note.GNU-stack noalloc noexec nowrite progbits
