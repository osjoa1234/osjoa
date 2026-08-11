BITS 64

SECTION .text

global clone_trampoline

clone_trampoline:
    mov rbx, rdi
    mov eax, 120
    int 0x80
    test eax, eax
    jnz .parent
    pop rax
    pop rdi
    push rdi
    call rax
    mov eax, 201
    int 0x80
.parent:
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
