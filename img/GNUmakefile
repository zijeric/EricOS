QEMU=qemu-system-x86_64
QEMUOPTS = -drive file=./kernel.img, index=0, media=disk, format=raw -serial mon:stdio -vga std -m 256
run:
	$(QEMU) -nographic $(QEMUOPTS)
