/* SPDX-License-Identifier: MIT */
#ifndef IDW_MEMORY_MANAGER_HPP
#define IDW_MEMORY_MANAGER_HPP

#include "gpuva_model.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace idw {

struct MemoryBackendOps {
    /*
     * Callbacks return zero or negative errno. Failed create/bind/submit
     * operations must not leave an unreported backing, mapping, or submission.
     * bind must report its actual VA; invalidate succeeds only after stale
     * translations can no longer be used. Successful submit returns a
     * nonzero fence. wait returns zero only on completion; quiesce returns
     * zero only when the fence can no longer reference resources. Fence zero
     * asks quiesce to establish backend-wide quiescence after a bad response.
     * Callbacks must not re-enter the manager.
     */
    int (*create_backing)(void *userdata, uint64_t bytes, uint64_t *backing);
    int (*destroy_backing)(void *userdata, uint64_t backing);
    int (*bind)(void *userdata, uint64_t backing, idw_gpuva requested_va,
                uint64_t bytes, idw_gpuva *actual_va);
    int (*unbind)(void *userdata, uint64_t backing, idw_gpuva va,
                  uint64_t bytes);
    int (*invalidate)(void *userdata, idw_gpuva va, uint64_t bytes);
    int (*submit)(void *userdata, uint64_t backing, idw_gpuva va,
                  uint64_t bytes, uint64_t *fence);
    int (*wait)(void *userdata, uint64_t fence, int64_t timeout_ns);
    /* Return success only when the backend guarantees the fence is quiescent. */
    int (*quiesce)(void *userdata, uint64_t fence);
};

struct MemoryResourceInfo {
    idw_handle handle;
    uint64_t bytes;
    idw_gpuva gpu_va;
    uint32_t submission_references;
    bool bound;
    bool recovery_required;
};

/* Callers serialize all methods and must successfully shut down before teardown. */
class MemoryManager {
public:
    MemoryManager();
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;
    ~MemoryManager();

    int initialize(const MemoryBackendOps& ops, void *userdata,
                   idw_gpuva min_va, idw_gpuva max_va);
    /* Fails while allocations or submissions remain, without dropping them. */
    int shutdown();
    int create(uint64_t bytes, idw_handle& handle);
    int bind(idw_handle handle, idw_gpuva requested_va, idw_gpuva& actual_va);
    int unbind(idw_handle handle);
    /* Retries the unbind/invalidation phase left incomplete by a failure. */
    int recover_binding(idw_handle handle);
    /* A zero-fence -EPROTO still sets ticket; it must be recovered. */
    int submit(idw_handle handle, uint64_t& ticket);
    /* A failed wait retains the reference until wait succeeds or quiesce does. */
    int wait(uint64_t ticket, int64_t timeout_ns);
    /* Also recovers a successful submit callback that returned an invalid fence. */
    int recover_submission(uint64_t ticket);
    int destroy(idw_handle handle);
    int query(idw_handle handle, MemoryResourceInfo& info) const;
    size_t resource_count() const;
    size_t submission_count() const;

private:
    enum class BindingState {
        Unbound,
        Bound,
        NeedsUnbind,
        NeedsInvalidation
    };

    struct Resource {
        idw_handle handle;
        uint64_t bytes;
        uint64_t backing;
        idw_gpuva reserved_va;
        idw_gpuva backend_va;
        uint32_t references;
        BindingState binding_state;
        bool mismatched_va;
    };

    struct Submission {
        uint64_t ticket;
        uint64_t fence;
        idw_handle resource;
    };

    int find_resource(idw_handle handle, size_t& index) const;
    int find_submission(uint64_t ticket, size_t& index) const;
    int release_submission(size_t index);
    int finish_unbind(Resource& resource);

    MemoryBackendOps ops_;
    void *userdata_;
    struct idw_gpuva_space va_space_;
    std::vector<Resource> resources_;
    std::vector<Submission> submissions_;
    idw_handle next_handle_;
    uint64_t next_ticket_;
    bool mapping_mismatch_;
    bool initialized_;
};

} // namespace idw

#endif
