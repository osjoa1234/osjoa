BITS 32

SECTION .text

global gdt_flush
global tss_flush
global enter_user_mode
global enter_user_mode_fork_child

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push dword 0x08
    push dword .reload_cs
    retf
.reload_cs:
    ret

tss_flush:
    mov ax, 0x28
    ltr ax
    ret

enter_user_mode:
    mov eax, [esp + 4]
    mov ecx, [esp + 8]
    mov bx, 0x23
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    push 0x23
    push ecx
    push 0x202
    push 0x1B
    push eax
    iret

enter_user_mode_fork_child:
    mov eax, [esp + 4]
    mov ecx, [esp + 8]
    mov bx, 0x23
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    push 0x23
    push ecx
    push 0x202
    push 0x1B
    push eax
    xor eax, eax
    iret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
