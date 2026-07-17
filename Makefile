CORE_PKGS := nasm qemu-system-x86
FAT_PKGS  := mtools dosfstools
C32_PKGS  := gcc-multilib g++-multilib
GRUB_PKGS := xorriso grub-pc-bin grub-common
INIT_PKGS := cpio

.PHONY: setup setup-core setup-fat setup-c32 setup-grub setup-init apt-update

setup: setup-core setup-fat setup-c32 setup-grub setup-init

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

apt-update:
	sudo apt update
