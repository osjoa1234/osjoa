ASM          := nasm
CC           := gcc
LD           := ld
QEMU         := qemu-system-i386
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
THREADOBJ    := $(BUILD)/thread.o
MONITOROBJ   := $(BUILD)/monitor.o
SYSCALLOBJ   := $(BUILD)/syscall.o
INITRDOBJ    := $(BUILD)/initrd.o
ELFOBJ       := $(BUILD)/elf.o
PROCESSOBJ   := $(BUILD)/process.o
KERNELOBJ    := $(BUILD)/kernel.o
KERNELELF    := $(BUILD)/kernel.elf
ISO          := $(BUILD)/os.iso

USERINIT     := $(BUILD)/init
USERHELLO    := $(BUILD)/hello
USERHELLO2   := $(BUILD)/hello2
USEROBJ      := $(BUILD)/init_user.o
HELLOOBJ     := $(BUILD)/hello_user.o
HELLO2OBJ    := $(BUILD)/hello2_user.o

CFLAGS       := -m32 -march=i386 -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector
LDFLAGS      := -m elf_i386 -T linker.ld -nostdlib
RUNNOGUI     := -display none -serial none -monitor none -debugcon stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04

.PHONY: all run run-nogui clean tools

all: $(ISO)

tools:
	$(MAKE) -C .. setup-grub setup-c32

$(ENTRYOBJ): boot/entry.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(CONSOLEOBJ): boot/console.c boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(INTASMOBJ): boot/interrupts.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

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
	$(ASM) -f elf32 $< -o $@

$(GDTASMOBJ): boot/gdt.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(GDTOBJ): boot/gdt.c boot/gdt.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(THREADOBJ): boot/thread.c boot/thread.h boot/kheap.h boot/interrupts.h boot/timer.h boot/gdt.h boot/paging.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(MONITOROBJ): boot/monitor.c boot/monitor.h boot/keyboard.h boot/kheap.h boot/phys_mem.h boot/thread.h boot/timer.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SYSCALLOBJ): boot/syscall.c boot/syscall.h boot/process.h boot/initrd.h boot/interrupts.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(PROCESSOBJ): boot/process.c boot/process.h boot/thread.h boot/elf.h boot/initrd.h boot/paging.h boot/timer.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(INITRDOBJ): boot/initrd.c boot/initrd.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELFOBJ): boot/elf.c boot/elf.h boot/paging.h boot/phys_mem.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELOBJ): boot/kernel.c boot/gdt.h boot/initrd.h boot/interrupts.h boot/keyboard.h boot/kheap.h boot/monitor.h boot/paging.h boot/phys_mem.h boot/process.h boot/syscall.h boot/thread.h boot/timer.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELELF): $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KBOBJ) $(PAGINGOBJ) $(PHYSMEMOBJ) $(KHEAPOBJ) $(TIMEROBJ) $(CTXOBJ) $(GDTASMOBJ) $(GDTOBJ) $(THREADOBJ) $(MONITOROBJ) $(SYSCALLOBJ) $(INITRDOBJ) $(ELFOBJ) $(PROCESSOBJ) $(KERNELOBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KBOBJ) $(PAGINGOBJ) $(PHYSMEMOBJ) $(KHEAPOBJ) $(TIMEROBJ) $(CTXOBJ) $(GDTASMOBJ) $(GDTOBJ) $(THREADOBJ) $(MONITOROBJ) $(SYSCALLOBJ) $(INITRDOBJ) $(ELFOBJ) $(PROCESSOBJ) $(KERNELOBJ)
	$(GRUBFILE) --is-x86-multiboot $@

$(USEROBJ): user/init.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(USERINIT): $(USEROBJ) user/user.ld
	$(LD) -m elf_i386 -T user/user.ld -nostdlib -o $@ $(USEROBJ)

$(HELLOOBJ): user/hello.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(USERHELLO): $(HELLOOBJ) user/user.ld
	$(LD) -m elf_i386 -T user/user.ld -nostdlib -o $@ $(HELLOOBJ)

$(HELLO2OBJ): user/hello2.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(USERHELLO2): $(HELLO2OBJ) user/user.ld
	$(LD) -m elf_i386 -T user/user.ld -nostdlib -o $@ $(HELLO2OBJ)

$(INITRAMFS): $(USERINIT) $(USERHELLO) $(USERHELLO2) | $(BUILD)
	cp $(USERINIT) $(INITRDDIR)/init
	cp $(USERHELLO) $(INITRDDIR)/hello
	cp $(USERHELLO2) $(INITRDDIR)/hello2
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
	rm -f $(INITRDDIR)/init $(INITRDDIR)/hello $(INITRDDIR)/hello2
