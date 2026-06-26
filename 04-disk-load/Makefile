ASM      := nasm
QEMU     := qemu-system-i386

BUILD    := build
SRC      := boot/boot.asm
BIN      := $(BUILD)/boot.bin

.PHONY: all run run-nogui clean

all: $(BIN)

$(BIN): $(SRC) | $(BUILD)
	$(ASM) -f bin $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(BIN)
	$(QEMU) -drive format=raw,file=$(BIN)

run-nogui: $(BIN)
	$(QEMU) -drive format=raw,file=$(BIN) -display none -serial mon:stdio

clean:
	rm -rf $(BUILD)
