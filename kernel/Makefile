obj-m += idw_core.o
idw_core-y := idw_core_main.o idw_ntshim.o idw_dma_memory.o
obj-m += idw_test_miniport.o
obj-m += idw_edu_dma_test.o
idw_edu_dma_test-objs := idw_edu_dma_test_main.o idw_dma_pt_adapter.o \
	pt_dma_adapter_shared.o page_table_memory_shared.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
.PHONY: all clean
