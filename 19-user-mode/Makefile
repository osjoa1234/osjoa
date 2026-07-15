ASM          := nasm
CC           := gcc
LD           := ld
QEMU         := qemu-system-i386
GRUBFILE     := grub-file
GRUBMKRESCUE := grub-mkrescue

BUILD        := build
ISO_ROOT     := $(BUILD)/isofiles
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
KERNELOBJ    := $(BUILD)/kernel.o
KERNELELF    := $(BUILD)/kernel.elf
ISO          := $(BUILD)/os.iso

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

$(INTOBJ): boot/interrupts.c boot/interrupts.h boot/console.h | $(BUILD)
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

$(THREADOBJ): boot/thread.c boot/thread.h boot/kheap.h boot/interrupts.h boot/timer.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(MONITOROBJ): boot/monitor.c boot/monitor.h boot/keyboard.h boot/kheap.h boot/phys_mem.h boot/thread.h boot/timer.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELOBJ): boot/kernel.c boot/gdt.h boot/interrupts.h boot/keyboard.h boot/kheap.h boot/monitor.h boot/paging.h boot/phys_mem.h boot/thread.h boot/timer.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELELF): $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KBOBJ) $(PAGINGOBJ) $(PHYSMEMOBJ) $(KHEAPOBJ) $(TIMEROBJ) $(CTXOBJ) $(GDTASMOBJ) $(GDTOBJ) $(THREADOBJ) $(MONITOROBJ) $(KERNELOBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KBOBJ) $(PAGINGOBJ) $(PHYSMEMOBJ) $(KHEAPOBJ) $(TIMEROBJ) $(CTXOBJ) $(GDTASMOBJ) $(GDTOBJ) $(THREADOBJ) $(MONITOROBJ) $(KERNELOBJ)
	$(GRUBFILE) --is-x86-multiboot $@

$(ISO): $(KERNELELF) grub/grub.cfg | $(BUILD)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNELELF) $(ISO_ROOT)/boot/kernel.elf
	cp grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUBMKRESCUE) -o $@ $(ISO_ROOT)

$(BUILD):
	mkdir -p $(BUILD)

run: $(ISO)
	$(QEMU) -cdrom $(ISO)

run-nogui: $(ISO)
	timeout 3 $(QEMU) -cdrom $(ISO) $(RUNNOGUI) || true

clean:
	rm -rf $(BUILD)
