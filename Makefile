# path: Makefile
#
# Out-of-tree kernel module build for u235fs.
#
#   make          - build u235fs.ko
#   make clean    - remove build artifacts
#
# Floating point note:
#   The decay model uses double-precision math (exp, scientific formatting).
#   The kernel disables SSE globally (-mno-sse); we re-enable it for OUR object
#   only so the compiler can emit SSE2 instructions. All actual floating-point
#   execution happens strictly inside kernel_fpu_begin()/kernel_fpu_end().
#   x86_64 only.

obj-m += u235fs.o
u235fs-y := src/u235fs.o

CFLAGS_src/u235fs.o += -msse -msse2 -mpreferred-stack-boundary=4 -fno-tree-vectorize

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	@rm -f *.o *.ko *.mod *.mod.c modules.order Module.symvers .*.cmd 2>/dev/null || true
	@rm -f src/*.o src/*.mod* src/.*.cmd 2>/dev/null || true

.PHONY: all clean
