BITS 32

SECTION .text

global switch_context

switch_context:
    pushfd
    pushad

    mov ecx, [esp + 40]
    mov edx, [esp + 44]

    mov [ecx], esp
    mov esp, edx

    popad
    popfd
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
