/* SPDX-License-Identifier: MIT */
#include "../include/idw/memory_manager.hpp"
#include "../include/idw/native_dma_backend.hpp"
#include "../include/uapi/idw_dma_test.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using Backend = idw::NativeDmaMemoryBackend;
using State = idw::NativeDmaMemoryBackend::ResourceState;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__,   \
                         #condition);                                        \
            return 1;                                                        \
        }                                                                    \
    } while (0)

bool counters_zero(const Backend& backend)
{
    const Backend::OwnershipCounters counters = backend.counters();
    const bool zero = counters.allocated_pages == 0 &&
                      counters.mapped_pages == 0 &&
                      counters.populated_ptes == 0 &&
                      counters.pt_pages == 0 &&
                      counters.va_reservations == 0 &&
                      counters.quarantined_pages == 0;
    if (!zero)
        std::fprintf(stderr,
                     "counters: alloc=%llu mapped=%llu ptes=%llu pt=%llu va=%llu quarantine=%llu\n",
                     (unsigned long long)counters.allocated_pages,
                     (unsigned long long)counters.mapped_pages,
                     (unsigned long long)counters.populated_ptes,
                     (unsigned long long)counters.pt_pages,
                     (unsigned long long)counters.va_reservations,
                     (unsigned long long)counters.quarantined_pages);
    return zero;
}

int initialize(Backend& backend, idw::MemoryManager& manager)
{
    return manager.initialize(backend.operations(), &backend, 0x10000,
                              0x100000);
}

int test_create_failure(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    Backend::FaultPlan plan;
    plan.create_fail_at = 0;
    backend.set_fault_plan(plan);
    idw_handle handle = 0;
    CHECK(manager.create(4096, handle) == -ENOMEM);
    CHECK(backend.allocation_count() == 0);
    CHECK(counters_zero(backend));
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    return 0;
}

int test_partial_create_failure(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    Backend::FaultPlan plan;
    plan.create_fail_at = 1;
    backend.set_fault_plan(plan);
    idw_handle first = 0;
    idw_handle second = 0;
    CHECK(manager.create(4096, first) == 0);
    CHECK(manager.create(4096, second) == -ENOMEM);
    CHECK(manager.destroy(first) == 0);
    CHECK(backend.allocation_count() == 0);
    CHECK(counters_zero(backend));
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    return 0;
}

