BITS 32

SECTION .text

global clone_trampoline

clone_trampoline:
    mov ebx, [esp+4]
    mov eax, 8
    int 0x80
    test eax, eax
    jnz .parent
    pop eax
    pop ecx
    push ecx
    call eax
    mov eax, 9
    int 0x80
.parent:
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
