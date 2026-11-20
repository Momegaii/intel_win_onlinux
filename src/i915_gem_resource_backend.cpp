/* SPDX-License-Identifier: MIT */
#include "../include/idw/i915_gem_resource_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <limits>
#include <new>

#include <sys/ioctl.h>
#include <xf86drm.h>

namespace idw {
namespace {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kMaxObjectSize = 256ULL * 1024 * 1024;

int drm_ioctl(int fd, unsigned long request, void *argument)
{
    return drmIoctl(fd, request, argument);
}

} // namespace

I915GemResourceBackend::I915GemResourceBackend(int drm_fd,
                                               I915IoctlFunction ioctl_fn)
    : drm_fd_(drm_fd), ioctl_fn_(ioctl_fn ? ioctl_fn : drm_ioctl)
{
}

I915GemResourceBackend::~I915GemResourceBackend()
{
    if (close_all() != 0)
        std::fprintf(stderr, "idw: failed to close one or more tracked i915 GEM handles\n");
}

int I915GemResourceBackend::find_resource(uint32_t handle, size_t& index) const
{
    for (size_t i = 0; i < resources_.size(); ++i) {
        if (resources_[i].handle == handle) {
            index = i;
            return 0;
        }
    }
    return -ENOENT;
}

void I915GemResourceBackend::copy_snapshot(
    const Resource& resource, I915ResourceSnapshot& snapshot) const
{
    snapshot.handle = resource.handle;
    snapshot.size = resource.size;
    snapshot.backing = resource.backing;
    snapshot.discardable = resource.discardable;
    snapshot.activity_known = resource.activity_known;
    snapshot.busy = resource.busy;
}

int I915GemResourceBackend::create(uint64_t bytes, uint32_t& handle)
{
    if (drm_fd_ < 0 || !bytes || bytes > kMaxObjectSize ||
        bytes > std::numeric_limits<uint64_t>::max() - (kPageSize - 1))
        return -EINVAL;
    if (resources_.size() >= 256)
        return -ENOSPC;

    const uint64_t size = (bytes + kPageSize - 1) & ~(kPageSize - 1);
    drm_i915_gem_create request{};
    request.size = size;
    if (ioctl_fn_(drm_fd_, DRM_IOCTL_I915_GEM_CREATE, &request) != 0)
        return -errno;
    if (!request.handle) {
        return -EIO;
    }

    try {
        resources_.push_back({
            request.handle, size, I915BackingState::Unknown,
            false, false, false, {}
        });
    } catch (const std::bad_alloc&) {
        drm_gem_close close_request{};
        close_request.handle = request.handle;
        if (ioctl_fn_(drm_fd_, DRM_IOCTL_GEM_CLOSE, &close_request) != 0) {
            std::fprintf(stderr,
                         "idw: GEM handle %u could not be tracked or closed\n",
                         request.handle);
            return -EIO;
        }
        return -ENOMEM;
    }

    handle = request.handle;
    return 0;
}

int I915GemResourceBackend::destroy(uint32_t handle)
{
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;

    drm_gem_close request{};
    request.handle = handle;
    if (ioctl_fn_(drm_fd_, DRM_IOCTL_GEM_CLOSE, &request) != 0)
        return -errno;
    resources_.erase(resources_.begin() + static_cast<std::ptrdiff_t>(index));
    return 0;
}

int I915GemResourceBackend::close_all()
{
    int first_error = 0;
    size_t index = 0;
    while (index < resources_.size()) {
        const uint32_t handle = resources_[index].handle;
        drm_gem_close request{};
        request.handle = handle;
        if (ioctl_fn_(drm_fd_, DRM_IOCTL_GEM_CLOSE, &request) != 0) {
            if (!first_error)
                first_error = -errno;
            ++index;
        } else {
            resources_.erase(resources_.begin() +
                             static_cast<std::ptrdiff_t>(index));
        }
    }
    return first_error;
}

int I915GemResourceBackend::advise(uint32_t handle, uint32_t advice,
                                   I915ResourceSnapshot *snapshot_out)
{
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;

    drm_i915_gem_madvise request{};
    request.handle = handle;
    request.madv = advice;
    if (ioctl_fn_(drm_fd_, DRM_IOCTL_I915_GEM_MADVISE, &request) != 0)
        return -errno;

    Resource& resource = resources_[index];
    resource.discardable = advice == I915_MADV_DONTNEED;
    resource.backing = !request.retained ? I915BackingState::Purged :
        (resource.discardable ? I915BackingState::Discardable :
                                I915BackingState::Retained);
    if (snapshot_out)
        copy_snapshot(resource, *snapshot_out);
    return 0;
}

int I915GemResourceBackend::advise_willneed(
    uint32_t handle, I915ResourceSnapshot *snapshot_out)
{
    return advise(handle, I915_MADV_WILLNEED, snapshot_out);
}

int I915GemResourceBackend::advise_discardable(
    uint32_t handle, I915ResourceSnapshot *snapshot_out)
{
    return advise(handle, I915_MADV_DONTNEED, snapshot_out);
}

int I915GemResourceBackend::query_activity(
    uint32_t handle, I915ResourceSnapshot *snapshot_out)
{
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;

    drm_i915_gem_busy request{};
    request.handle = handle;
    if (ioctl_fn_(drm_fd_, DRM_IOCTL_I915_GEM_BUSY, &request) != 0)
        return -errno;

    Resource& resource = resources_[index];
    resource.activity_known = true;
    resource.busy = request.busy != 0;
    if (snapshot_out)
        copy_snapshot(resource, *snapshot_out);
    return 0;
}

int I915GemResourceBackend::record_exec_binding(
    uint32_t handle, uint32_t context_id, uint64_t requested_gpu_va,
    uint64_t returned_gpu_va)
{
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    if (!context_id || requested_gpu_va != returned_gpu_va ||
        (returned_gpu_va & (kPageSize - 1)))
        return -EINVAL;

    Resource& resource = resources_[index];
    for (I915GpuBinding& binding : resource.bindings) {
        if (binding.context_id == context_id) {
            binding.gpu_va = returned_gpu_va;
            return 0;
        }
    }
    try {
        resource.bindings.push_back({context_id, returned_gpu_va});
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }
    return 0;
}

void I915GemResourceBackend::release_context(uint32_t context_id)
{
    for (Resource& resource : resources_) {
        for (auto it = resource.bindings.begin();
             it != resource.bindings.end();) {
            if (it->context_id == context_id)
                it = resource.bindings.erase(it);
            else
                ++it;
        }
    }
}

int I915GemResourceBackend::snapshot(
    uint32_t handle, I915ResourceSnapshot& result) const
{
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    copy_snapshot(resources_[index], result);
    return 0;
}

int I915GemResourceBackend::binding(
    uint32_t handle, uint32_t context_id, I915GpuBinding& result) const
{
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    for (const I915GpuBinding& candidate : resources_[index].bindings) {
        if (candidate.context_id == context_id) {
            result = candidate;
            return 0;
        }
    }
    return -ENOENT;
}

size_t I915GemResourceBackend::resource_count() const
{
    return resources_.size();
}

} // namespace idw
