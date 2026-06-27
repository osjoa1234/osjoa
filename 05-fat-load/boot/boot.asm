BITS 16
ORG 0x7C00

DIRBUF      equ 0x9000
FATBUF      equ 0x9200
KERNEL_DEST equ 0x7E00

jmp short start
nop

bpb_oem         db "MSWIN4.1"
bpb_bytspersec  dw 512
bpb_secperclus  db 1
bpb_rsvdseccnt  dw 1
bpb_numfats     db 2
bpb_rootentcnt  dw 512
bpb_totsec16    dw 0
bpb_media       db 0xF8
bpb_fatsz16     dw 0
bpb_secpertrk   dw 0
bpb_numheads    dw 0
bpb_hiddsec     dd 0
bpb_totsec32    dd 0
bpb_drvnum      db 0x80
bpb_reserved1   db 0
bpb_bootsig     db 0x29
bpb_volid       dd 0x12345678
bpb_vollab      db "CUSTOMOS   "
bpb_filsystype  db "FAT16   "

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl
    cld

    mov si, msg
    call print

    mov ax, [bpb_fatsz16]
    mov bl, [bpb_numfats]
    xor bh, bh
    mul bx
    add ax, [bpb_rsvdseccnt]
    mov [root_dir_sector], ax

    mov ax, [bpb_rootentcnt]
    mov bx, 32
    mul bx
    add ax, [bpb_bytspersec]
    adc dx, 0
    sub ax, 1
    sbb dx, 0
    div word [bpb_bytspersec]
    mov [root_dir_sectors], ax

    add ax, [root_dir_sector]
    mov [data_sector], ax

    mov ax, [root_dir_sector]
    mov [scan_sector], ax
    mov ax, [root_dir_sectors]
    mov [scan_cnt], ax

.next_dirsec:
    mov ax, [scan_sector]
    mov cx, 1
    mov di, DIRBUF
    call read_sectors

    mov di, DIRBUF
    mov dx, [bpb_bytspersec]
    shr dx, 5
.next_entry:
    cmp byte [di], 0
    je not_found
    mov si, kernel_name
    mov cx, 11
    push di
    repe cmpsb
    pop di
    je found
    add di, 32
    dec dx
    jnz .next_entry

    inc word [scan_sector]
    dec word [scan_cnt]
    jnz .next_dirsec

not_found:
    jmp disk_fail

found:
    mov ax, [di+26]
    mov [cur_cluster], ax
    mov word [dest_off], KERNEL_DEST

.load_cluster:
    mov ax, [cur_cluster]
    sub ax, 2
    mov bl, [bpb_secperclus]
    xor bh, bh
    mul bx
    add ax, [data_sector]

    mov bl, [bpb_secperclus]
    xor bh, bh
    mov cx, bx
    mov di, [dest_off]
    call read_sectors

    mov al, [bpb_secperclus]
    xor ah, ah
    mul word [bpb_bytspersec]
    add [dest_off], ax

    mov ax, [cur_cluster]
    call fat_next
    mov [cur_cluster], ax
    cmp ax, 0xFFF8
    jb .load_cluster

    jmp 0x0000:KERNEL_DEST

fat_next:
    xor dx, dx
    shl ax, 1
    rcl dx, 1
    div word [bpb_bytspersec]
    add ax, [bpb_rsvdseccnt]
    push dx
    mov cx, 1
    mov di, FATBUF
    call read_sectors
    pop bx
    mov ax, [FATBUF + bx]
    ret

read_sectors:
.next:
    push ax
    push cx
    push di
    call set_chs
    mov byte [retries], 3
.retry:
    mov ah, 0x02
    mov al, 1
    mov bx, di
    mov dl, [boot_drive]
    int 0x13
    jnc .read_ok
    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    dec byte [retries]
    jnz .retry
    jmp disk_fail
.read_ok:
    pop di
    pop cx
    pop ax
    inc ax
    mov bx, [bpb_bytspersec]
    add di, bx
    loop .next
    ret

set_chs:
    xor dx, dx
    div word [bpb_secpertrk]
    inc dl
    mov cl, dl
    xor dx, dx
    div word [bpb_numheads]
    mov dh, dl
    mov ch, al
    and ah, 0x03
    shl ah, 6
    or cl, ah
    ret

disk_fail:
    mov si, msg_disk_fail
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

boot_drive       db 0
retries          db 0
root_dir_sector  dw 0
root_dir_sectors dw 0
data_sector      dw 0
scan_sector      dw 0
scan_cnt         dw 0
cur_cluster      dw 0
dest_off         dw 0

kernel_name   db "KERNEL  BIN"
msg           db "Hello world -- real mode (16-bit)", 0x0D, 0x0A, 0
msg_disk_fail db "KERNEL.BIN load failed", 0

times 510-($-$$) db 0
dw 0xAA55
