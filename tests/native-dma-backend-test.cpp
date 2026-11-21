/* SPDX-License-Identifier: MIT */
#include "../include/idw/memory_manager.hpp"
#include "../include/idw/native_dma_backend.hpp"
#include "../include/uapi/idw_dma_test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__,   \
                         #condition);                                        \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(int argc, char **argv)
{
    if (::access(IDW_DMA_TEST_DEVICE, F_OK) != 0) {
        const bool required =
            argc > 1 && std::strcmp(argv[1], "--require-device") == 0;
        if (required) {
            std::fprintf(stderr, "FAIL: required device %s is not present\n",
                         IDW_DMA_TEST_DEVICE);
            return 1;
        }
        std::printf("SKIP: %s is not present; run under the QEMU EDU harness\n",
                    IDW_DMA_TEST_DEVICE);
        return 0;
    }

    idw::NativeDmaMemoryBackend backend;
    idw::MemoryManager manager;
    int ret = manager.initialize(backend.operations(), &backend, 0x10000,
                                 0x100000);
    CHECK(ret == 0);

    idw_handle handle = 0;
    ret = manager.create(3 * 4096, handle);
    CHECK(ret == 0);
    CHECK(handle != 0);
    CHECK(backend.allocation_count() == 1);

    uint64_t backing = 0;
    uint64_t bytes = 0;
    uint64_t cookie = 0;
    ret = backend.allocation_at(0, backing, bytes, cookie);
    CHECK(ret == 0);
    CHECK(bytes == 3 * 4096);
    CHECK(backing != 0);
    CHECK(cookie == backing);

    idw_gpuva va = 0;
    ret = manager.bind(handle, 0x20000, va);
    CHECK(ret == 0);
    CHECK(va == 0x20000);

    idw_gpuva duplicate_va = 0;
    ret = manager.bind(handle, 0x30000, duplicate_va);
    CHECK(ret == -EBUSY);

    uint64_t resource_dma0 = 0;
    uint64_t resource_dma1 = 0;
    ret = backend.resource_dma(backing, 0, resource_dma0);
    CHECK(ret == 0);
    ret = backend.resource_dma(backing, 4096, resource_dma1);
    CHECK(ret == 0);
    CHECK(resource_dma0 != resource_dma1);

    uint64_t translated = 0;
    ret = backend.translate(va, translated);
    CHECK(ret == 0);
    CHECK(translated == resource_dma0);
    ret = backend.translate(va + 4096, translated);
    CHECK(ret == 0);
    CHECK(translated == resource_dma1);

    ret = manager.cpu_write_dword(handle, 0, 0xdeadbeefU);
    CHECK(ret == 0);
    uint32_t value = 0;
    ret = manager.cpu_read_dword(handle, 0, value);
    CHECK(ret == 0);
    CHECK(value == 0xdeadbeefU);

    uint64_t ticket = 0;
    ret = manager.submit_write_dword(handle, 4, 0x12345678U, ticket);
    CHECK(ret == 0);
    CHECK(ticket != 0);
    ret = manager.wait(ticket, 1000000);
    CHECK(ret == 0);
    ret = manager.cpu_read_dword(handle, 4, value);
    CHECK(ret == 0);
    CHECK(value == 0x12345678U);

    ret = manager.destroy(handle);
    CHECK(ret == -EBUSY);

    ret = manager.unbind(handle);
    CHECK(ret == 0);
    ret = manager.destroy(handle);
    CHECK(ret == 0);
    CHECK(backend.allocation_count() == 0);

    ret = manager.shutdown();
    CHECK(ret == 0);
    CHECK(manager.resource_count() == 0);
    CHECK(manager.submission_count() == 0);

    ret = backend.close_all();
    CHECK(ret == 0);
    CHECK(backend.page_table_count() == 0);

    std::printf("PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup\n");
    return 0;
}
