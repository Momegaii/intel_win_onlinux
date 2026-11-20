CXX ?= g++
CC ?= gcc
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libdrm)
LDLIBS += $(shell $(PKG_CONFIG) --libs libdrm)
BUILD := build
BIN := $(BUILD)/idwctl
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean test contract-test backend-contract-test i915-resource-test gpu-submission-test abi-test ppgtt-model-test cli-test gpu-test gpu-resource-test gpu-memory-test install-user install-system-helper kernel kernel-clean
all: $(BIN)
$(BIN): src/idwctl.cpp src/i915_gem_resource_backend.cpp \
		src/gpu_submission.cpp include/uapi/idw_ioctl.h \
		include/idw/i915_gem_resource_backend.hpp include/idw/gpu_submission.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/idwctl.cpp \
		src/i915_gem_resource_backend.cpp src/gpu_submission.cpp \
		-o $@ $(LDLIBS)
contract-test:
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude -fsyntax-only tests/contracts/contracts_compile.c
backend-contract-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
		src/page_table_memory.c src/i915_assisted_backend.c \
		src/native_dma_contract.c tests/page-table-memory-contract-test.c \
		-o $(BUILD)/page-table-memory-contract-test
	./$(BUILD)/page-table-memory-contract-test
i915-resource-test:
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/i915_gem_resource_backend.cpp \
		tests/i915-gem-resource-backend-test.cpp \
		-o $(BUILD)/i915-gem-resource-backend-test $(LDLIBS)
	./$(BUILD)/i915-gem-resource-backend-test
gpu-submission-test:
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Iinclude \
		src/gpu_submission.cpp tests/gpu-submission-test.cpp \
		-o $(BUILD)/gpu-submission-test
	./$(BUILD)/gpu-submission-test
abi-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude -o build/abi-layout-test tests/abi-layout-test.c
	./build/abi-layout-test
ppgtt-model-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
		src/gpuva_model.c src/page_table_memory.c src/haswell_ppgtt_model.c \
		tests/ppgtt-model-test.c \
		-o build/ppgtt-model-test
	./build/ppgtt-model-test
cli-test: all
	./tests/cli-arguments-smoke.sh
test: all contract-test backend-contract-test i915-resource-test gpu-submission-test abi-test ppgtt-model-test cli-test
	./tests/smoke.sh
	./tests/gem-resource-backend-smoke.sh
	./tests/kernel-query-smoke.sh
	./tests/gem-lifecycle-smoke.sh
	./tests/idw-resource-smoke.sh
	./tests/dmabuf-import-smoke.sh
gpu-test: all
	./tests/gpu-submit-smoke.sh
gpu-resource-test: all
	./tests/gpu-resource-smoke.sh
gpu-memory-test: all
	./tests/gpu-memory-smoke.sh
kernel:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel modules
kernel-clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean
clean:
	rm -rf $(BUILD)
install-user: all
	install -Dm755 $(BIN) "$(HOME)/.local/bin/idwctl"
	install -Dm755 gui/idw-gpu-properties.py "$(HOME)/.local/bin/idw-gpu-properties"
	install -Dm644 desktop/idw-gpu-properties.desktop "$(HOME)/.local/share/applications/idw-gpu-properties.desktop"
	@echo "Installed to ~/.local/bin and ~/.local/share/applications"
install-system-helper:
	install -Dm755 tools/idw-debugfs-snapshot /usr/local/libexec/idw-debugfs-snapshot
