BITS 32

KERNEL_OFFSET equ 0xFFFFFFFF80000000

SECTION .boot

align 4
dd 0x1BADB002
dd 0x00000003
dd -(0x1BADB002 + 0x00000003)

global start

start:
    cli
    cld
    mov esp, stack_top - KERNEL_OFFSET

    mov [saved_magic - KERNEL_OFFSET], eax
    mov [saved_mbi   - KERNEL_OFFSET], ebx

    mov edi, boot_pml4 - KERNEL_OFFSET
    xor eax, eax
    mov ecx, (4096 * 4) / 4
    rep stosd

    mov eax, boot_pdpt_id - KERNEL_OFFSET
    or  eax, 3
    mov [boot_pml4 - KERNEL_OFFSET + 0*8    ], eax
    mov [boot_pml4 - KERNEL_OFFSET + 0*8 + 4], dword 0

    mov eax, boot_pdpt_hi - KERNEL_OFFSET
    or  eax, 3
    mov [boot_pml4 - KERNEL_OFFSET + 511*8    ], eax
    mov [boot_pml4 - KERNEL_OFFSET + 511*8 + 4], dword 0

    mov eax, boot_pd - KERNEL_OFFSET
    or  eax, 3
    mov [boot_pdpt_id - KERNEL_OFFSET + 0*8    ], eax
    mov [boot_pdpt_id - KERNEL_OFFSET + 0*8 + 4], dword 0

    mov [boot_pdpt_hi - KERNEL_OFFSET + 510*8    ], eax
    mov [boot_pdpt_hi - KERNEL_OFFSET + 510*8 + 4], dword 0

    mov edi, boot_pd - KERNEL_OFFSET
    mov eax, 0x83
    mov ecx, 512
.fill:
    mov [edi    ], eax
    mov [edi + 4], dword 0
    add eax, 0x200000
    add edi, 8
    loop .fill

    mov eax, boot_pml4 - KERNEL_OFFSET
    mov cr3, eax

    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    mov eax, cr0
    or  eax, (1 << 31)
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp  0x08:long_mode_stub

BITS 64
long_mode_stub:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, stack_top
    mov rax, long_mode_entry
    jmp rax

align 8
gdt64:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

SECTION .text
BITS 64

global long_mode_entry
extern kernel_main

long_mode_entry:
    mov edi, dword [rel saved_magic]
    mov esi, dword [rel saved_mbi]
    call kernel_main
.halt:
    hlt
    jmp .halt

SECTION .bss
alignb 16
stack_bottom:
    resb 16384
global stack_top
stack_top:

global saved_magic
global saved_mbi
saved_magic: resd 1
saved_mbi:   resd 1

alignb 4096
global boot_pml4
boot_pml4:    resb 4096
boot_pdpt_id: resb 4096
boot_pdpt_hi: resb 4096
boot_pd:      resb 4096

SECTION .note.GNU-stack noalloc noexec nowrite progbits