int test_page_table_failure(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    idw_handle handle = 0;
    CHECK(manager.create(3 * 4096, handle) == 0);
    uint64_t backing = 0;
    uint64_t bytes = 0;
    uint64_t cookie = 0;
    CHECK(backend.allocation_at(0, backing, bytes, cookie) == 0);
    Backend::FaultPlan plan;
    plan.page_table_fail_at = 0;
    backend.set_fault_plan(plan);
    idw_gpuva va = 0;
    CHECK(manager.bind(handle, 0x20000, va) == -ENOMEM);
    State state;
    CHECK(backend.allocation_state(backing, state) == 0);
    CHECK(state == State::BackingAllocated);
    CHECK(backend.counters().populated_ptes == 0);
    CHECK(backend.counters().va_reservations == 0);
    backend.set_fault_plan(Backend::FaultPlan{});
    CHECK(manager.bind(handle, 0x20000, va) == 0);
    CHECK(manager.unbind(handle) == 0);
    CHECK(manager.destroy(handle) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    CHECK(counters_zero(backend));
    return 0;
}

int test_mmap_failure(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    idw_handle handle = 0;
    CHECK(manager.create(3 * 4096, handle) == 0);
    Backend::FaultPlan plan;
    plan.mmap_fail = true;
    backend.set_fault_plan(plan);
    idw_gpuva va = 0;
    CHECK(manager.bind(handle, 0x20000, va) == -ENOMEM);
    CHECK(backend.page_table_count() == 0);
    backend.set_fault_plan(Backend::FaultPlan{});
    CHECK(manager.bind(handle, 0x20000, va) == 0);
    CHECK(manager.unbind(handle) == 0);
    CHECK(manager.destroy(handle) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    CHECK(counters_zero(backend));
    return 0;
}

int test_sync_failure(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    idw_handle handle = 0;
    CHECK(manager.create(3 * 4096, handle) == 0);
    Backend::FaultPlan plan;
    plan.sync_page_table_fail = true;
    backend.set_fault_plan(plan);
    idw_gpuva va = 0;
    CHECK(manager.bind(handle, 0x20000, va) == -EIO);
    backend.set_fault_plan(Backend::FaultPlan{});
    CHECK(manager.bind(handle, 0x20000, va) == 0);
    CHECK(manager.unbind(handle) == 0);
    CHECK(manager.destroy(handle) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    CHECK(counters_zero(backend));
    return 0;
}

int test_destroy_retry(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    idw_handle handle = 0;
    CHECK(manager.create(3 * 4096, handle) == 0);
    uint64_t backing = 0;
    uint64_t bytes = 0;
    uint64_t cookie = 0;
    CHECK(backend.allocation_at(0, backing, bytes, cookie) == 0);
    Backend::FaultPlan plan;
    plan.destroy_fail = true;
    backend.set_fault_plan(plan);
    CHECK(manager.destroy(handle) == -EIO);
    State state;
    CHECK(backend.allocation_state(backing, state) == 0);
    CHECK(state == State::Quarantined);
    CHECK(backend.counters().allocated_pages == 3);
    CHECK(backend.counters().quarantined_pages == 3);
    backend.set_fault_plan(Backend::FaultPlan{});
    CHECK(manager.destroy(handle) == 0);
    CHECK(backend.allocation_count() == 0);
    CHECK(counters_zero(backend));
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    return 0;
}

int test_unbind_retry(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    idw_handle handle = 0;
    CHECK(manager.create(3 * 4096, handle) == 0);
    idw_gpuva va = 0;
    CHECK(manager.bind(handle, 0x20000, va) == 0);
    uint64_t backing = 0;
    uint64_t bytes = 0;
    uint64_t cookie = 0;
    CHECK(backend.allocation_at(0, backing, bytes, cookie) == 0);
    Backend::FaultPlan plan;
    plan.unbind_fail_invalidation = true;
    backend.set_fault_plan(plan);
    CHECK(manager.unbind(handle) == -EIO);
    State state;
    CHECK(backend.allocation_state(backing, state) == 0);
    CHECK(state == State::Quarantined);
    CHECK(backend.counters().populated_ptes == 0);
    CHECK(backend.counters().va_reservations == 1);
    CHECK(backend.counters().quarantined_pages == 3);
    backend.set_fault_plan(Backend::FaultPlan{});
    CHECK(manager.recover_binding(handle) == 0);
    CHECK(backend.allocation_state(backing, state) == 0);
    CHECK(state == State::BackingAllocated);
    CHECK(backend.counters().quarantined_pages == 0);
    CHECK(manager.destroy(handle) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    CHECK(counters_zero(backend));
    return 0;
}

int test_bind_failure_after_map(void)
{
    Backend backend;
    idw::MemoryManager manager;
    CHECK(initialize(backend, manager) == 0);
    idw_handle handle = 0;
    CHECK(manager.create(3 * 4096, handle) == 0);
    uint64_t backing = 0;
    uint64_t bytes = 0;
    uint64_t cookie = 0;
    CHECK(backend.allocation_at(0, backing, bytes, cookie) == 0);
    Backend::FaultPlan plan;
    plan.bind_fail_after_map = true;
    backend.set_fault_plan(plan);
    idw_gpuva va = 0;
    CHECK(manager.bind(handle, 0x20000, va) == -EIO);
    State state;
    CHECK(backend.allocation_state(backing, state) == 0);
    CHECK(state == State::Quarantined);
    CHECK(backend.counters().populated_ptes == 3);
    CHECK(backend.counters().va_reservations == 1);
    CHECK(backend.counters().quarantined_pages == 3);
    CHECK(manager.destroy(handle) == -EBUSY);
    backend.set_fault_plan(Backend::FaultPlan{});
    idw::MemoryBackendOps ops = backend.operations();
    CHECK(ops.unbind(&backend, backing, 0x20000, bytes) == 0);
    CHECK(manager.destroy(handle) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    CHECK(counters_zero(backend));
    return 0;
}

int test_vma_lifetime(void)
{
    const int fd = ::open(IDW_DMA_TEST_DEVICE, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    const int control = ::dup(fd);
    CHECK(control >= 0);

    struct idw_dma_test_alloc request = {};
    request.size = sizeof(request);
    request.direction = IDW_DMA_TEST_BIDIRECTIONAL;
    request.bytes = 4096;
    CHECK(::ioctl(fd, IDW_DMA_TEST_IOCTL_ALLOC, &request) == 0);
    CHECK(request.cookie != 0);
    CHECK(request.page_count == 1);

    void *address = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, static_cast<off_t>(request.cookie * 4096));
    CHECK(address != MAP_FAILED);
    static_cast<unsigned char *>(address)[0] = 0x5a;

    // A VMA holds a file reference, so closing one fd must not release the
    // device or invalidate the mapping while the VMA is live.
    CHECK(::close(fd) == 0);
    CHECK(static_cast<unsigned char *>(address)[0] == 0x5a);

    struct idw_dma_test_stats stats = {};
    stats.size = sizeof(stats);
    CHECK(::ioctl(control, IDW_DMA_TEST_IOCTL_STATS, &stats) == 0);
    CHECK(stats.vma_refs == 1);

    struct idw_dma_test_free release = {};
    release.size = sizeof(release);
    release.cookie = request.cookie;
    CHECK(::ioctl(control, IDW_DMA_TEST_IOCTL_FREE, &release) == -1);
    CHECK(errno == EBUSY);

    CHECK(::munmap(address, 4096) == 0);
    stats = {};
    stats.size = sizeof(stats);
    CHECK(::ioctl(control, IDW_DMA_TEST_IOCTL_STATS, &stats) == 0);
    CHECK(stats.vma_refs == 0);
    CHECK(::ioctl(control, IDW_DMA_TEST_IOCTL_FREE, &release) == 0);
    CHECK(::close(control) == 0);
    return 0;
}

} // namespace

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

    CHECK(test_create_failure() == 0);
    CHECK(test_partial_create_failure() == 0);
    CHECK(test_page_table_failure() == 0);
    CHECK(test_mmap_failure() == 0);
    CHECK(test_sync_failure() == 0);
    CHECK(test_destroy_retry() == 0);
    CHECK(test_unbind_retry() == 0);
    CHECK(test_bind_failure_after_map() == 0);
    CHECK(test_vma_lifetime() == 0);

    std::printf("PASS: native DMA backend fault injection, retry, and ownership counters\n");
    return 0;
}
