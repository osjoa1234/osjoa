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

$(KERNELOBJ): boot/kernel.c boot/interrupts.h boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELELF): $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KERNELOBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ENTRYOBJ) $(CONSOLEOBJ) $(INTASMOBJ) $(INTOBJ) $(KERNELOBJ)
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
	$(QEMU) -cdrom $(ISO) $(RUNNOGUI) || test $$? -eq 1

clean:
	rm -rf $(BUILD)