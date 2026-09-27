obj-m += netdisplay_power.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)" modules
clean:
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)" clean
