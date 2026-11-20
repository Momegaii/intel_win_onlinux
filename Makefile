CXX ?= g++
CC ?= gcc
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libdrm)
LDLIBS += $(shell $(PKG_CONFIG) --libs libdrm)
BUILD := build
BIN := $(BUILD)/idwctl
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean test contract-test backend-contract-test i915-resource-test i915-memory-backend-test gpu-submission-test memory-manager-test abi-test ppgtt-model-test pt-dma-adapter-test ggtt-allocator-test haswell-gtt-hw-test device-lifecycle-test stage3a-test stage3a-sanitize-test cli-test gpu-test gpu-resource-test gpu-memory-test gpu-memory-manager-test gpu-resource-write-test qemu-dma-test install-user install-system-helper kernel kernel-clean
all: $(BIN)
$(BUILD)/gpuva_model.o: src/gpuva_model.c include/idw/gpuva_model.h \
		include/idw/contracts/base.h
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -pedantic -Iinclude -c $< -o $@
$(BIN): src/idwctl.cpp src/i915_gem_resource_backend.cpp \
		src/i915_memory_backend.cpp src/memory_manager.cpp \
		src/gpu_submission.cpp include/uapi/idw_ioctl.h \
		include/idw/i915_gem_resource_backend.hpp \
		include/idw/i915_memory_backend.hpp include/idw/gpu_submission.hpp \
		include/idw/memory_manager.hpp $(BUILD)/gpuva_model.o
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/idwctl.cpp \
		src/i915_gem_resource_backend.cpp src/i915_memory_backend.cpp \
		src/memory_manager.cpp src/gpu_submission.cpp $(BUILD)/gpuva_model.o \
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
memory-manager-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
		-c src/gpuva_model.c -o $(BUILD)/gpuva_model-memory-test.o
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -Iinclude \
		src/memory_manager.cpp $(BUILD)/gpuva_model-memory-test.o \
		tests/memory-manager-test.cpp \
		-o $(BUILD)/memory-manager-test
	./$(BUILD)/memory-manager-test
i915-memory-backend-test: $(BUILD)/gpuva_model.o
	$(CXX) $(CPPFLAGS) -std=c++17 -Wall -Wextra -Werror -Iinclude \
		src/memory_manager.cpp src/i915_gem_resource_backend.cpp \
		src/i915_memory_backend.cpp src/gpu_submission.cpp \
		tests/i915-memory-backend-test.cpp \
		$(BUILD)/gpuva_model.o -o $(BUILD)/i915-memory-backend-test $(LDLIBS)
	./$(BUILD)/i915-memory-backend-test
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
pt-dma-adapter-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
		src/gpuva_model.c src/page_table_memory.c src/haswell_ppgtt_model.c \
		src/pt_dma_adapter.c src/native_dma_contract.c \
		tests/pt-dma-adapter-test.c \
		-o build/pt-dma-adapter-test
	./build/pt-dma-adapter-test
ggtt-allocator-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
		src/ggtt_allocator.c tests/ggtt-allocator-test.c \
		-o build/ggtt-allocator-test
	./build/ggtt-allocator-test
haswell-gtt-hw-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
		src/ggtt_allocator.c src/haswell_gtt_hw.c src/ggtt_directory.c \
		src/page_table_memory.c src/haswell_ppgtt_model.c \
		tests/haswell-gtt-hw-test.c -o build/haswell-gtt-hw-test
	./build/haswell-gtt-hw-test
device-lifecycle-test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
		src/device_lifecycle.c tests/device-lifecycle-test.c \
		-o build/device-lifecycle-test
	./build/device-lifecycle-test
stage3a-test: ggtt-allocator-test haswell-gtt-hw-test device-lifecycle-test
stage3a-sanitize-test:
	@mkdir -p $(BUILD)/sanitize
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude -O1 -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		src/ggtt_allocator.c tests/ggtt-allocator-test.c \
		-o build/sanitize/ggtt-allocator-test
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude -O1 -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		src/device_lifecycle.c tests/device-lifecycle-test.c \
		-o build/sanitize/device-lifecycle-test
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude -O1 -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		src/ggtt_allocator.c src/haswell_gtt_hw.c src/ggtt_directory.c \
		src/page_table_memory.c src/haswell_ppgtt_model.c \
		tests/haswell-gtt-hw-test.c -o build/sanitize/haswell-gtt-hw-test
	ASAN_OPTIONS=detect_leaks=1 ./build/sanitize/ggtt-allocator-test
	ASAN_OPTIONS=detect_leaks=1 ./build/sanitize/device-lifecycle-test
	ASAN_OPTIONS=detect_leaks=1 ./build/sanitize/haswell-gtt-hw-test
cli-test: all
	./tests/cli-arguments-smoke.sh
test: all contract-test backend-contract-test i915-resource-test i915-memory-backend-test gpu-submission-test memory-manager-test abi-test ppgtt-model-test pt-dma-adapter-test ggtt-allocator-test haswell-gtt-hw-test device-lifecycle-test cli-test
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
gpu-memory-manager-test: all
	./tests/gpu-memory-manager-smoke.sh
gpu-resource-write-test: all
	./tests/gpu-resource-write-smoke.sh
qemu-dma-test:
	./tests/qemu-dma/run.sh
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
