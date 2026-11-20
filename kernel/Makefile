obj-m += idw_core.o
idw_core-y := idw_core_main.o idw_ntshim.o
obj-m += idw_test_miniport.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
.PHONY: all clean
