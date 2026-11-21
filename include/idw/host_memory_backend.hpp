/* SPDX-License-Identifier: MIT */
#ifndef IDW_HOST_MEMORY_BACKEND_HPP
#define IDW_HOST_MEMORY_BACKEND_HPP

#include "memory_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace idw {

/*
 * IDW-owned host-RAM backing provider for MemoryManager.
 *
 * create_backing() creates a real memfd object, sizes it, maps it, and faults
 * every page with zeroes. The pages are real host memory, but they are not GEM
 * objects, not DMA-mapped, not GPU-visible, and not published in any page
 * table. bind() records the GPU VA reserved by MemoryManager; it does not
 * program hardware. submit() performs the requested dword write synchronously
 * on the host mapping and returns an already-complete fence.
 *
 * This is the first real backing provider in the native path. It deliberately
 * does not pretend to be a physical Haswell backend.
 *
 * Callers serialize all methods, matching MemoryManager.
 */
class HostMemoryBackend {
public:
    HostMemoryBackend();
    HostMemoryBackend(const HostMemoryBackend&) = delete;
    HostMemoryBackend& operator=(const HostMemoryBackend&) = delete;
    ~HostMemoryBackend();

    MemoryBackendOps operations() const;

    /* Releases every unbound allocation. Fails while any allocation is bound. */
    int close_all();

    size_t allocation_count() const;
    size_t mapped_count() const;
    uint64_t bytes_allocated() const;

    /* Diagnostic/test accessor. The returned address is a host mapping. */
    int backing_info(uint64_t backing, void **address, uint64_t *bytes) const;
    int backing_at(size_t index, uint64_t *backing, void **address,
                   uint64_t *bytes) const;

private:
    struct Allocation {
        uint64_t id;
        int fd;
        void *address;
        uint64_t bytes;
        idw_gpuva va;
        bool bound;
    };

    static int create_backing(void *userdata, uint64_t bytes,
                              uint64_t *backing);
    static int destroy_backing(void *userdata, uint64_t backing);
    static int bind(void *userdata, uint64_t backing, idw_gpuva requested_va,
                    uint64_t bytes, idw_gpuva *actual_va);
    static int unbind(void *userdata, uint64_t backing, idw_gpuva va,
                      uint64_t bytes);
    static int invalidate(void *userdata, idw_gpuva va, uint64_t bytes);
    static int submit(void *userdata, uint64_t backing, idw_gpuva va,
                      uint64_t bytes,
                      const MemorySubmissionCommand& command,
                      uint64_t *fence);
    static int wait(void *userdata, uint64_t fence, int64_t timeout_ns);
    static int quiesce(void *userdata, uint64_t fence);
    static int cpu_read_dword(void *userdata, uint64_t backing,
                              uint64_t offset, uint32_t *value);
    static int cpu_write_dword(void *userdata, uint64_t backing,
                               uint64_t offset, uint32_t value);

    int find_allocation(uint64_t backing, size_t& index) const;
    int release_allocation(size_t index);

    std::vector<Allocation> allocations_;
    uint64_t next_backing_;
    uint64_t next_fence_;
};

} // namespace idw

#endif
