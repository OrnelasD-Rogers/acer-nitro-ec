obj-m := acer-nitro-wmi.o
acer-nitro-wmi-objs := wmi-version.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

LLVM ?= 1

all:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) modules_install
	depmod -a

.PHONY: all clean install
