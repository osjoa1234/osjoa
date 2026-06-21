CORE_PKGS := nasm qemu-system-x86

.PHONY: setup setup-core

setup: setup-core

setup-core:
	sudo apt update
	sudo apt install -y $(CORE_PKGS)
