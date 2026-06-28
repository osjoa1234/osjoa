CORE_PKGS := nasm qemu-system-x86
FAT_PKGS  := mtools dosfstools

.PHONY: setup setup-core setup-fat apt-update

setup: setup-core setup-fat

setup-core: apt-update
	sudo apt install -y $(CORE_PKGS)

setup-fat: apt-update
	sudo apt install -y $(FAT_PKGS)

apt-update:
	sudo apt update
