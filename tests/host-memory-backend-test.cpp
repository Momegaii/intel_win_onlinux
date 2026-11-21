/* Proves the host backend owns real pages, not just a handle. */
#include "../include/idw/host_memory_backend.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <sys/mman.h>

namespace {

constexpr uint64_t kPageSize = 4096;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        return 1; \
    } \
} while (0)

bool pages_resident(void *address, uint64_t bytes)
{
    if (!address || !bytes || (bytes % kPageSize))
        return false;
    std::vector<unsigned char> residency(
        static_cast<size_t>(bytes / kPageSize));
    if (mincore(address, static_cast<size_t>(bytes), residency.data()) != 0)
        return false;
    for (unsigned char page : residency) {
        if (!(page & 1))
            return false;
    }
    return true;
}

int test_direct_backing(void)
{
    idw::HostMemoryBackend backend;
    const idw::MemoryBackendOps ops = backend.operations();
    uint64_t backing = 0;
    void *address = nullptr;
    uint64_t bytes = 0;
    idw_gpuva va = 0;
    uint32_t value = 0;

    CHECK(ops.create_backing(&backend, 3 * kPageSize, &backing) == 0);
    CHECK(backing != 0);
    CHECK(backend.allocation_count() == 1);
    CHECK(backend.bytes_allocated() == 3 * kPageSize);
    CHECK(backend.backing_info(backing, &address, &bytes) == 0);
    CHECK(address != nullptr && bytes == 3 * kPageSize);
    CHECK((reinterpret_cast<uintptr_t>(address) & (kPageSize - 1)) == 0);
    CHECK(pages_resident(address, bytes));

    CHECK(ops.cpu_write_dword(&backend, backing, 0, 0x11223344u) == 0);
    CHECK(ops.cpu_write_dword(&backend, backing, kPageSize, 0x55667788u) == 0);
    CHECK(ops.cpu_write_dword(&backend, backing, 2 * kPageSize,
                              0x99aabbccu) == 0);
    CHECK(ops.cpu_read_dword(&backend, backing, 0, &value) == 0);
    CHECK(value == 0x11223344u);
    CHECK(ops.cpu_read_dword(&backend, backing, kPageSize, &value) == 0);
    CHECK(value == 0x55667788u);
    CHECK(ops.cpu_read_dword(&backend, backing, 2 * kPageSize, &value) == 0);
    CHECK(value == 0x99aabbccu);
    CHECK(pages_resident(address, bytes));

    CHECK(ops.bind(&backend, backing, 0x20000, bytes, &va) == 0);
    CHECK(va == 0x20000);
    CHECK(backend.mapped_count() == 1);
    CHECK(ops.destroy_backing(&backend, backing) == -EBUSY);
    CHECK(ops.unbind(&backend, backing, 0x21000, bytes) == -EINVAL);
    CHECK(ops.unbind(&backend, backing, 0x20000, bytes) == 0);
    CHECK(backend.mapped_count() == 0);
    CHECK(ops.destroy_backing(&backend, backing) == 0);
    CHECK(backend.allocation_count() == 0);
    CHECK(backend.bytes_allocated() == 0);
    return 0;
}

int test_manager_lifecycle(void)
{
    idw::HostMemoryBackend backend;
    idw::MemoryManager manager;
    idw_handle handle = 0;
    idw_gpuva va = 0;
    uint64_t ticket = 0;
    uint32_t value = 0;
    idw::MemoryResourceInfo info{};

    CHECK(manager.initialize(backend.operations(), &backend, 0x10000,
                             0x200000) == 0);
    CHECK(manager.create(2 * kPageSize + 1, handle) == 0);
    CHECK(backend.allocation_count() == 1);
    CHECK(backend.bytes_allocated() == 3 * kPageSize);
    CHECK(manager.bind(handle, 0, va) == 0);
    CHECK(va == 0x10000);
    CHECK(backend.mapped_count() == 1);
    CHECK(manager.query(handle, info) == 0);
    CHECK(info.bound && info.bytes == 3 * kPageSize);

    CHECK(manager.submit_write_dword(handle, kPageSize, 0xfeedfaceu,
                                     ticket) == 0);
    CHECK(manager.wait(ticket, 1000000) == 0);
    CHECK(manager.cpu_read_dword(handle, kPageSize, value) == 0);
    CHECK(value == 0xfeedfaceu);
    CHECK(manager.cpu_write_dword(handle, 2 * kPageSize, 0x0badf00du) == 0);
    CHECK(manager.cpu_read_dword(handle, 2 * kPageSize, value) == 0);
    CHECK(value == 0x0badf00du);

    CHECK(manager.destroy(handle) == -EBUSY);
    CHECK(manager.unbind(handle) == 0);
    CHECK(backend.mapped_count() == 0);
    CHECK(manager.destroy(handle) == 0);
    CHECK(backend.allocation_count() == 0);
    CHECK(manager.shutdown() == 0);
    return 0;
}

