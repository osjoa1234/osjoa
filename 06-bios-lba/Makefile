ASM      := nasm
QEMU     := qemu-system-i386

BUILD    := build
BOOT     := $(BUILD)/boot.bin
KERNEL   := $(BUILD)/KERNEL.BIN
IMG      := $(BUILD)/os.img
IMG_SIZE := 16M
IMG_GEOM := 16/63

.PHONY: all run run-nogui clean tools

all: $(IMG)

tools:
	$(MAKE) -C .. setup-fat

$(BOOT): boot/boot.asm | $(BUILD)
	$(ASM) -f bin $< -o $@

$(KERNEL): boot/kernel.asm | $(BUILD)
	$(ASM) -f bin $< -o $@

$(IMG): $(BOOT) $(KERNEL)
	truncate -s $(IMG_SIZE) $@
	mkfs.fat -F 16 -s 1 -g $(IMG_GEOM) -n CUSTOMOS $@
	mcopy -i $@ -o $(KERNEL) ::KERNEL.BIN
	dd if=$(BOOT) of=$@ bs=1 count=11 conv=notrunc
	dd if=$(BOOT) of=$@ bs=1 skip=62 seek=62 conv=notrunc

$(BUILD):
	mkdir -p $(BUILD)

run: $(IMG)
	$(QEMU) -drive format=raw,file=$(IMG)

run-nogui: $(IMG)
	$(QEMU) -drive format=raw,file=$(IMG) -display none -serial mon:stdio

clean:
	rm -rf $(BUILD)
