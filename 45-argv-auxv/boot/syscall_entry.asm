BITS 64

SECTION .data

global syscall_kernel_rsp
syscall_kernel_rsp: dq 0
syscall_user_rsp_scratch: dq 0

SECTION .text

extern syscall64_dispatch

global syscall_entry

syscall_entry:
    mov [rel syscall_user_rsp_scratch], rsp
    mov rsp, [rel syscall_kernel_rsp]

    push qword 0x23
    push qword [rel syscall_user_rsp_scratch]
    push r11
    push qword 0x1B
    push rcx
    push qword 0
    push qword 0x100

    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call syscall64_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

SECTION .note.GNU-stack noalloc noexec nowrite progbits
