BITS 16
ORG 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov si, msg

print:
    lodsb
    test al, al
    jz hang
    mov ah, 0x0E
    mov bh, 0x00
    int 0x10
    jmp print

hang:
    hlt
    jmp hang

msg db "Hello world", 0x0D, 0x0A, 0

times 510-($-$$) db 0
dw 0xAA55
