BITS 64

SECTION .text

global clone_trampoline

clone_trampoline:
    mov eax, 56
    syscall
    test eax, eax
    jnz .parent
    pop rax
    pop rdi
    push rdi
    call rax
    mov eax, 60
    syscall
.parent:
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
