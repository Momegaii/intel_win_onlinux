/* SPDX-License-Identifier: MIT */
#ifndef IDW_I915_GEM_RESOURCE_BACKEND_HPP
#define IDW_I915_GEM_RESOURCE_BACKEND_HPP

#include <cstdint>
#include <vector>

#include <i915_drm.h>

namespace idw {

enum class I915BackingState {
    Unknown,
    Retained,
    Discardable,
    Purged
};

struct I915ResourceSnapshot {
    uint32_t handle;
    uint64_t size;
    I915BackingState backing;
    bool discardable;
    bool activity_known;
    bool busy;
};

struct I915GpuBinding {
    uint32_t context_id;
    uint64_t gpu_va;
};

using I915IoctlFunction = int (*)(int fd, unsigned long request, void *argument);

class I915GemResourceBackend {
public:
    explicit I915GemResourceBackend(int drm_fd,
                                    I915IoctlFunction ioctl_fn = nullptr);
    I915GemResourceBackend(const I915GemResourceBackend&) = delete;
    I915GemResourceBackend& operator=(const I915GemResourceBackend&) = delete;
    ~I915GemResourceBackend();

    int create(uint64_t bytes, uint32_t& handle);
    int destroy(uint32_t handle);
    int close_all();
    int advise_willneed(uint32_t handle, I915ResourceSnapshot *snapshot = nullptr);
    int advise_discardable(uint32_t handle,
                           I915ResourceSnapshot *snapshot = nullptr);
    int query_activity(uint32_t handle, I915ResourceSnapshot *snapshot = nullptr);
    /* Record only an offset returned by a successful i915 execbuffer. */
    int record_exec_binding(uint32_t handle, uint32_t context_id,
                            uint64_t requested_gpu_va,
                            uint64_t returned_gpu_va);
    void release_context(uint32_t context_id);
    int snapshot(uint32_t handle, I915ResourceSnapshot& result) const;
    int binding(uint32_t handle, uint32_t context_id,
                I915GpuBinding& result) const;
    size_t resource_count() const;

private:
    struct Resource {
        uint32_t handle;
        uint64_t size;
        I915BackingState backing;
        bool discardable;
        bool activity_known;
        bool busy;
        std::vector<I915GpuBinding> bindings;
    };

    int find_resource(uint32_t handle, size_t& index) const;
    int advise(uint32_t handle, uint32_t advice,
               I915ResourceSnapshot *snapshot);
    void copy_snapshot(const Resource& resource,
                       I915ResourceSnapshot& snapshot) const;

    int drm_fd_;
    I915IoctlFunction ioctl_fn_;
    std::vector<Resource> resources_;
};

} // namespace idw

#endif
