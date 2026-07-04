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
KERNELOBJ    := $(BUILD)/kernel.o
KERNELELF    := $(BUILD)/kernel.elf
ISO          := $(BUILD)/os.iso

CFLAGS       := -m32 -march=i386 -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector
LDFLAGS      := -m elf_i386 -T linker.ld -nostdlib

.PHONY: all run run-nogui clean tools

all: $(ISO)

tools:
	$(MAKE) -C .. setup-grub setup-c32

$(ENTRYOBJ): boot/entry.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(CONSOLEOBJ): boot/console.c boot/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELOBJ): boot/kernel.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNELOBJ): boot/console.h

$(KERNELELF): $(ENTRYOBJ) $(CONSOLEOBJ) $(KERNELOBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ENTRYOBJ) $(CONSOLEOBJ) $(KERNELOBJ)
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
	$(QEMU) -cdrom $(ISO) -display none -serial mon:stdio

clean:
	rm -rf $(BUILD)