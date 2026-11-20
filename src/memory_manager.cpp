/* SPDX-License-Identifier: MIT */
#include "../include/idw/memory_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <limits>
#include <new>

namespace idw {
namespace {

constexpr uint64_t kPageSize = 4096;

bool valid_ops(const MemoryBackendOps& ops)
{
    return ops.create_backing && ops.destroy_backing && ops.bind &&
           ops.unbind && ops.invalidate && ops.submit && ops.wait &&
           ops.quiesce;
}

int page_aligned_size(uint64_t bytes, uint64_t& aligned)
{
    if (!bytes || bytes > std::numeric_limits<uint64_t>::max() -
                              (kPageSize - 1))
        return -EINVAL;
    aligned = (bytes + kPageSize - 1) & ~(kPageSize - 1);
    return 0;
}

int callback_status(int status)
{
    return status > 0 ? -EPROTO : status;
}

} // namespace

MemoryManager::MemoryManager()
    : ops_{}, userdata_(nullptr), va_space_{}, next_handle_(1),
      next_ticket_(1), mapping_mismatch_(false), initialized_(false)
{
}

MemoryManager::~MemoryManager()
{
    if (!resources_.empty() || !submissions_.empty())
        std::fprintf(stderr,
                     "idw: memory manager destroyed with live resources or submissions\n");
    if (initialized_)
        idw_gpuva_space_fini(&va_space_);
}

int MemoryManager::initialize(const MemoryBackendOps& ops, void *userdata,
                              idw_gpuva min_va, idw_gpuva max_va)
{
    if (initialized_ || !valid_ops(ops))
        return -EINVAL;
    const int ret = idw_gpuva_space_init(&va_space_, min_va, max_va);
    if (ret)
        return ret;
    ops_ = ops;
    userdata_ = userdata;
    initialized_ = true;
    return 0;
}

int MemoryManager::shutdown()
{
    if (!initialized_)
        return -EINVAL;
    if (!resources_.empty() || !submissions_.empty())
        return -EBUSY;
    idw_gpuva_space_fini(&va_space_);
    ops_ = {};
    userdata_ = nullptr;
    mapping_mismatch_ = false;
    initialized_ = false;
    return 0;
}

int MemoryManager::find_resource(idw_handle handle, size_t& index) const
{
    for (size_t i = 0; i < resources_.size(); ++i) {
        if (resources_[i].handle == handle) {
            index = i;
            return 0;
        }
    }
    return -ENOENT;
}

int MemoryManager::find_submission(uint64_t ticket, size_t& index) const
{
    for (size_t i = 0; i < submissions_.size(); ++i) {
        if (submissions_[i].ticket == ticket) {
            index = i;
            return 0;
        }
    }
    return -ENOENT;
}

int MemoryManager::create(uint64_t bytes, idw_handle& handle)
{
    if (!initialized_ || next_handle_ == 0)
        return -EINVAL;
    uint64_t aligned = 0;
    int ret = page_aligned_size(bytes, aligned);
    if (ret)
        return ret;
    if (resources_.size() == resources_.max_size())
        return -ENOSPC;
    try {
        resources_.reserve(resources_.size() + 1);
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }
    uint64_t backing = 0;
    ret = callback_status(ops_.create_backing(userdata_, aligned, &backing));
    if (ret)
        return ret;
    resources_.push_back({next_handle_, aligned, backing, 0, 0, 0,
                          BindingState::Unbound, false});
    handle = next_handle_++;
    return 0;
}

int MemoryManager::bind(idw_handle handle, idw_gpuva requested_va,
                        idw_gpuva& actual_va)
{
    if (!initialized_)
        return -EINVAL;
    if (mapping_mismatch_)
        return -EIO;
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    Resource& resource = resources_[index];
    if (resource.binding_state != BindingState::Unbound)
        return -EBUSY;

    idw_gpuva reserved = requested_va;
    if (requested_va) {
        ret = idw_gpuva_reserve(&va_space_, requested_va, resource.bytes, 0);
    } else {
        ret = idw_gpuva_alloc(&va_space_, resource.bytes, kPageSize, 0,
                              &reserved);
    }
    if (ret)
        return ret;

    idw_gpuva backend_va = reserved;
    ret = callback_status(ops_.bind(userdata_, resource.backing, reserved,
                                    resource.bytes, &backend_va));
    if (ret) {
        if (ret == -EUCLEAN) {
            resource.reserved_va = reserved;
            resource.backend_va = backend_va;
            resource.binding_state = BindingState::NeedsUnbind;
            if (backend_va != reserved) {
                resource.mismatched_va = true;
                mapping_mismatch_ = true;
            }
            return ret;
        }
        const int release = idw_gpuva_free(&va_space_, reserved);
        return release ? release : ret;
    }

    resource.reserved_va = reserved;
    resource.backend_va = backend_va;
    if (backend_va != reserved) {
        resource.binding_state = BindingState::NeedsUnbind;
        resource.mismatched_va = true;
        mapping_mismatch_ = true;
        return -ERANGE;
    }
    resource.binding_state = BindingState::Bound;
    actual_va = backend_va;
    return 0;
}

int MemoryManager::finish_unbind(Resource& resource)
{
    if (resource.binding_state == BindingState::NeedsUnbind) {
        int ret = callback_status(ops_.unbind(userdata_, resource.backing,
                                              resource.backend_va,
                                              resource.bytes));
        if (ret)
            return ret;
        resource.binding_state = BindingState::NeedsInvalidation;
    }
    if (resource.binding_state != BindingState::NeedsInvalidation)
        return -EINVAL;

    int ret = callback_status(ops_.invalidate(userdata_, resource.backend_va,
                                              resource.bytes));
    if (ret)
        return ret;
    ret = idw_gpuva_free(&va_space_, resource.reserved_va);
    if (ret)
        return ret;
    resource.reserved_va = 0;
    resource.backend_va = 0;
    resource.binding_state = BindingState::Unbound;
    if (resource.mismatched_va) {
        resource.mismatched_va = false;
        mapping_mismatch_ = false;
    }
    return 0;
}

int MemoryManager::unbind(idw_handle handle)
{
    if (!initialized_)
        return -EINVAL;
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    Resource& resource = resources_[index];
    if (resource.references)
        return -EBUSY;
    if (resource.binding_state == BindingState::Unbound)
        return -ENOENT;
    if (resource.binding_state == BindingState::Bound)
        resource.binding_state = BindingState::NeedsUnbind;
    return finish_unbind(resource);
}

int MemoryManager::recover_binding(idw_handle handle)
{
    if (!initialized_)
        return -EINVAL;
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    Resource& resource = resources_[index];
    if (resource.references)
        return -EBUSY;
    if (resource.binding_state != BindingState::NeedsUnbind &&
        resource.binding_state != BindingState::NeedsInvalidation)
        return -EINVAL;
    return finish_unbind(resource);
}

int MemoryManager::submit(idw_handle handle, uint64_t& ticket)
{
    return submit_command(handle, {MemorySubmissionKind::Noop, 0, 0}, ticket);
}

int MemoryManager::submit_write_dword(idw_handle handle, uint64_t offset,
                                      uint32_t value, uint64_t& ticket)
{
    return submit_command(handle,
                          {MemorySubmissionKind::WriteDword, offset, value},
                          ticket);
}

int MemoryManager::submit_command(idw_handle handle,
                                  const MemorySubmissionCommand& command,
                                  uint64_t& ticket)
{
    if (!initialized_ || next_ticket_ == 0)
        return -EINVAL;
    if (mapping_mismatch_)
        return -EIO;
    size_t resource_index = 0;
    int ret = find_resource(handle, resource_index);
    if (ret)
        return ret;
    Resource& resource = resources_[resource_index];
    if (resource.binding_state != BindingState::Bound)
        return -EINVAL;
    switch (command.kind) {
    case MemorySubmissionKind::Noop:
        if (command.offset || command.value)
            return -EINVAL;
        break;
    case MemorySubmissionKind::WriteDword:
        if ((command.offset & (sizeof(uint32_t) - 1)) ||
            command.offset > resource.bytes - sizeof(uint32_t))
            return -EINVAL;
        break;
    default:
        return -EINVAL;
    }
    if (resource.references == std::numeric_limits<uint32_t>::max())
        return -EOVERFLOW;
    if (submissions_.size() == submissions_.max_size())
        return -ENOSPC;

    const uint64_t new_ticket = next_ticket_;
    try {
        submissions_.push_back({new_ticket, 0, handle});
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }
    ++resource.references;

    uint64_t fence = 0;
    ret = callback_status(ops_.submit(userdata_, resource.backing,
                                      resource.backend_va, resource.bytes,
                                      command, &fence));
    if (ret && ret != -EUCLEAN) {
        --resource.references;
        submissions_.pop_back();
        return ret;
    }
    submissions_.back().fence = fence;
    ticket = new_ticket;
    ++next_ticket_;
    if (ret == -EUCLEAN)
        return ret;
    return fence ? 0 : -EPROTO;
}

int MemoryManager::cpu_read_dword(idw_handle handle, uint64_t offset,
                                  uint32_t& value)
{
    if (!initialized_)
        return -EINVAL;
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    const Resource& resource = resources_[index];
    if ((offset & (sizeof(uint32_t) - 1)) ||
        offset > resource.bytes - sizeof(uint32_t))
        return -EINVAL;
    if (resource.references)
        return -EBUSY;
    if (!ops_.cpu_read_dword)
        return -EOPNOTSUPP;
    return callback_status(ops_.cpu_read_dword(userdata_, resource.backing,
                                               offset, &value));
}

int MemoryManager::cpu_write_dword(idw_handle handle, uint64_t offset,
                                   uint32_t value)
{
    if (!initialized_)
        return -EINVAL;
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    const Resource& resource = resources_[index];
    if ((offset & (sizeof(uint32_t) - 1)) ||
        offset > resource.bytes - sizeof(uint32_t))
        return -EINVAL;
    if (resource.references)
        return -EBUSY;
    if (!ops_.cpu_write_dword)
        return -EOPNOTSUPP;
    return callback_status(ops_.cpu_write_dword(userdata_, resource.backing,
                                                offset, value));
}

int MemoryManager::release_submission(size_t index)
{
    size_t resource_index = 0;
    int ret = find_resource(submissions_[index].resource, resource_index);
    if (ret)
        return ret;
    Resource& resource = resources_[resource_index];
    if (!resource.references)
        return -EUCLEAN;
    --resource.references;
    submissions_.erase(submissions_.begin() +
                       static_cast<std::ptrdiff_t>(index));
    return 0;
}

int MemoryManager::wait(uint64_t ticket, int64_t timeout_ns)
{
    if (!initialized_ || timeout_ns <= 0)
        return -EINVAL;
    size_t index = 0;
    int ret = find_submission(ticket, index);
    if (ret)
        return ret;
    if (!submissions_[index].fence)
        return -EPROTO;
    ret = callback_status(ops_.wait(userdata_, submissions_[index].fence,
                                     timeout_ns));
    if (ret)
        return ret;
    return release_submission(index);
}

int MemoryManager::recover_submission(uint64_t ticket)
{
    if (!initialized_)
        return -EINVAL;
    size_t index = 0;
    int ret = find_submission(ticket, index);
    if (ret)
        return ret;
    ret = callback_status(ops_.quiesce(userdata_, submissions_[index].fence));
    if (ret)
        return ret;
    return release_submission(index);
}

int MemoryManager::destroy(idw_handle handle)
{
    if (!initialized_)
        return -EINVAL;
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    Resource& resource = resources_[index];
    if (resource.references ||
        resource.binding_state != BindingState::Unbound)
        return -EBUSY;
    ret = callback_status(ops_.destroy_backing(userdata_, resource.backing));
    if (ret)
        return ret;
    resources_.erase(resources_.begin() +
                     static_cast<std::ptrdiff_t>(index));
    return 0;
}

int MemoryManager::query(idw_handle handle, MemoryResourceInfo& info) const
{
    size_t index = 0;
    int ret = find_resource(handle, index);
    if (ret)
        return ret;
    const Resource& resource = resources_[index];
    info.handle = resource.handle;
    info.bytes = resource.bytes;
    info.gpu_va = resource.reserved_va;
    info.submission_references = resource.references;
    info.bound = resource.binding_state == BindingState::Bound;
    info.recovery_required =
        resource.binding_state == BindingState::NeedsUnbind ||
        resource.binding_state == BindingState::NeedsInvalidation;
    return 0;
}

size_t MemoryManager::resource_count() const
{
    return resources_.size();
}

size_t MemoryManager::submission_count() const
{
    return submissions_.size();
}

} // namespace idw
