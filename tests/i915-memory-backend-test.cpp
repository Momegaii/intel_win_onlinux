/* DRM ioctl behavior is simulated; this is not physical GPU validation. */
#include "../include/idw/i915_memory_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/ioctl.h>

namespace {

struct FakeDrm {
    uint32_t next_handle = 1;
    uint32_t next_context = 1;
    uint32_t exec_count = 0;
    uint32_t waits = 0;
    uint32_t fail_close_handle = 0;
    uint32_t fail_close_count = 0;
    int fail_wait_once = 0;
    int fail_context_destroy_once = 0;
    std::vector<uint32_t> handles;
    std::vector<uint32_t> contexts;
    std::vector<int64_t> wait_timeouts;
    bool move_resource_va = false;
};

FakeDrm *active = nullptr;

bool contains(const std::vector<uint32_t>& list, uint32_t value)
{
    for (uint32_t item : list) {
        if (item == value)
            return true;
    }
    return false;
}

int fake_ioctl(int, unsigned long request, void *argument)
{
    if (!active) {
        errno = EINVAL;
        return -1;
    }
    if (request == DRM_IOCTL_I915_GEM_CREATE) {
        auto *create = static_cast<drm_i915_gem_create *>(argument);
        create->handle = active->next_handle++;
        active->handles.push_back(create->handle);
        return 0;
    }
    if (request == DRM_IOCTL_GEM_CLOSE) {
        auto *close = static_cast<drm_gem_close *>(argument);
        if (close->handle == active->fail_close_handle &&
            active->fail_close_count) {
            --active->fail_close_count;
            errno = EBUSY;
            return -1;
        }
        for (auto it = active->handles.begin(); it != active->handles.end(); ++it) {
            if (*it == close->handle) {
                active->handles.erase(it);
                return 0;
            }
        }
        errno = ENOENT;
        return -1;
    }
    if (request == DRM_IOCTL_I915_GEM_CONTEXT_CREATE) {
        auto *create = static_cast<drm_i915_gem_context_create *>(argument);
        create->ctx_id = active->next_context++;
        active->contexts.push_back(create->ctx_id);
        return 0;
    }
    if (request == DRM_IOCTL_I915_GEM_CONTEXT_DESTROY) {
        auto *destroy =
            static_cast<drm_i915_gem_context_destroy *>(argument);
        if (active->fail_context_destroy_once) {
            errno = active->fail_context_destroy_once;
            active->fail_context_destroy_once = 0;
            return -1;
        }
        for (auto it = active->contexts.begin();
             it != active->contexts.end(); ++it) {
            if (*it == destroy->ctx_id) {
                active->contexts.erase(it);
                return 0;
            }
        }
        errno = ENOENT;
        return -1;
    }
    if (request == DRM_IOCTL_I915_GEM_PWRITE) {
        auto *write = static_cast<drm_i915_gem_pwrite *>(argument);
        if (!contains(active->handles, write->handle) ||
            write->size != 2 * sizeof(uint32_t)) {
            errno = EINVAL;
            return -1;
        }
        uint32_t words[2]{};
        std::memcpy(words, reinterpret_cast<const void *>(write->data_ptr),
                    sizeof(words));
        if (words[0] != 0x05000000u || words[1] != 0) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (request == DRM_IOCTL_I915_GEM_EXECBUFFER2) {
        auto *exec = static_cast<drm_i915_gem_execbuffer2 *>(argument);
        auto *objects = reinterpret_cast<drm_i915_gem_exec_object2 *>(
            exec->buffers_ptr);
        if (exec->buffer_count != 2 || exec->batch_len != 8 ||
            exec->flags != I915_EXEC_RENDER ||
            !contains(active->contexts, static_cast<uint32_t>(exec->rsvd1)) ||
            !contains(active->handles, objects[0].handle) ||
            !contains(active->handles, objects[1].handle) ||
            !(objects[0].flags & EXEC_OBJECT_PINNED)) {
            errno = EINVAL;
            return -1;
        }
        if (active->move_resource_va)
            objects[0].offset += 4096;
        ++active->exec_count;
        return 0;
    }
    if (request == DRM_IOCTL_I915_GEM_WAIT) {
        auto *wait = static_cast<drm_i915_gem_wait *>(argument);
        if (!contains(active->handles, wait->bo_handle)) {
            errno = ENOENT;
            return -1;
        }
        ++active->waits;
        active->wait_timeouts.push_back(wait->timeout_ns);
        if (active->fail_wait_once && wait->timeout_ns > 0) {
            errno = active->fail_wait_once;
            active->fail_wait_once = 0;
            return -1;
        }
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int test_integrated_i915_lifetime_and_recovery()
{
    FakeDrm fake;
    active = &fake;
    idw::I915AssistedMemoryBackend backend(9, fake_ioctl);
    idw::MemoryManager manager;
    const auto ops = backend.operations();
    CHECK(manager.initialize(ops, &backend, 0x10000000, 0x20000000) == 0);

    idw_handle resources[2]{};
    idw_gpuva addresses[2]{};
    uint64_t tickets[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(manager.create(4096, resources[i]) == 0);
        CHECK(manager.bind(resources[i], 0, addresses[i]) == 0);
        CHECK(addresses[i] != 0 && (addresses[i] & 4095) == 0);
    }
    CHECK(backend.context_count() == 2);
    CHECK(fake.exec_count == 2);

    for (unsigned i = 0; i < 2; ++i)
        CHECK(manager.submit(resources[i], tickets[i]) == 0);
    CHECK(backend.pending_fence_count() == 2);
    CHECK(fake.exec_count == 4);
    CHECK(manager.destroy(resources[0]) == -EBUSY);
    CHECK(manager.unbind(resources[0]) == -EBUSY);

    fake.fail_wait_once = ETIME;
    CHECK(manager.wait(tickets[0], 1000000) == -ETIME);
    idw::MemoryResourceInfo info{};
    CHECK(manager.query(resources[0], info) == 0);
    CHECK(info.submission_references == 1 && info.bound);
    CHECK(manager.recover_submission(tickets[0]) == 0);
    CHECK(backend.pending_fence_count() == 1);
    CHECK(fake.wait_timeouts.back() == -1);
    CHECK(manager.wait(tickets[1], 5000000000LL) == 0);
    CHECK(backend.pending_fence_count() == 0);

    fake.fail_context_destroy_once = EBUSY;
    CHECK(manager.unbind(resources[0]) == -EBUSY);
    CHECK(manager.query(resources[0], info) == 0 && info.recovery_required);
    CHECK(manager.recover_binding(resources[0]) == 0);
    CHECK(manager.unbind(resources[1]) == 0);
    CHECK(backend.context_count() == 0);
    fake.fail_close_handle = static_cast<uint32_t>(resources[0]);
    fake.fail_close_count = 1;
    CHECK(manager.destroy(resources[0]) == -EBUSY);
    CHECK(manager.resource_count() == 2);
    CHECK(manager.destroy(resources[0]) == 0);
    CHECK(manager.destroy(resources[1]) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    CHECK(backend.context_count() == 0);
    CHECK(backend.pending_fence_count() == 0);
    CHECK(backend.resource_count() == 0);
    CHECK(fake.handles.empty());
    CHECK(fake.contexts.empty());
    active = nullptr;
    return 0;
}

int test_partial_mapping_poison_and_cleanup()
{
    FakeDrm fake;
    active = &fake;
    idw::I915AssistedMemoryBackend backend(9, fake_ioctl);
    idw::MemoryManager manager;
    CHECK(manager.initialize(backend.operations(), &backend,
                             0x10000000, 0x20000000) == 0);
    idw_handle resource = 0;
    idw_gpuva va = 0;
    CHECK(manager.create(4096, resource) == 0);
    fake.move_resource_va = true;
    CHECK(manager.bind(resource, 0, va) == -EUCLEAN);
    idw::MemoryResourceInfo info{};
    CHECK(manager.query(resource, info) == 0 && info.recovery_required);
    uint64_t blocked_ticket = 0;
    CHECK(manager.submit(resource, blocked_ticket) == -EIO);
    fake.move_resource_va = false;
    CHECK(manager.recover_binding(resource) == 0);
    CHECK(manager.destroy(resource) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(backend.close_all() == 0);
    CHECK(fake.handles.empty() && fake.contexts.empty());
    active = nullptr;
    return 0;
}

} // namespace

int main()
{
    if (test_integrated_i915_lifetime_and_recovery() ||
        test_partial_mapping_poison_and_cleanup())
        return 1;
    std::puts("PASS: i915-assisted memory backend lifecycle and recovery tests");
    std::puts("NOTE: DRM ioctls are simulated; no physical GPU was used.");
    return 0;
}
