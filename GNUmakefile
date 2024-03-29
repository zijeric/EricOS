#
# AlvOS GNUmakefile 遵循 Peter Miller 在其优秀论文中推荐的结构化约定:
#
#	title: Recursive Make Considered Harmful
#	http://aegis.sourceforge.net/auug97.pdf
#
OBJDIR := obj

# Run 'make V=1' to turn on verbose commands, or 'make V=0' to turn them off.
V = @

LABSETUP ?= ./

TOP = .

ifndef QEMU
QEMU := $(shell if which qemu-system-x86_64 > /dev/null; \
	then echo qemu-system-x86_64; exit; \
	else \
	qemu=/Applications/Q.app/Contents/MacOS/i386-softmmu.app/Contents/MacOS/i386-softmmu; \
	if test -x $$qemu; then echo $$qemu; exit; fi; fi;)
endif
# 尝试生成一个唯一的 GDB 端口
GDBPORT	:= $(shell expr `id -u` % 5000 + 25000)

CC	:= gcc -pipe
AS	:= as
AR	:= ar
LD	:= ld
OBJCOPY	:= objcopy
OBJDUMP	:= objdump
NM	:= nm

# 本机命令
NCC	:= gcc $(CC_VER) -pipe
NATIVE_CFLAGS := $(CFLAGS) $(DEFS) $(LABDEFS) -I$(TOP) -MD -Wall
TAR	:= gtar
PERL	:= perl

# 编译器标志
# 必须使用 -fno-builtin 来避免对内核中未定义函数的引用
# 为了阻止内联而只优化为 -O1，这会使回溯复杂化
CFLAGS := $(CFLAGS) $(DEFS) $(LABDEFS) -O0 -fno-builtin -I$(TOP) -MD
CFLAGS += -fno-omit-frame-pointer -mno-red-zone
CFLAGS += -Wall -Wno-format -Wno-unused -gdwarf-2 -fno-PIC -fno-stack-protector # -Werror 

CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# 通用的链接器标志
LDFLAGS := -m elf_x86_64 -z max-page-size=0x1000 --print-gc-sections
BOOT_LDFLAGS := -m elf_i386

# AlvOS 用户程序的链接器标志
ULDFLAGS := -T user/user.ld

GCC_LIB := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name)

# 列出将要添加的 */Makefrag (makefile 片段)
OBJDIRS :=

# 确保 'all' 是第一个目标
all:

# 消除默认后缀规则
.SUFFIXES:

# 如果出现错误(或make被中断)，删除目标文件
.DELETE_ON_ERROR:

# 确保不会删除任何中间生成的 .o 文件
.PRECIOUS: %.o $(OBJDIR)/boot/%.o $(OBJDIR)/kern/%.o \
	   $(OBJDIR)/lib/%.o $(OBJDIR)/fs/%.o $(OBJDIR)/net/%.o \
	   $(OBJDIR)/user/%.o

KERN_CFLAGS := $(CFLAGS) -DALVOS_KERNEL -DDWARF_SUPPORT -gdwarf-2 -mcmodel=large -m64
BOOT_CFLAGS := $(CFLAGS) -DALVOS_KERNEL -gdwarf-2 -m32
USER_CFLAGS := $(CFLAGS) -DALVOS_USER -gdwarf-2 -mcmodel=large -m64

# 如果变量 X 自上次 make 运行以来发生了更改，则更新.vars.X.
#
# 变量 X 的规则应该依赖于 $(OBJDIR)/.vars.X
# 如果变量的值发生了变化，就更新vars文件，并强制重新构建(Make)依赖于它的规则.
$(OBJDIR)/.vars.%: FORCE
	$(V)echo "$($*)" | cmp -s $@ || echo "$($*)" > $@
.PRECIOUS: $(OBJDIR)/.vars.%
.PHONY: FORCE


# 包含子目录的编译脚本 Makefile
include boot/Makefrag
include kern/Makefrag
include lib/Makefrag
include user/Makefrag

# 指定 qemu-system-x86_64 -smp 后的参数，即模拟的处理器个数
CPUS ?= 3

# 配置内核内存大小，限制为256MB，并将其编译为 obj/kern/kernel.img
QEMUOPTS = -m 256 -hda $(OBJDIR)/kern/kernel.img -serial mon:stdio -gdb tcp::$(GDBPORT)
QEMUOPTS += $(shell if $(QEMU) -nographic -help | grep -q '^-D '; then echo '-D qemu.log'; fi)
# 配置内核映像的路径，并挂载
IMAGES = $(OBJDIR)/kern/kernel.img
QEMUOPTS += -smp $(CPUS)
QEMUOPTS += $(QEMUEXTRA)


.gdbinit:$(OBJDIR)/kern/kernel.asm .gdbinit.tmpl
	$(eval LONGMODE := $(shell grep -C1 '<jumpto_longmode>:' '$(OBJDIR)/kern/kernel.asm' | sed 's/^\([0-9a-f]*\).*/\1/g'))
	sed -e "s/localhost:1234/localhost:$(GDBPORT)/" -e "s/jumpto_longmode/*0x$(LONGMODE)/" < $^ > $@

pre-qemu: .gdbinit

qemu: $(IMAGES) pre-qemu
	$(QEMU) $(QEMUOPTS)

qemu-nox: $(IMAGES) pre-qemu
	$(QEMU) -nographic $(QEMUOPTS)

qemu-gdb: $(IMAGES) pre-qemu
	$(QEMU) $(QEMUOPTS) -S

qemu-nox-gdb: $(IMAGES) pre-qemu
	$(QEMU) -nographic $(QEMUOPTS) -S

print-qemu:
	@echo $(QEMU)

print-gdbport:
	@echo $(GDBPORT)

# 用于删除编译结果文件
clean:
	rm -rf $(OBJDIR) .gdbinit alvos.in qemu.log

realclean: clean
	rm -rf lab$(LAB).tar.gz \
		alvos.out $(wildcard alvos.out.*) \
		qemu.pcap $(wildcard qemu.pcap.*)

distclean: realclean
	rm -rf conf/gcc.mk

ifneq ($(V),@)
GRADEFLAGS += -v
endif

# 参考 MIT OS 源码，这种神奇的方法会自动为所编译的C源文件中包含的头文件生成makefile依赖项，并在每次重新编译时使这些依赖项保持最新
$(OBJDIR)/.deps: $(foreach dir, $(OBJDIRS), $(wildcard $(OBJDIR)/$(dir)/*.d))
	@mkdir -p $(@D)
	@$(PERL) mergedep.pl $@ $^

-include $(OBJDIR)/.deps

always:
	@:

.PHONY: all always \
	clean realclean distclean
