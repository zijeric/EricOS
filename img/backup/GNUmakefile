QEMU=qemu-system-x86_64
CPUS ?= 3
QEMUOPTS = -drive file=./kernel.img,index=0,media=disk,format=raw -serial mon:stdio -vga std -m 256
QEMUOPTS += -smp $(CPUS)
QEMUOPTS += -drive file=./fs.img,index=1,media=disk,format=raw
run:
	$(QEMU) -nographic $(QEMUOPTS)