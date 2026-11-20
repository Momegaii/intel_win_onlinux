/* DRM ioctl behavior is simulated here; this is not GPU/DMA validation. */
#include "../include/idw/i915_gem_resource_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sys/ioctl.h>

namespace {

uint32_t next_handle = 1;
uint32_t close_fail_handle = 0;
uint32_t close_failure_count = 0;
bool purge_on_dontneed = false;
bool fake_backing_purged = false;
bool fake_busy = false;

int fake_ioctl(int, unsigned long request, void *argument)
{
    if (request == DRM_IOCTL_I915_GEM_CREATE) {
        auto *create = static_cast<drm_i915_gem_create *>(argument);
        create->handle = next_handle++;
        return 0;
    }
    if (request == DRM_IOCTL_GEM_CLOSE) {
        auto *close_request = static_cast<drm_gem_close *>(argument);
        if (close_request->handle == close_fail_handle &&
            close_failure_count) {
            --close_failure_count;
            errno = EBUSY;
            return -1;
        }
        return 0;
    }
    if (request == DRM_IOCTL_I915_GEM_MADVISE) {
        auto *advice = static_cast<drm_i915_gem_madvise *>(argument);
        if (advice->madv == I915_MADV_DONTNEED && purge_on_dontneed)
            fake_backing_purged = true;
        advice->retained = !fake_backing_purged;
        return 0;
    }
    if (request == DRM_IOCTL_I915_GEM_BUSY) {
        auto *busy = static_cast<drm_i915_gem_busy *>(argument);
        busy->busy = fake_busy ? 1 : 0;
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

int test_resource_state_and_binding()
{
    idw::I915GemResourceBackend backend(9, fake_ioctl);
    uint32_t handle = 0;
    idw::I915ResourceSnapshot state{};
    idw::I915GpuBinding binding{};

    CHECK(backend.create(4097, handle) == 0);
    CHECK(handle != 0);
    CHECK(backend.snapshot(handle, state) == 0);
    CHECK(state.size == 8192);
    CHECK(state.backing == idw::I915BackingState::Unknown);
    CHECK(backend.advise_willneed(handle, &state) == 0);
    CHECK(state.backing == idw::I915BackingState::Retained);
    CHECK(!state.discardable);

    CHECK(backend.advise_discardable(handle, &state) == 0);
    CHECK(state.backing == idw::I915BackingState::Discardable);
    CHECK(state.discardable);

    fake_busy = true;
    CHECK(backend.query_activity(handle, &state) == 0);
    CHECK(state.activity_known && state.busy);
    fake_busy = false;
    CHECK(backend.query_activity(handle, &state) == 0);
    CHECK(state.activity_known && !state.busy);

    CHECK(backend.record_exec_binding(handle, 42, 0x100000, 0x100000) == 0);
    CHECK(backend.binding(handle, 42, binding) == 0);
    CHECK(binding.gpu_va == 0x100000);
    CHECK(backend.record_exec_binding(handle, 42, 0x100000, 0x200000) ==
          -EINVAL);
    CHECK(backend.record_exec_binding(handle, 0, 0x100000, 0x100000) ==
          -EINVAL);
    CHECK(backend.record_exec_binding(handle, 42, 0x100001, 0x100001) ==
          -EINVAL);
    backend.release_context(42);
    CHECK(backend.binding(handle, 42, binding) == -ENOENT);
    CHECK(backend.binding(handle, 41, binding) == -ENOENT);

    purge_on_dontneed = true;
    CHECK(backend.advise_discardable(handle, &state) == 0);
    CHECK(state.backing == idw::I915BackingState::Purged);
    CHECK(state.discardable);
    CHECK(backend.advise_willneed(handle, &state) == 0);
    CHECK(state.backing == idw::I915BackingState::Purged);
    CHECK(!state.discardable);
    purge_on_dontneed = false;

    CHECK(backend.destroy(handle) == 0);
    CHECK(backend.snapshot(handle, state) == -ENOENT);
    CHECK(backend.destroy(handle) == -ENOENT);
    fake_backing_purged = false;
    return 0;
}

int test_failure_tracking_and_cleanup()
{
    idw::I915GemResourceBackend backend(9, fake_ioctl);
    idw::I915ResourceSnapshot state{};
    uint32_t first = 0;
    uint32_t second = 0;

    CHECK(backend.create(4096, first) == 0);
    CHECK(backend.create(4096, second) == 0);
    close_fail_handle = first;
    close_failure_count = 1;
    CHECK(backend.destroy(first) == -EBUSY);
    CHECK(backend.resource_count() == 2);
    CHECK(backend.snapshot(first, state) == 0);
    close_failure_count = 1;
    CHECK(backend.close_all() == -EBUSY);
    CHECK(backend.resource_count() == 1);
    close_fail_handle = 0;
    CHECK(backend.close_all() == 0);
    CHECK(backend.resource_count() == 0);
    return 0;
}

} // namespace

int main()
{
    if (test_resource_state_and_binding() ||
        test_failure_tracking_and_cleanup())
        return 1;
    std::puts("PASS: i915 GEM resource backend state, binding, and cleanup tests");
    std::puts("NOTE: ioctl callbacks are simulated; no physical GPU or DMA was tested.");
    return 0;
}
