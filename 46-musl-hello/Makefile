ASM          := nasm
CC           := gcc
LD           := ld
MUSLCC       := musl-gcc
QEMU         := qemu-system-x86_64
GRUBFILE     := grub-file
GRUBMKRESCUE := grub-mkrescue

BUILD        := build
ISO_ROOT     := $(BUILD)/isofiles
INITRDDIR    := initrd
INITRAMFS    := $(BUILD)/initramfs.cpio
ENTRYOBJ     := $(BUILD)/entry.o
CONSOLEOBJ   := $(BUILD)/console.o
INTASMOBJ    := $(BUILD)/interrupts_asm.o
INTOBJ       := $(BUILD)/interrupts.o
KBOBJ        := $(BUILD)/keyboard.o
PAGINGOBJ    := $(BUILD)/paging.o
PHYSMEMOBJ   := $(BUILD)/phys_mem.o
KHEAPOBJ     := $(BUILD)/kheap.o
TIMEROBJ     := $(BUILD)/timer.o
CTXOBJ       := $(BUILD)/context_switch.o
GDTASMOBJ    := $(BUILD)/gdt_asm.o
GDTOBJ       := $(BUILD)/gdt.o
SYSENTRYOBJ  := $(BUILD)/syscall_entry.o
THREADOBJ    := $(BUILD)/thread.o
MONITOROBJ   := $(BUILD)/monitor.o
WQOBJ        := $(BUILD)/wait_queue.o
SYSCALLOBJ   := $(BUILD)/syscall.o
INITRDOBJ    := $(BUILD)/initrd.o
ELFOBJ       := $(BUILD)/elf.o
VFSOBJ       := $(BUILD)/vfs.o
CONDEVOBJ    := $(BUILD)/console_dev.o
PROCESSOBJ   := $(BUILD)/process.o
KERNELOBJ    := $(BUILD)/kernel.o
KERNELELF    := $(BUILD)/kernel.elf
ISO          := $(BUILD)/os.iso

USERINIT     := $(BUILD)/init
USEROBJ      := $(BUILD)/init_user.o
USERHELLO    := $(BUILD)/hello
HELLOOBJ     := $(BUILD)/hello_user.o
USERHELLO2   := $(BUILD)/hello2
HELLO2OBJ    := $(BUILD)/hello2_user.o
USERBRK      := $(BUILD)/brk
BRKOBJ       := $(BUILD)/brk_user.o
USERMMAP     := $(BUILD)/mmap
MMAPOBJ      := $(BUILD)/mmap_user.o
USERTLS      := $(BUILD)/tls
TLSOBJ       := $(BUILD)/tls_user.o
USERSYSCALL64 := $(BUILD)/syscall64
SYSCALL64OBJ  := $(BUILD)/syscall64_user.o
USERMUSLHELLO := $(BUILD)/musl_hello
CLONEOBJ     := $(BUILD)/clone_user.o

CFLAGS       := -m64 -mcmodel=kernel -std=c11 -O2 -Wall -Wextra -ffreestanding \
                -fno-builtin -fno-pic -fno-pie -fno-stack-protector -mno-red-zone \
                -mno-sse -mno-sse2 -mno-mmx
LDFLAGS      := -m elf_x86_64 -T linker.ld -nostdlib
RUNNOGUI     := -display none -serial none -monitor none -debugcon stdio \
                -device isa-debug-exit,iobase=0xf4,iosize=0x04

.PHONY: all run run-nogui clean tools

all: $(ISO)

tools:
	$(MAKE) -C .. setup-grub setup-c32 setup-musl

$(ENTRYOBJ): boot/entry.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(CONSOLEOBJ): boot/console.c boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(INTASMOBJ): boot/interrupts.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(INTOBJ): boot/interrupts.c boot/interrupts.h boot/syscall.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KBOBJ): boot/keyboard.c boot/keyboard.h boot/interrupts.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(PAGINGOBJ): boot/paging.c boot/paging.h boot/phys_mem.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(PHYSMEMOBJ): boot/phys_mem.c boot/phys_mem.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KHEAPOBJ): boot/kheap.c boot/kheap.h boot/paging.h boot/phys_mem.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TIMEROBJ): boot/timer.c boot/timer.h boot/interrupts.h boot/thread.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(CTXOBJ): boot/context_switch.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(GDTASMOBJ): boot/gdt.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(GDTOBJ): boot/gdt.c boot/gdt.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SYSENTRYOBJ): boot/syscall_entry.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(THREADOBJ): boot/thread.c boot/thread.h boot/kheap.h boot/interrupts.h boot/timer.h boot/gdt.h boot/paging.h boot/syscall.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(MONITOROBJ): boot/monitor.c boot/monitor.h boot/keyboard.h boot/kheap.h boot/phys_mem.h boot/thread.h boot/timer.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(WQOBJ): boot/wait_queue.c boot/wait_queue.h boot/thread.h boot/kheap.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SYSCALLOBJ): boot/syscall.c boot/syscall.h boot/process.h boot/vfs.h boot/gdt.h boot/paging.h boot/interrupts.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(PROCESSOBJ): boot/process.c boot/process.h boot/console_dev.h boot/vfs.h boot/wait_queue.h boot/thread.h boot/elf.h boot/initrd.h boot/paging.h boot/phys_mem.h boot/kheap.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(INITRDOBJ): boot/initrd.c boot/initrd.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELFOBJ): boot/elf.c boot/elf.h boot/paging.h boot/phys_mem.h boot/process.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(VFSOBJ): boot/vfs.c boot/vfs.h boot/kheap.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(CONDEVOBJ): boot/console_dev.c boot/console_dev.h boot/vfs.h boot/console.h boot/kheap.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELOBJ): boot/kernel.c boot/gdt.h boot/syscall.h boot/phys_mem.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELELF): $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KBOBJ) $(PAGINGOBJ) $(PHYSMEMOBJ) $(KHEAPOBJ) $(TIMEROBJ) $(CTXOBJ) $(GDTASMOBJ) $(GDTOBJ) $(SYSENTRYOBJ) $(THREADOBJ) $(MONITOROBJ) $(WQOBJ) $(SYSCALLOBJ) $(INITRDOBJ) $(ELFOBJ) $(VFSOBJ) $(CONDEVOBJ) $(PROCESSOBJ) $(KERNELOBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KBOBJ) $(PAGINGOBJ) $(PHYSMEMOBJ) $(KHEAPOBJ) $(TIMEROBJ) $(CTXOBJ) $(GDTASMOBJ) $(GDTOBJ) $(SYSENTRYOBJ) $(THREADOBJ) $(MONITOROBJ) $(WQOBJ) $(SYSCALLOBJ) $(INITRDOBJ) $(ELFOBJ) $(VFSOBJ) $(CONDEVOBJ) $(PROCESSOBJ) $(KERNELOBJ)
	$(GRUBFILE) --is-x86-multiboot $@ || true