int test_distinct_backings(void)
{
    idw::HostMemoryBackend backend;
    const idw::MemoryBackendOps ops = backend.operations();
    uint64_t first = 0;
    uint64_t second = 0;
    void *first_address = nullptr;
    void *second_address = nullptr;
    uint64_t first_bytes = 0;
    uint64_t second_bytes = 0;
    uint32_t value = 0;

    CHECK(ops.create_backing(&backend, kPageSize, &first) == 0);
    CHECK(ops.create_backing(&backend, kPageSize, &second) == 0);
    CHECK(first != second);
    CHECK(backend.allocation_count() == 2);
    CHECK(backend.backing_info(first, &first_address, &first_bytes) == 0);
    CHECK(backend.backing_info(second, &second_address, &second_bytes) == 0);
    CHECK(first_address != second_address);
    CHECK(first_bytes == kPageSize && second_bytes == kPageSize);
    CHECK(pages_resident(first_address, first_bytes));
    CHECK(pages_resident(second_address, second_bytes));
    CHECK(ops.cpu_write_dword(&backend, first, 0, 0x11111111u) == 0);
    CHECK(ops.cpu_write_dword(&backend, second, 0, 0x22222222u) == 0);
    CHECK(ops.cpu_read_dword(&backend, first, 0, &value) == 0);
    CHECK(value == 0x11111111u);
    CHECK(ops.cpu_read_dword(&backend, second, 0, &value) == 0);
    CHECK(value == 0x22222222u);
    CHECK(ops.destroy_backing(&backend, first) == 0);
    CHECK(ops.destroy_backing(&backend, second) == 0);
    CHECK(backend.allocation_count() == 0);
    return 0;
}

int test_rejections_and_cleanup(void)
{
    idw::HostMemoryBackend backend;
    const idw::MemoryBackendOps ops = backend.operations();
    uint64_t backing = 0;
    uint64_t fence = 0;
    idw_gpuva va = 0;
    uint32_t value = 0;

    CHECK(ops.create_backing(&backend, 0, &backing) == -EINVAL);
    CHECK(ops.create_backing(&backend, 1, &backing) == -EINVAL);
    CHECK(ops.create_backing(&backend, kPageSize, &backing) == 0);
    CHECK(ops.bind(&backend, backing, 0, kPageSize, &va) == -EINVAL);
    CHECK(ops.bind(&backend, backing, 0x10000, kPageSize, &va) == 0);
    CHECK(ops.bind(&backend, backing, 0x10000, kPageSize, &va) == -EBUSY);
    CHECK(ops.cpu_read_dword(&backend, backing, kPageSize, &value) == -EINVAL);
    CHECK(ops.cpu_write_dword(&backend, backing, 2, 1) == -EINVAL);
    CHECK(ops.submit(&backend, backing, 0x11000, kPageSize,
                     {idw::MemorySubmissionKind::Noop, 0, 0}, &fence) ==
          -EINVAL);
    CHECK(ops.wait(&backend, 0, 1000) == -EINVAL);
    CHECK(ops.wait(&backend, 99, 1000) == -EINVAL);
    CHECK(ops.invalidate(&backend, 0x10000, kPageSize) == 0);
    CHECK(ops.invalidate(&backend, 0x10001, kPageSize) == -EINVAL);
    CHECK(backend.close_all() == -EBUSY);
    CHECK(ops.unbind(&backend, backing, 0x10000, kPageSize) == 0);
    CHECK(backend.close_all() == 0);
    CHECK(backend.allocation_count() == 0);
    return 0;
}

} // namespace

int main()
{
    if (test_direct_backing() || test_manager_lifecycle() ||
        test_distinct_backings() || test_rejections_and_cleanup()) {
        return 1;
    }
    std::printf("PASS: host backing allocates real pages and integrates with MemoryManager\n");
    return 0;
}
