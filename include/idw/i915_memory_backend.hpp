/* SPDX-License-Identifier: MIT */
#ifndef IDW_I915_MEMORY_BACKEND_HPP
#define IDW_I915_MEMORY_BACKEND_HPP

#include "i915_gem_resource_backend.hpp"
#include "memory_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace idw {

class I915AssistedMemoryBackend {
public:
    explicit I915AssistedMemoryBackend(int drm_fd,
                                       I915IoctlFunction ioctl_fn = nullptr);
    I915AssistedMemoryBackend(const I915AssistedMemoryBackend&) = delete;
    I915AssistedMemoryBackend& operator=(const I915AssistedMemoryBackend&) = delete;
    ~I915AssistedMemoryBackend();

    MemoryBackendOps operations() const;
    /* Explicit cleanup boundary; requires manager shutdown first. */
    int close_all();
    size_t context_count() const;
    size_t pending_fence_count() const;
    size_t resource_count() const;

private:
    struct Context {
        uint32_t backing_handle;
        uint32_t context_id;
        idw_gpuva gpu_va;
        uint64_t bytes;
        uint32_t pending_batch;
        bool pending_batch_submitted;
        bool poisoned;
    };

    struct Fence {
        uint64_t token;
        uint32_t batch_handle;
    };

    static int create_backing(void *userdata, uint64_t bytes,
                              uint64_t *backing);
    static int destroy_backing(void *userdata, uint64_t backing);
    static int bind(void *userdata, uint64_t backing,
                    idw_gpuva requested_va, uint64_t bytes,
                    idw_gpuva *actual_va);
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

    int find_context(uint32_t backing_handle, size_t& index) const;
    int find_fence(uint64_t token, size_t& index) const;
    int create_context(uint32_t *context_id);
    int destroy_context(uint32_t context_id);
    int create_batch(uint32_t& handle);
    int upload_batch(uint32_t handle, const void *bytes, uint32_t length);
    int exec_resource_batch(uint32_t context_id, uint32_t resource_handle,
                            idw_gpuva resource_va, uint32_t batch_handle,
                            uint32_t batch_length,
                            idw_gpuva *actual_resource_va);
    int wait_batch(uint32_t handle, int64_t timeout_ns);
    int close_batch(uint32_t handle);
    int cleanup_bind_context(size_t index);
    int finish_fence(size_t index, int64_t timeout_ns);

    int drm_fd_;
    I915IoctlFunction ioctl_fn_;
    I915GemResourceBackend resources_;
    std::vector<Context> contexts_;
    std::vector<Fence> fences_;
    uint64_t next_fence_;
};

} // namespace idw

#endif
