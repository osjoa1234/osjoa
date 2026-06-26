BITS 16
ORG 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    mov si, msg
    call print

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

    mov byte [retries], 3
.disk_retry:
    mov ah, 0x02
    mov al, 1
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    mov bx, stage2
    int 0x13
    jnc disk_ok
    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    dec byte [retries]
    jnz .disk_retry
    mov si, msg_disk_fail
    call print
    jmp $

disk_ok:
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_start

a20_fail:
    mov si, msg_a20_fail
    call print
    jmp $

print:
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0x00
    int 0x10
    jmp .loop
.done:
    ret

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

boot_drive    db 0
retries       db 0
msg           db "Hello world -- real mode (16-bit)", 0x0D, 0x0A, 0
msg_a20_fail  db "A20 enable failed -- still wrapping at 1MB", 0
msg_disk_fail db "Disk read failed -- stage2 not loaded", 0

times 510-($-$$) db 0
dw 0xAA55

BITS 32
stage2:
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

msg_pm db "Hello world -- protected mode (32-bit), loaded from disk", 0

times 1024-($-$$) db 0
