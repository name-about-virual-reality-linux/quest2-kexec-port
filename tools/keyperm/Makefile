# Out-of-tree build of keyperm against the running Android kernel tree.
# Same recipe as quest-kexec/module (see NOTES.md).
ifneq ($(KERNELRELEASE),)
obj-m += keyperm.o kextract.o
ccflags-y += -I$(KSRC)/fs/crypto
else
KDIR ?= $(error set KDIR to the built Android kernel output directory)
KSRC ?= $(KDIR)
LOCALVERSION ?=
KMAKE = $(MAKE) -C $(abspath $(KDIR)) M=$(CURDIR) ARCH=arm64 CC=clang LD=ld.lld \
	HOSTCC=clang CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
	LOCALVERSION=$(LOCALVERSION)
.PHONY: all clean
all:
	$(KMAKE) modules
clean:
	$(KMAKE) clean
endif
