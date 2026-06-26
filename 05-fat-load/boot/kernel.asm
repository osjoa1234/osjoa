BITS 16
ORG 0x7E00

start:
    in al, 0x92
    or al, 2
    out 0x92, al

    mov ax, 0xFFFF
    mov es, ax
    mov word [0x0500], 0x1234
    mov word [es:0x0510], 0x4321
    cmp word [0x0500], 0x4321
    je a20_fail

    xor ax, ax
    mov es, ax

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_start

a20_fail:
    mov si, msg_a20_fail
.loop:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    mov bh, 0x00
    int 0x10
    jmp .loop
.hang:
    jmp $

BITS 32
protected_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x90000

    mov edi, 0xB8000
    mov ecx, 80*25
    mov ax, 0x0F20
    rep stosw

    mov esi, msg_pm
    mov edi, 0xB8000
    mov ah, 0x0F
.loop:
    lodsb
    test al, al
    jz .hang
    mov [edi], ax
    add edi, 2
    jmp .loop
.hang:
    hlt
    jmp .hang

gdt_start:
    dd 0x00000000, 0x00000000
gdt_code:
    dd 0x0000FFFF, 0x00CF9A00
gdt_data:
    dd 0x0000FFFF, 0x00CF9200
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

msg_a20_fail db "A20 enable failed -- still wrapping at 1MB", 0
msg_pm       db "Hello world -- protected mode (32-bit), loaded from FAT16", 0
