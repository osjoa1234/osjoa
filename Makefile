CORE_PKGS := nasm qemu-system-x86
FAT_PKGS  := mtools dosfstools
C32_PKGS  := gcc-multilib g++-multilib
GRUB_PKGS := xorriso grub-pc-bin grub-common
INIT_PKGS := cpio
MUSL_PKGS := musl-tools

.PHONY: setup setup-core setup-fat setup-c32 setup-grub setup-init setup-musl apt-update

setup: setup-core setup-fat setup-c32 setup-grub setup-init setup-musl

setup-core: apt-update
	sudo apt install -y $(CORE_PKGS)

setup-fat: apt-update
	sudo apt install -y $(FAT_PKGS)

setup-c32: apt-update
	sudo apt install -y $(C32_PKGS)

setup-grub: apt-update
	sudo apt install -y $(GRUB_PKGS)

setup-init: apt-update
	sudo apt install -y $(INIT_PKGS)

setup-musl: apt-update
	sudo apt install -y $(MUSL_PKGS)

apt-update:
	sudo apt update
