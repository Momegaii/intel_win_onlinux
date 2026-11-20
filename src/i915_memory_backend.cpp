/* SPDX-License-Identifier: MIT */
#include "../include/idw/i915_memory_backend.hpp"

#include "../include/idw/gpu_submission.hpp"

#include <cerrno>
#include <cstdio>
#include <limits>
#include <new>

#include <sys/ioctl.h>
#include <xf86drm.h>

namespace idw {
namespace {

constexpr uint64_t kPageSize = 4096;
constexpr int64_t kInfiniteWait = -1;

int drm_ioctl(int fd, unsigned long request, void *argument)
{
    return drmIoctl(fd, request, argument);
}

int ioctl_result(I915IoctlFunction ioctl_fn, int fd,
                 unsigned long request, void *argument)
{
    return ioctl_fn(fd, request, argument) == 0 ? 0 : -errno;
}

} // namespace

I915AssistedMemoryBackend::I915AssistedMemoryBackend(
    int drm_fd, I915IoctlFunction ioctl_fn)
    : drm_fd_(drm_fd), ioctl_fn_(ioctl_fn ? ioctl_fn : drm_ioctl),
      resources_(drm_fd, ioctl_fn_), contexts_(), fences_(), next_fence_(1)
{
}

I915AssistedMemoryBackend::~I915AssistedMemoryBackend()
{
    if (close_all() != 0)
        std::fprintf(stderr,
                     "idw: i915-assisted memory backend cleanup was incomplete\n");
}

MemoryBackendOps I915AssistedMemoryBackend::operations() const
{
    return {create_backing, destroy_backing, bind, unbind, invalidate,
            submit, wait, quiesce, cpu_read_dword, cpu_write_dword};
}

int I915AssistedMemoryBackend::create_backing(
    void *userdata, uint64_t bytes, uint64_t *backing)
{
    if (!userdata || !backing)
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    uint32_t handle = 0;
    const int ret = self.resources_.create(bytes, handle);
    if (ret)
        return ret;
    *backing = handle;
    return 0;
}

int I915AssistedMemoryBackend::destroy_backing(void *userdata,
                                               uint64_t backing)
{
    if (!userdata || !backing || backing > UINT32_MAX)
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    size_t context_index = 0;
    if (self.find_context(static_cast<uint32_t>(backing), context_index) == 0)
        return -EBUSY;
    return self.resources_.destroy(static_cast<uint32_t>(backing));
}

int I915AssistedMemoryBackend::create_context(uint32_t *context_id)
{
    if (!context_id || drm_fd_ < 0)
        return -EINVAL;
    drm_i915_gem_context_create request{};
    const int ret = ioctl_result(ioctl_fn_, drm_fd_,
                                 DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &request);
    if (ret)
        return ret;
    if (!request.ctx_id)
        return -EIO;
    *context_id = request.ctx_id;
    return 0;
}

int I915AssistedMemoryBackend::destroy_context(uint32_t context_id)
{
    if (!context_id)
        return -EINVAL;
    drm_i915_gem_context_destroy request{};
    request.ctx_id = context_id;
    return ioctl_result(ioctl_fn_, drm_fd_,
                        DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &request);
}

int I915AssistedMemoryBackend::create_batch(uint32_t& handle)
{
    return resources_.create(kPageSize, handle);
}

int I915AssistedMemoryBackend::upload_batch(uint32_t handle,
                                            const void *bytes,
                                            uint32_t length)
{
    if (!bytes || !length || (length & 7))
        return -EINVAL;
    drm_i915_gem_pwrite request{};
    request.handle = handle;
    request.offset = 0;
    request.size = length;
    request.data_ptr = reinterpret_cast<uint64_t>(bytes);
    return ioctl_result(ioctl_fn_, drm_fd_, DRM_IOCTL_I915_GEM_PWRITE,
                        &request);
}

int I915AssistedMemoryBackend::exec_resource_batch(
    uint32_t context_id, uint32_t resource_handle, idw_gpuva resource_va,
    uint32_t batch_handle, uint32_t batch_length,
    idw_gpuva *actual_resource_va)
{
    if (!context_id || !resource_handle || !batch_handle ||
        !actual_resource_va || (resource_va & (kPageSize - 1)) ||
        !batch_length || (batch_length & 7) || batch_length > kPageSize)
        return -EINVAL;

    drm_i915_gem_exec_object2 objects[2]{};
    objects[0].handle = resource_handle;
    objects[0].offset = resource_va;
    objects[0].flags = EXEC_OBJECT_PINNED;
    objects[1].handle = batch_handle;

    drm_i915_gem_execbuffer2 exec{};
    exec.buffers_ptr = reinterpret_cast<uint64_t>(objects);
    exec.buffer_count = 2;
    exec.batch_start_offset = 0;
    exec.batch_len = batch_length;
    exec.flags = I915_EXEC_RENDER;
    exec.rsvd1 = context_id;
    int ret = ioctl_result(ioctl_fn_, drm_fd_,
                           DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec);
    if (ret)
        return ret;
    *actual_resource_va = objects[0].offset;
    return objects[0].offset == resource_va ? 0 : -ERANGE;
}

int I915AssistedMemoryBackend::wait_batch(uint32_t handle,
                                          int64_t timeout_ns)
{
    drm_i915_gem_wait request{};
    request.bo_handle = handle;
    request.timeout_ns = timeout_ns;
    return ioctl_result(ioctl_fn_, drm_fd_, DRM_IOCTL_I915_GEM_WAIT, &request);
}

int I915AssistedMemoryBackend::close_batch(uint32_t handle)
{
    return resources_.destroy(handle);
}

int I915AssistedMemoryBackend::find_context(uint32_t backing_handle,
                                            size_t& index) const
{
    for (size_t i = 0; i < contexts_.size(); ++i) {
        if (contexts_[i].backing_handle == backing_handle) {
            index = i;
            return 0;
        }
    }
    return -ENOENT;
}

int I915AssistedMemoryBackend::find_fence(uint64_t token,
                                          size_t& index) const
{
    for (size_t i = 0; i < fences_.size(); ++i) {
        if (fences_[i].token == token) {
            index = i;
            return 0;
        }
    }
    return -ENOENT;
}

int I915AssistedMemoryBackend::bind(void *userdata, uint64_t backing,
                                    idw_gpuva requested_va, uint64_t bytes,
                                    idw_gpuva *actual_va)
{
    if (!userdata || !backing || backing > UINT32_MAX || !actual_va ||
        !bytes || (requested_va & (kPageSize - 1)))
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    const uint32_t backing_handle = static_cast<uint32_t>(backing);
    size_t ignored = 0;
    if (self.find_context(backing_handle, ignored) == 0)
        return -EBUSY;
    try {
        self.contexts_.reserve(self.contexts_.size() + 1);
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }

    uint32_t context_id = 0;
    int ret = self.create_context(&context_id);
    if (ret)
        return ret;
    self.contexts_.push_back(
        {backing_handle, context_id, requested_va, bytes, 0, false, false});
    const size_t context_index = self.contexts_.size() - 1;
    auto fail_and_rollback = [&](int failure) {
        const int cleanup = self.cleanup_bind_context(context_index);
        if (cleanup) {
            *actual_va = requested_va;
            return -EUCLEAN;
        }
        return failure;
    };

    uint32_t batch_handle = 0;
    ret = self.create_batch(batch_handle);
    if (ret)
        return fail_and_rollback(ret);
    self.contexts_[context_index].pending_batch = batch_handle;
    const uint32_t noop_words[2] = {
        kHaswellMiBatchBufferEnd, kHaswellMiNoop
    };
    ret = self.upload_batch(batch_handle, noop_words, sizeof(noop_words));
    if (ret)
        return fail_and_rollback(ret);

    idw_gpuva bound_va = requested_va;
    ret = self.exec_resource_batch(context_id, backing_handle, requested_va,
                                   batch_handle,
                                   static_cast<uint32_t>(
                                       kHaswellNoopBatchLength),
                                   &bound_va);
    if (ret == -ERANGE) {
        self.contexts_[context_index].pending_batch_submitted = true;
        *actual_va = bound_va;
        return -EUCLEAN;
    }
    if (ret)
        return fail_and_rollback(ret);
    self.contexts_[context_index].pending_batch_submitted = true;
    ret = self.wait_batch(batch_handle, kDefaultGpuWaitTimeoutNs);
    if (ret) {
        *actual_va = bound_va;
        return -EUCLEAN;
    }
    ret = self.resources_.record_exec_binding(backing_handle, context_id,
                                               requested_va, bound_va);
    if (ret)
        return fail_and_rollback(ret);
    ret = self.close_batch(batch_handle);
    if (ret) {
        *actual_va = bound_va;
        return -EUCLEAN;
    }
    self.contexts_[context_index].pending_batch = 0;
    self.contexts_[context_index].pending_batch_submitted = false;
    *actual_va = bound_va;
    return 0;
}

int I915AssistedMemoryBackend::cleanup_bind_context(size_t index)
{
    if (index >= contexts_.size())
        return -EINVAL;
    Context& context = contexts_[index];
    if (context.pending_batch) {
        if (context.pending_batch_submitted) {
            const int ret = wait_batch(context.pending_batch, kInfiniteWait);
            if (ret)
                return ret;
        }
        const int ret = close_batch(context.pending_batch);
        if (ret)
            return ret;
        context.pending_batch = 0;
        context.pending_batch_submitted = false;
    }
    const uint32_t context_id = context.context_id;
    const int ret = destroy_context(context_id);
    if (ret)
        return ret;
    resources_.release_context(context_id);
    contexts_.erase(contexts_.begin() +
                    static_cast<std::ptrdiff_t>(index));
    return 0;
}

int I915AssistedMemoryBackend::unbind(void *userdata, uint64_t backing,
                                      idw_gpuva, uint64_t)
{
    if (!userdata || !backing || backing > UINT32_MAX)
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    size_t index = 0;
    int ret = self.find_context(static_cast<uint32_t>(backing), index);
    if (ret)
        return ret;
    return self.cleanup_bind_context(index);
}

int I915AssistedMemoryBackend::invalidate(void *, idw_gpuva, uint64_t)
{
    /* Destroying the per-resource i915 context releases its i915-managed VM. */
    return 0;
}

int I915AssistedMemoryBackend::submit(void *userdata, uint64_t backing,
                                      idw_gpuva va, uint64_t bytes,
                                      const MemorySubmissionCommand& command,
                                      uint64_t *fence)
{
    if (!userdata || !backing || backing > UINT32_MAX || !fence ||
        bytes < sizeof(uint32_t))
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    size_t context_index = 0;
    int ret = self.find_context(static_cast<uint32_t>(backing),
                                context_index);
    if (ret)
        return ret;
    const Context& context = self.contexts_[context_index];
    if (context.gpu_va != va || context.bytes != bytes || context.poisoned)
        return -EINVAL;
    if (!self.next_fence_ || self.fences_.size() == self.fences_.max_size())
        return -ENOSPC;
    try {
        self.fences_.reserve(self.fences_.size() + 1);
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }

    uint32_t batch_handle = 0;
    ret = self.create_batch(batch_handle);
    if (ret)
        return ret;
    uint32_t batch_words[6]{};
    uint32_t batch_length = static_cast<uint32_t>(
        kHaswellNoopBatchLength);
    if (command.kind == MemorySubmissionKind::Noop) {
        if (command.offset || command.value) {
            ret = -EINVAL;
        } else {
            batch_words[0] = kHaswellMiBatchBufferEnd;
            batch_words[1] = kHaswellMiNoop;
        }
    } else if (command.kind == MemorySubmissionKind::WriteDword) {
        if ((command.offset & (sizeof(uint32_t) - 1)) ||
            command.offset > bytes - sizeof(uint32_t) ||
            va > UINT64_MAX - command.offset) {
            ret = -EINVAL;
        } else {
            batch_length = static_cast<uint32_t>(
                kHaswellStoreDwordBatchLength);
            ret = build_haswell_store_dword_batch(
                va + command.offset, command.value, batch_words,
                kHaswellStoreDwordBatchLength);
        }
    } else {
        ret = -EINVAL;
    }
    if (!ret)
        ret = self.upload_batch(batch_handle, batch_words, batch_length);
    if (ret) {
        const int close_ret = self.close_batch(batch_handle);
        return close_ret ? close_ret : ret;
    }

    idw_gpuva actual_va = va;
    ret = self.exec_resource_batch(context.context_id,
                                   static_cast<uint32_t>(backing), va,
                                   batch_handle, batch_length, &actual_va);
    if (ret && ret != -ERANGE) {
        const int close_ret = self.close_batch(batch_handle);
        return close_ret ? close_ret : ret;
    }
    const bool va_mismatch = ret == -ERANGE;
    if (va_mismatch)
        self.contexts_[context_index].poisoned = true;
    const uint64_t token = self.next_fence_++;
    self.fences_.push_back({token, batch_handle});
    *fence = token;
    ret = self.resources_.record_exec_binding(
        static_cast<uint32_t>(backing), context.context_id, va, actual_va);
    return va_mismatch || ret ? -EUCLEAN : 0;
}

int I915AssistedMemoryBackend::finish_fence(size_t index,
                                            int64_t timeout_ns)
{
    if (index >= fences_.size())
        return -EINVAL;
    Fence& fence = fences_[index];
    int ret = wait_batch(fence.batch_handle, timeout_ns);
    if (ret)
        return ret;
    ret = close_batch(fence.batch_handle);
    if (ret)
        return ret;
    fences_.erase(fences_.begin() + static_cast<std::ptrdiff_t>(index));
    return 0;
}

int I915AssistedMemoryBackend::wait(void *userdata, uint64_t token,
                                    int64_t timeout_ns)
{
    if (!userdata || !token || timeout_ns <= 0)
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    size_t index = 0;
    int ret = self.find_fence(token, index);
    return ret ? ret : self.finish_fence(index, timeout_ns);
}

int I915AssistedMemoryBackend::quiesce(void *userdata, uint64_t token)
{
    if (!userdata)
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    if (!token) {
        for (size_t i = 0; i < self.fences_.size();) {
            const int ret = self.finish_fence(i, kInfiniteWait);
            if (ret)
                return ret;
        }
        for (size_t i = 0; i < self.contexts_.size();) {
            if (!self.contexts_[i].pending_batch) {
                ++i;
                continue;
            }
            const int ret = self.cleanup_bind_context(i);
            if (ret)
                return ret;
        }
        return 0;
    }
    size_t index = 0;
    int ret = self.find_fence(token, index);
    return ret ? ret : self.finish_fence(index, kInfiniteWait);
}

int I915AssistedMemoryBackend::cpu_read_dword(void *userdata,
                                               uint64_t backing,
                                               uint64_t offset,
                                               uint32_t *value)
{
    if (!userdata || !backing || backing > UINT32_MAX || !value ||
        (offset & (sizeof(uint32_t) - 1)))
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    I915ResourceSnapshot snapshot{};
    int ret = self.resources_.snapshot(static_cast<uint32_t>(backing),
                                      snapshot);
    if (ret)
        return ret;
    if (offset > snapshot.size - sizeof(*value))
        return -EINVAL;

    drm_i915_gem_pread request{};
    request.handle = static_cast<uint32_t>(backing);
    request.offset = offset;
    request.size = sizeof(*value);
    request.data_ptr = reinterpret_cast<uint64_t>(value);
    return ioctl_result(self.ioctl_fn_, self.drm_fd_,
                        DRM_IOCTL_I915_GEM_PREAD, &request);
}

int I915AssistedMemoryBackend::cpu_write_dword(void *userdata,
                                                uint64_t backing,
                                                uint64_t offset,
                                                uint32_t value)
{
    if (!userdata || !backing || backing > UINT32_MAX ||
        (offset & (sizeof(uint32_t) - 1)))
        return -EINVAL;
    auto& self = *static_cast<I915AssistedMemoryBackend *>(userdata);
    I915ResourceSnapshot snapshot{};
    int ret = self.resources_.snapshot(static_cast<uint32_t>(backing),
                                      snapshot);
    if (ret)
        return ret;
    if (offset > snapshot.size - sizeof(value))
        return -EINVAL;

    drm_i915_gem_pwrite request{};
    request.handle = static_cast<uint32_t>(backing);
    request.offset = offset;
    request.size = sizeof(value);
    request.data_ptr = reinterpret_cast<uint64_t>(&value);
    return ioctl_result(self.ioctl_fn_, self.drm_fd_,
                        DRM_IOCTL_I915_GEM_PWRITE, &request);
}

int I915AssistedMemoryBackend::close_all()
{
    int first_error = 0;
    for (size_t i = 0; i < fences_.size();) {
        const int ret = finish_fence(i, kInfiniteWait);
        if (ret) {
            if (!first_error)
                first_error = ret;
            ++i;
        }
    }
    if (!fences_.empty())
        return first_error ? first_error : -EBUSY;
    for (size_t i = 0; i < contexts_.size();) {
        const int ret = cleanup_bind_context(i);
        if (ret) {
            if (!first_error)
                first_error = ret;
            ++i;
        }
    }
    if (!contexts_.empty())
        return first_error ? first_error : -EBUSY;
    const int close_ret = resources_.close_all();
    if (close_ret && !first_error)
        first_error = close_ret;
    return first_error;
}

size_t I915AssistedMemoryBackend::context_count() const
{
    return contexts_.size();
}

size_t I915AssistedMemoryBackend::pending_fence_count() const
{
    return fences_.size();
}

size_t I915AssistedMemoryBackend::resource_count() const
{
    return resources_.resource_count();
}

} // namespace idw