USERCFLAGS   := -m64 -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
                -fno-pic -fno-pie -fno-stack-protector -mno-red-zone \
                -mno-sse -mno-sse2 -mno-mmx

$(USEROBJ): user/init.c | $(BUILD)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(HELLOOBJ): user/hello.c | $(BUILD)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(HELLO2OBJ): user/hello2.c | $(BUILD)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(BRKOBJ): user/brk.c | $(BUILD)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(MMAPOBJ): user/mmap.c | $(BUILD)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(TLSOBJ): user/tls.c | $(BUILD)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(SYSCALL64OBJ): user/syscall64.c | $(BUILD)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(CLONEOBJ): user/clone.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(USERINIT): $(USEROBJ) $(CLONEOBJ) user/user.ld
	$(LD) -m elf_x86_64 -T user/user.ld -nostdlib -o $@ $(USEROBJ) $(CLONEOBJ)

$(USERHELLO): $(HELLOOBJ) $(CLONEOBJ) user/user.ld
	$(LD) -m elf_x86_64 -T user/user.ld -nostdlib -o $@ $(HELLOOBJ) $(CLONEOBJ)

$(USERHELLO2): $(HELLO2OBJ) $(CLONEOBJ) user/user.ld
	$(LD) -m elf_x86_64 -T user/user.ld -nostdlib -o $@ $(HELLO2OBJ) $(CLONEOBJ)

$(USERBRK): $(BRKOBJ) $(CLONEOBJ) user/user.ld
	$(LD) -m elf_x86_64 -T user/user.ld -nostdlib -o $@ $(BRKOBJ) $(CLONEOBJ)

$(USERMMAP): $(MMAPOBJ) $(CLONEOBJ) user/user.ld
	$(LD) -m elf_x86_64 -T user/user.ld -nostdlib -o $@ $(MMAPOBJ) $(CLONEOBJ)

$(USERTLS): $(TLSOBJ) $(CLONEOBJ) user/user.ld
	$(LD) -m elf_x86_64 -T user/user.ld -nostdlib -o $@ $(TLSOBJ) $(CLONEOBJ)

$(USERSYSCALL64): $(SYSCALL64OBJ) $(CLONEOBJ) user/user.ld
	$(LD) -m elf_x86_64 -T user/user.ld -nostdlib -o $@ $(SYSCALL64OBJ) $(CLONEOBJ)

$(USERMUSLHELLO): user/musl_hello.c | $(BUILD)
	$(MUSLCC) -static -O2 -s -o $@ $<

$(INITRAMFS): $(USERINIT) $(USERHELLO) $(USERHELLO2) $(USERBRK) $(USERMMAP) $(USERTLS) $(USERSYSCALL64) $(USERMUSLHELLO) | $(BUILD)
	cp $(USERINIT) $(INITRDDIR)/init
	cp $(USERHELLO) $(INITRDDIR)/hello
	cp $(USERHELLO2) $(INITRDDIR)/hello2
	cp $(USERBRK) $(INITRDDIR)/brk
	cp $(USERMMAP) $(INITRDDIR)/mmap
	cp $(USERTLS) $(INITRDDIR)/tls
	cp $(USERSYSCALL64) $(INITRDDIR)/syscall64
	cp $(USERMUSLHELLO) $(INITRDDIR)/musl_hello
	cd $(INITRDDIR) && find . | sort | cpio -o --format=newc > ../$(INITRAMFS) 2>/dev/null

$(ISO): $(KERNELELF) $(INITRAMFS) grub/grub.cfg | $(BUILD)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNELELF) $(ISO_ROOT)/boot/kernel.elf
	cp $(INITRAMFS) $(ISO_ROOT)/boot/initramfs.cpio
	cp grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUBMKRESCUE) -o $@ $(ISO_ROOT)

$(BUILD):
	mkdir -p $(BUILD)

run: $(ISO)
	$(QEMU) -cdrom $(ISO)

run-nogui: $(ISO)
	timeout 5 $(QEMU) -cdrom $(ISO) $(RUNNOGUI) || true

clean:
	rm -rf $(BUILD)
	rm -f $(INITRDDIR)/init $(INITRDDIR)/hello $(INITRDDIR)/hello2 $(INITRDDIR)/brk $(INITRDDIR)/mmap $(INITRDDIR)/tls $(INITRDDIR)/syscall64 $(INITRDDIR)/musl_hello
