/* SPDX-License-Identifier: MIT */
#include "../include/idw/native_dma_backend.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace idw {
namespace {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kDirectoryGgttOffset = 0x100000;
constexpr uint32_t kPdes = 512;

int ioctl_call(int fd, unsigned long command, void *request)
{
    if (::ioctl(fd, command, request) < 0)
        return -errno;
    return 0;
}

} // namespace

NativeDmaMemoryBackend::NativeDmaMemoryBackend(const char *device_path)
    : device_path_(device_path ? device_path : IDW_DMA_TEST_DEVICE),
      fd_(-1),
      scope_{},
      next_fence_(1),
      model_(nullptr),
      directory_{}
{
    (void)open_device();
}

NativeDmaMemoryBackend::~NativeDmaMemoryBackend()
{
    (void)close_all();
}

int NativeDmaMemoryBackend::open_device()
{
    if (fd_ >= 0)
        return 0;
    fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
    return fd_ < 0 ? -errno : 0;
}

MemoryBackendOps NativeDmaMemoryBackend::operations()
{
    MemoryBackendOps ops{};
    ops.create_backing = &NativeDmaMemoryBackend::create_backing_cb;
    ops.destroy_backing = &NativeDmaMemoryBackend::destroy_backing_cb;
    ops.bind = &NativeDmaMemoryBackend::bind_cb;
    ops.unbind = &NativeDmaMemoryBackend::unbind_cb;
    ops.invalidate = &NativeDmaMemoryBackend::invalidate_cb;
    ops.submit = &NativeDmaMemoryBackend::submit_cb;
    ops.wait = &NativeDmaMemoryBackend::wait_cb;
    ops.quiesce = &NativeDmaMemoryBackend::quiesce_cb;
    ops.cpu_read_dword = &NativeDmaMemoryBackend::cpu_read_dword_cb;
    ops.cpu_write_dword = &NativeDmaMemoryBackend::cpu_write_dword_cb;
    return ops;
}

NativeDmaMemoryBackend::Allocation *
NativeDmaMemoryBackend::find_allocation(uint64_t backing)
{
    for (Allocation& allocation : allocations_) {
        if (allocation.backing == backing)
            return &allocation;
    }
    return nullptr;
}

const NativeDmaMemoryBackend::Allocation *
NativeDmaMemoryBackend::find_allocation(uint64_t backing) const
{
    for (const Allocation& allocation : allocations_) {
        if (allocation.backing == backing)
            return &allocation;
    }
    return nullptr;
}

NativeDmaMemoryBackend::PageTable *
NativeDmaMemoryBackend::find_page_table(uint64_t cookie)
{
    for (PageTable& table : page_tables_) {
        if (table.cookie == cookie)
            return &table;
    }
    return nullptr;
}

bool NativeDmaMemoryBackend::add_counter(uint64_t& counter, uint64_t amount)
{
    if (counter > UINT64_MAX - amount)
        return false;
    counter += amount;
    return true;
}

bool NativeDmaMemoryBackend::sub_counter(uint64_t& counter, uint64_t amount)
{
    if (counter < amount)
        return false;
    counter -= amount;
    return true;
}

bool NativeDmaMemoryBackend::transition(Allocation& allocation,
                                        ResourceState next)
{
    const ResourceState current = allocation.state;
    if (current == next)
        return true;
    bool legal = false;
    switch (current) {
    case ResourceState::New:
        legal = next == ResourceState::BackingAllocated ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::BackingAllocated:
        legal = next == ResourceState::VaReserved ||
                next == ResourceState::Destroying ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::VaReserved:
        legal = next == ResourceState::DmaMapped ||
                next == ResourceState::BackingAllocated ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::DmaMapped:
        legal = next == ResourceState::PgttPopulated ||
                next == ResourceState::BackingAllocated ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::PgttPopulated:
        legal = next == ResourceState::PtSynchronized ||
                next == ResourceState::BackingAllocated ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::PtSynchronized:
        legal = next == ResourceState::ReadyInactive ||
                next == ResourceState::BackingAllocated ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::ReadyInactive:
        legal = next == ResourceState::Destroying ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::Destroying:
        legal = next == ResourceState::BackingAllocated ||
                next == ResourceState::Released ||
                next == ResourceState::Quarantined;
        break;
    case ResourceState::Quarantined:
        legal = next == ResourceState::Destroying ||
                next == ResourceState::Released;
        break;
    case ResourceState::Released:
        legal = false;
        break;
    }
    if (!legal)
        return false;
    allocation.state = next;
    return true;
}

void NativeDmaMemoryBackend::quarantine(Allocation& allocation)
{
    if (allocation.state != ResourceState::Quarantined) {
        if (transition(allocation, ResourceState::Quarantined))
            (void)add_counter(counters_.quarantined_pages,
                              allocation.page_count);
    }
}

int NativeDmaMemoryBackend::check_inactive() const
{
    if (!model_)
        return 0;
    if (idw_hsw_ppgtt_is_active(model_))
        return -EBUSY;
    struct idw_hsw_ppgtt_pending_invalidation pending = {};
    pending.size = sizeof(pending);
    if (idw_hsw_ppgtt_pending(model_, &pending))
        return -EIO;
    return pending.pending ? -EAGAIN : 0;
}

int NativeDmaMemoryBackend::create_backing_cb(void *userdata, uint64_t bytes,
                                              uint64_t *backing)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->create_backing_cb_impl(bytes, backing);
}

int NativeDmaMemoryBackend::create_backing_cb_impl(uint64_t bytes,
                                                   uint64_t *backing)
{
    if (!backing || !bytes || (bytes % kPageSize))
        return -EINVAL;
    if (faults_.create_fail_at >= 0 &&
        static_cast<size_t>(faults_.create_fail_at) == allocations_.size())
        return -ENOMEM;
    int ret = open_device();
    if (ret)
        return ret;

    struct idw_dma_test_alloc request = {};
    request.size = sizeof(request);
    request.direction = IDW_DMA_TEST_BIDIRECTIONAL;
    request.bytes = bytes;
    ret = ioctl_call(fd_, IDW_DMA_TEST_IOCTL_ALLOC, &request);
    if (ret)
        return ret;
    if (!request.cookie || !request.page_count || !request.device_cookie ||
        !request.generation) {
        struct idw_dma_test_free release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &release);
        return -EIO;
    }
    if (scope_.device_cookie &&
        (scope_.device_cookie != request.device_cookie ||
         scope_.generation != request.generation)) {
        struct idw_dma_test_free release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &release);
        return -EXDEV;
    }

    const uint64_t mapping_bytes = static_cast<uint64_t>(request.page_count) *
                                   kPageSize;
    void *address = ::mmap(nullptr, static_cast<size_t>(mapping_bytes),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                           static_cast<off_t>(request.cookie * kPageSize));
    if (address == MAP_FAILED) {
        struct idw_dma_test_free release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &release);
        return -errno;
    }

    Allocation allocation{};
    allocation.backing = request.cookie;
    allocation.device_cookie = request.device_cookie;
    allocation.generation = request.generation;
    allocation.bytes = mapping_bytes;
    allocation.address = address;
    allocation.bound = false;
    allocation.ptes_cleared = false;
    allocation.ptes_accounted = false;
    allocation.invalidation_done = false;
    allocation.va_released = false;
    allocation.dma_unmapped = false;
    allocation.backing_released = false;
    allocation.release_safe = false;
    allocation.va = 0;
    allocation.page_count = request.page_count;
    allocation.state = ResourceState::New;
    if (!add_counter(counters_.allocated_pages, request.page_count)) {
        ::munmap(address, static_cast<size_t>(mapping_bytes));
        struct idw_dma_test_free release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &release);
        return -EOVERFLOW;
    }
    if (!add_counter(counters_.mapped_pages, request.page_count)) {
        (void)sub_counter(counters_.allocated_pages, request.page_count);
        ::munmap(address, static_cast<size_t>(mapping_bytes));
        struct idw_dma_test_free release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &release);
        return -EOVERFLOW;
    }
    try {
        allocations_.push_back(allocation);
    } catch (const std::bad_alloc&) {
        (void)sub_counter(counters_.allocated_pages, request.page_count);
        (void)sub_counter(counters_.mapped_pages, request.page_count);
        ::munmap(address, static_cast<size_t>(mapping_bytes));
        struct idw_dma_test_free release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &release);
        return -ENOMEM;
    }
    Allocation& stored = allocations_.back();
    if (!transition(stored, ResourceState::BackingAllocated)) {
        allocations_.pop_back();
        (void)sub_counter(counters_.allocated_pages, request.page_count);
        (void)sub_counter(counters_.mapped_pages, request.page_count);
        ::munmap(address, static_cast<size_t>(mapping_bytes));
        struct idw_dma_test_free release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &release);
        return -EIO;
    }
    scope_.device_cookie = request.device_cookie;
    scope_.generation = request.generation;
    *backing = request.cookie;
    return 0;
}

int NativeDmaMemoryBackend::destroy_backing_cb(void *userdata,
                                               uint64_t backing)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->destroy_backing_cb_impl(backing);
}

int NativeDmaMemoryBackend::destroy_backing_cb_impl(uint64_t backing)
{
    Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    if (allocation->bound)
        return -EBUSY;
    if (allocation->state == ResourceState::Quarantined &&
        !allocation->release_safe)
        return -EBUSY;
    if (allocation->state != ResourceState::BackingAllocated &&
        allocation->state != ResourceState::Quarantined)
        return -EINVAL;
    bool was_quarantined =
        allocation->state == ResourceState::Quarantined;
    if (faults_.destroy_fail) {
        allocation->release_safe = true;
        quarantine(*allocation);
        return -EIO;
    }
    if (!transition(*allocation, ResourceState::Destroying))
        return -EINVAL;

    if (!allocation->dma_unmapped) {
        if (allocation->address) {
            ::munmap(allocation->address,
                     static_cast<size_t>(allocation->bytes));
            allocation->address = nullptr;
        }
        allocation->dma_unmapped = true;
        if (faults_.destroy_fail_after_unmap) {
            allocation->release_safe = true;
            quarantine(*allocation);
            return -EIO;
        }
    }

    if (!allocation->backing_released) {
        struct idw_dma_test_free request = {};
        request.size = sizeof(request);
        request.cookie = allocation->backing;
        int ret = ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE, &request);
        if (ret) {
            allocation->release_safe = true;
            quarantine(*allocation);
            return ret;
        }
        allocation->backing_released = true;
    }

    if (counters_.allocated_pages < allocation->page_count ||
        counters_.mapped_pages < allocation->page_count) {
        allocation->release_safe = true;
        quarantine(*allocation);
        return -EIO;
    }
    (void)sub_counter(counters_.allocated_pages, allocation->page_count);
    (void)sub_counter(counters_.mapped_pages, allocation->page_count);
    if (was_quarantined)
        (void)sub_counter(counters_.quarantined_pages,
                          allocation->page_count);
    if (!transition(*allocation, ResourceState::Released))
        return -EIO;
    allocations_.erase(allocations_.begin() +
                       (allocation - allocations_.data()));
    return 0;
}

int NativeDmaMemoryBackend::bind_cb(void *userdata, uint64_t backing,
                                    idw_gpuva requested_va, uint64_t bytes,
                                    idw_gpuva *actual_va)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->bind_cb_impl(backing, requested_va, bytes, actual_va);
}

int NativeDmaMemoryBackend::bind_cb_impl(uint64_t backing,
                                         idw_gpuva requested_va,
                                         uint64_t bytes,
                                         idw_gpuva *actual_va)
{
    if (!actual_va || !requested_va || (requested_va % kPageSize) ||
        (bytes % kPageSize))
        return -EINVAL;
    Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    if (allocation->bound || allocation->state == ResourceState::Quarantined)
        return -EBUSY;
    if (allocation->bytes != bytes)
        return -EINVAL;
    if (allocation->state != ResourceState::BackingAllocated)
        return -EINVAL;
    allocation->ptes_cleared = false;
    allocation->ptes_accounted = false;
    allocation->invalidation_done = false;
    allocation->va_reserved = false;
    allocation->va_released = false;
    allocation->dma_unmapped = false;
    allocation->backing_released = false;
    if (!transition(*allocation, ResourceState::VaReserved))
        return -EINVAL;

    std::vector<idw_dma_page_mapping> mappings;
    int ret = get_segments(backing, mappings);
    if (ret) {
        (void)transition(*allocation, ResourceState::BackingAllocated);
        return ret;
    }
    if (mappings.size() * kPageSize != bytes ||
        mappings.size() != allocation->page_count) {
        (void)transition(*allocation, ResourceState::BackingAllocated);
        return -EIO;
    }
    if (!transition(*allocation, ResourceState::DmaMapped))
        return -EIO;
    ret = ensure_model();
    if (ret) {
        (void)transition(*allocation, ResourceState::BackingAllocated);
        return ret;
    }
    ret = idw_hsw_ppgtt_map(model_, requested_va, bytes, mappings.data(),
                            mappings.size());
    if (ret) {
        bool cleared = true;
        if (!idw_hsw_ppgtt_range_cleared(model_, requested_va, bytes,
                                         &cleared) &&
            !cleared) {
            allocation->bound = true;
            allocation->va = requested_va;
            *actual_va = requested_va;
            if (!add_counter(counters_.va_reservations, 1)) {
                quarantine(*allocation);
                return -EOVERFLOW;
            }
            allocation->va_reserved = true;
            if (!add_counter(counters_.populated_ptes,
                             allocation->page_count)) {
                quarantine(*allocation);
                return -EOVERFLOW;
            }
            quarantine(*allocation);
            return -EUCLEAN;
        }
        (void)transition(*allocation, ResourceState::BackingAllocated);
        return ret;
    }
    if (!transition(*allocation, ResourceState::PgttPopulated)) {
        quarantine(*allocation);
        return -EIO;
    }
    allocation->bound = true;
    allocation->va = requested_va;
    if (!add_counter(counters_.va_reservations, 1)) {
        quarantine(*allocation);
        return -EOVERFLOW;
    }
    allocation->va_reserved = true;
    if (!add_counter(counters_.populated_ptes, allocation->page_count)) {
        quarantine(*allocation);
        return -EOVERFLOW;
    }
    ret = check_inactive();
    if (ret == -EAGAIN) {
        struct idw_hsw_ppgtt_pending_invalidation pending = {};
        pending.size = sizeof(pending);
        ret = idw_hsw_ppgtt_pending(model_, &pending);
        if (!ret && pending.pending)
            ret = idw_hsw_ppgtt_invalidation_complete(model_,
                                                      pending.generation);
        if (!ret)
            ret = check_inactive();
    }
    if (ret) {
        quarantine(*allocation);
        return ret;
    }
    if (!transition(*allocation, ResourceState::PtSynchronized)) {
        quarantine(*allocation);
        return -EIO;
    }
    if (!transition(*allocation, ResourceState::ReadyInactive)) {
        quarantine(*allocation);
        return -EIO;
    }
    if (faults_.bind_fail_after_map) {
        allocation->bound = true;
        allocation->va = requested_va;
        *actual_va = requested_va;
        quarantine(*allocation);
        return -EIO;
    }
    allocation->bound = true;
    allocation->va = requested_va;
    *actual_va = requested_va;
    return 0;
}

int NativeDmaMemoryBackend::complete_unbind(Allocation& allocation)
{
    if (!model_)
        return -ENODEV;
    if (!allocation.invalidation_done) {
        struct idw_hsw_ppgtt_pending_invalidation pending = {};
        pending.size = sizeof(pending);
        int ret = idw_hsw_ppgtt_pending(model_, &pending);
        if (ret)
            return ret;
        if (pending.pending) {
            ret = idw_hsw_ppgtt_invalidation_complete(model_,
                                                      pending.generation);
            if (ret)
                return ret;
        }
        allocation.invalidation_done = true;
    }
    if (!allocation.va_released) {
        if (allocation.va_reserved) {
            if (!sub_counter(counters_.va_reservations, 1))
                return -EIO;
            allocation.va_reserved = false;
        }
        allocation.va_released = true;
    }
    allocation.bound = false;
    allocation.va = 0;
    if (!transition(allocation, ResourceState::BackingAllocated))
        return -EIO;
    allocation.ptes_cleared = false;
    allocation.ptes_accounted = false;
    allocation.invalidation_done = false;
    allocation.va_reserved = false;
    allocation.va_released = false;
    allocation.dma_unmapped = false;
    allocation.backing_released = false;
    return 0;
}

int NativeDmaMemoryBackend::unbind_cb(void *userdata, uint64_t backing,
                                      idw_gpuva va, uint64_t bytes)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->unbind_cb_impl(backing, va, bytes);
}

int NativeDmaMemoryBackend::unbind_cb_impl(uint64_t backing, idw_gpuva va,
                                           uint64_t bytes)
{
    Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    if (allocation->bytes != bytes || allocation->va != va)
        return -EINVAL;
    if (!model_)
        return -ENODEV;
    if (allocation->state == ResourceState::Quarantined &&
        !allocation->ptes_cleared && !allocation->bound)
        return -EBUSY;
    if (allocation->state != ResourceState::ReadyInactive &&
        allocation->state != ResourceState::Quarantined)
        return -EINVAL;
    bool was_quarantined =
        allocation->state == ResourceState::Quarantined;
    if (!transition(*allocation, ResourceState::Destroying))
        return -EINVAL;

    int ret = idw_hsw_ppgtt_retry_sync(model_);
    if (ret) {
        quarantine(*allocation);
        return ret;
    }
    if (!allocation->ptes_cleared) {
        bool cleared = false;
        ret = idw_hsw_ppgtt_range_cleared(model_, va, bytes, &cleared);
        if (ret) {
            quarantine(*allocation);
            return ret;
        }
        if (!cleared) {
            ret = idw_hsw_ppgtt_unmap(model_, va, bytes);
            if (ret) {
                cleared = false;
                if (!idw_hsw_ppgtt_range_cleared(model_, va, bytes,
                                                 &cleared) &&
                    cleared)
                    allocation->ptes_cleared = true;
                else {
                    quarantine(*allocation);
                    return ret;
                }
            } else {
                allocation->ptes_cleared = true;
            }
        } else {
            allocation->ptes_cleared = true;
        }
    }
    if (!allocation->ptes_accounted) {
        if (!sub_counter(counters_.populated_ptes,
                         allocation->page_count)) {
            quarantine(*allocation);
            return -EIO;
        }
        allocation->ptes_accounted = true;
    }
    ret = idw_hsw_ppgtt_retry_sync(model_);
    if (ret) {
        quarantine(*allocation);
        return ret;
    }
    if (faults_.unbind_fail_invalidation) {
        quarantine(*allocation);
        return -EIO;
    }
    ret = complete_unbind(*allocation);
    if (ret) {
        quarantine(*allocation);
        return ret;
    }
    if (was_quarantined)
        (void)sub_counter(counters_.quarantined_pages,
                          allocation->page_count);
    return 0;
}

int NativeDmaMemoryBackend::invalidate_cb(void *userdata, idw_gpuva va,
                                          uint64_t bytes)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->invalidate_cb_impl(va, bytes);
}

int NativeDmaMemoryBackend::invalidate_cb_impl(idw_gpuva va, uint64_t bytes)
{
    (void)va;
    (void)bytes;
    if (!model_)
        return 0;
    struct idw_hsw_ppgtt_pending_invalidation pending = {};
    pending.size = sizeof(pending);
    int ret = idw_hsw_ppgtt_pending(model_, &pending);
    if (ret)
        return ret;
    if (!pending.pending)
        return 0;
    return idw_hsw_ppgtt_invalidation_complete(model_, pending.generation);
}

int NativeDmaMemoryBackend::submit_cb(
    void *userdata, uint64_t backing, idw_gpuva va, uint64_t bytes,
    const MemorySubmissionCommand& command, uint64_t *fence)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->submit_cb_impl(backing, va, bytes, command, fence);
}

int NativeDmaMemoryBackend::submit_cb_impl(
    uint64_t backing, idw_gpuva va, uint64_t bytes,
    const MemorySubmissionCommand& command, uint64_t *fence)
{
    (void)va;
    (void)bytes;
    if (!fence)
        return -EINVAL;
    Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    if (command.kind == MemorySubmissionKind::WriteDword) {
        if (command.offset > allocation->bytes ||
            sizeof(uint32_t) > allocation->bytes - command.offset)
            return -EINVAL;
        std::memcpy(static_cast<uint8_t *>(allocation->address) +
                        command.offset,
                    &command.value, sizeof(command.value));
    } else if (command.kind != MemorySubmissionKind::Noop) {
        return -EINVAL;
    }
    if (!next_fence_)
        return -EOVERFLOW;
    *fence = next_fence_++;
    return 0;
}

int NativeDmaMemoryBackend::wait_cb(void *userdata, uint64_t fence,
                                    int64_t timeout_ns)
{
    (void)timeout_ns;
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->wait_cb_impl(fence);
}

int NativeDmaMemoryBackend::wait_cb_impl(uint64_t fence)
{
    if (!fence || fence >= next_fence_)
        return -EINVAL;
    return 0;
}

int NativeDmaMemoryBackend::quiesce_cb(void *userdata, uint64_t fence)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    if (!self)
        return -EINVAL;
    if (fence && fence >= self->next_fence_)
        return -EINVAL;
    return self->check_inactive();
}

int NativeDmaMemoryBackend::cpu_read_dword_cb(void *userdata, uint64_t backing,
                                              uint64_t offset,
                                              uint32_t *value)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->cpu_read_dword_cb_impl(backing, offset, value);
}

int NativeDmaMemoryBackend::cpu_read_dword_cb_impl(uint64_t backing,
                                                   uint64_t offset,
                                                   uint32_t *value)
{
    if (!value)
        return -EINVAL;
    const Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    if (offset > allocation->bytes ||
        sizeof(uint32_t) > allocation->bytes - offset)
        return -EINVAL;
    std::memcpy(value, static_cast<const uint8_t *>(allocation->address) +
                          offset,
                sizeof(*value));
    return 0;
}

int NativeDmaMemoryBackend::cpu_write_dword_cb(void *userdata,
                                               uint64_t backing,
                                               uint64_t offset,
                                               uint32_t value)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(userdata);
    return self->cpu_write_dword_cb_impl(backing, offset, value);
}

int NativeDmaMemoryBackend::cpu_write_dword_cb_impl(uint64_t backing,
                                                    uint64_t offset,
                                                    uint32_t value)
{
    Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    if (offset > allocation->bytes ||
        sizeof(uint32_t) > allocation->bytes - offset)
        return -EINVAL;
    std::memcpy(static_cast<uint8_t *>(allocation->address) + offset, &value,
                sizeof(value));
    return 0;
}

int NativeDmaMemoryBackend::get_segments(
    uint64_t backing, std::vector<idw_dma_page_mapping>& mappings) const
{
    const Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    const uint64_t page_count = allocation->bytes / kPageSize;
    if (!page_count || page_count > IDW_DMA_TEST_MAX_SEGMENTS)
        return -E2BIG;

    std::vector<idw_dma_test_segment> segments(IDW_DMA_TEST_MAX_SEGMENTS);
    struct idw_dma_test_segments request = {};
    request.size = sizeof(request);
    request.capacity = IDW_DMA_TEST_MAX_SEGMENTS;
    request.cookie = allocation->backing;
    request.offset = 0;
    request.bytes = allocation->bytes;
    request.segments = reinterpret_cast<uintptr_t>(segments.data());
    int ret = ioctl_call(fd_, IDW_DMA_TEST_IOCTL_SEGMENTS, &request);
    if (ret)
        return ret;
    if (request.count != page_count)
        return -EIO;

    mappings.resize(static_cast<size_t>(request.count));
    for (uint32_t i = 0; i < request.count; ++i) {
        mappings[i].dma_address = segments[i].dma_address;
        mappings[i].bytes = segments[i].bytes;
        mappings[i].scope.device_cookie = segments[i].device_cookie;
        mappings[i].scope.generation = segments[i].generation;
        mappings[i].mapping_cookie = segments[i].mapping_cookie;
    }
    return 0;
}

int NativeDmaMemoryBackend::ensure_model()
{
    if (model_)
        return 0;
    if (!scope_.device_cookie || !scope_.generation)
        return -EINVAL;

    struct idw_hsw_ppgtt_page_ops page_ops = {};
    page_ops.size = sizeof(page_ops);
    page_ops.version = 1;
    page_ops.scope = scope_;
    page_ops.alloc_page_table_page =
        &NativeDmaMemoryBackend::alloc_page_table_page_cb;
    page_ops.free_page_table_page =
        &NativeDmaMemoryBackend::free_page_table_page_cb;
    page_ops.sync_page_table_for_device =
        &NativeDmaMemoryBackend::sync_page_table_for_device_cb;
    page_ops.context = this;

    struct idw_hsw_ppgtt_directory_ops directory_ops = {};
    directory_ops.size = sizeof(directory_ops);
    directory_ops.version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    directory_ops.scope = scope_;
    directory_ops.alloc_directory =
        &NativeDmaMemoryBackend::alloc_directory_cb;
    directory_ops.free_directory =
        &NativeDmaMemoryBackend::free_directory_cb;
    directory_ops.write_pde_gttaddr =
        &NativeDmaMemoryBackend::write_pde_gttaddr_cb;
    directory_ops.read_pde_gttaddr =
        &NativeDmaMemoryBackend::read_pde_gttaddr_cb;
    directory_ops.flush_gtt = &NativeDmaMemoryBackend::flush_gtt_cb;
    directory_ops.context = this;

    return idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model_);
}

int NativeDmaMemoryBackend::alloc_page_table_page_cb(
    void *context, struct idw_dma_page *page)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(context);
    return self->alloc_page_table_page(page);
}

int NativeDmaMemoryBackend::alloc_page_table_page(struct idw_dma_page *page)
{
    if (!page)
        return -EINVAL;
    if (faults_.page_table_fail_at >= 0 &&
        static_cast<size_t>(faults_.page_table_fail_at) ==
            page_tables_.size())
        return -ENOMEM;
    struct idw_dma_test_pt request = {};
    request.size = sizeof(request);
    int ret = ioctl_call(fd_, IDW_DMA_TEST_IOCTL_ALLOC_PT, &request);
    if (ret)
        return ret;
    if (request.bytes != kPageSize ||
        !request.dma_address || !request.cookie ||
        (request.dma_address % kPageSize) ||
        request.dma_address > (1ULL << 40) - kPageSize) {
        struct idw_dma_test_pt release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE_PT, &release);
        return -EIO;
    }
    if (scope_.device_cookie &&
        (scope_.device_cookie != request.device_cookie ||
         scope_.generation != request.generation)) {
        struct idw_dma_test_pt release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE_PT, &release);
        return -EXDEV;
    }

    void *address = ::mmap(
        nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
        static_cast<off_t>((IDW_DMA_TEST_PT_PGOFF_BASE + request.cookie) *
                           kPageSize));
    if (address == MAP_FAILED) {
        struct idw_dma_test_pt release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE_PT, &release);
        return -errno;
    }
    if (faults_.mmap_fail) {
        ::munmap(address, kPageSize);
        struct idw_dma_test_pt release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE_PT, &release);
        return -ENOMEM;
    }
    PageTable table{};
    table.cookie = request.cookie;
    table.device_cookie = request.device_cookie;
    table.generation = request.generation;
    table.dma_address = request.dma_address;
    table.bytes = request.bytes;
    table.address = address;
    table.unmapped = false;
    table.quarantined = false;
    if (!add_counter(counters_.pt_pages, 1)) {
        ::munmap(address, kPageSize);
        struct idw_dma_test_pt release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE_PT, &release);
        return -EOVERFLOW;
    }
    try {
        page_tables_.push_back(table);
    } catch (const std::bad_alloc&) {
        (void)sub_counter(counters_.pt_pages, 1);
        ::munmap(address, kPageSize);
        struct idw_dma_test_pt release = {};
        release.size = sizeof(release);
        release.cookie = request.cookie;
        (void)ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE_PT, &release);
        return -ENOMEM;
    }

    page->size = sizeof(*page);
    page->memory_type = IDW_PT_PAGE_COHERENT;
    page->cpu_address = address;
    page->dma_address = request.dma_address;
    page->bytes = request.bytes;
    page->scope.device_cookie = request.device_cookie;
    page->scope.generation = request.generation;
    page->allocation_cookie = request.cookie;
    return 0;
}

void NativeDmaMemoryBackend::free_page_table_page_cb(
    void *context, struct idw_dma_page *page)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(context);
    self->free_page_table_page(page);
}

void NativeDmaMemoryBackend::free_page_table_page(struct idw_dma_page *page)
{
    if (!page)
        return;
    PageTable *table = find_page_table(page->allocation_cookie);
    if (!table)
        return;
    if (faults_.free_page_table_fail) {
        if (!table->quarantined) {
            table->quarantined = true;
            (void)add_counter(counters_.quarantined_pages, 1);
        }
        return;
    }
    if (!table->unmapped) {
        if (::munmap(table->address, kPageSize) != 0) {
            if (!table->quarantined) {
                table->quarantined = true;
                (void)add_counter(counters_.quarantined_pages, 1);
            }
            return;
        }
        table->unmapped = true;
        table->address = nullptr;
    }
    struct idw_dma_test_pt release = {};
    release.size = sizeof(release);
    release.cookie = table->cookie;
    if (ioctl_call(fd_, IDW_DMA_TEST_IOCTL_FREE_PT, &release) != 0) {
        if (!table->quarantined) {
            table->quarantined = true;
            (void)add_counter(counters_.quarantined_pages, 1);
        }
        return;
    }
    (void)sub_counter(counters_.pt_pages, 1);
    if (table->quarantined)
        (void)sub_counter(counters_.quarantined_pages, 1);
    table->released = true;
    page_tables_.erase(page_tables_.begin() + (table - page_tables_.data()));
    std::memset(page, 0, sizeof(*page));
}

int NativeDmaMemoryBackend::release_remaining_page_tables()
{
    while (!page_tables_.empty()) {
        const uint64_t cookie = page_tables_.front().cookie;
        struct idw_dma_page page = {};
        page.size = sizeof(page);
        page.allocation_cookie = cookie;
        free_page_table_page(&page);
        if (find_page_table(cookie))
            return -EBUSY;
    }
    return 0;
}

int NativeDmaMemoryBackend::sync_page_table_for_device_cb(
    void *context, const struct idw_dma_page *page, uint32_t offset,
    uint32_t bytes)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(context);
    return self->sync_page_table_for_device(page, offset, bytes);
}

int NativeDmaMemoryBackend::sync_page_table_for_device(
    const struct idw_dma_page *page, uint32_t offset, uint32_t bytes)
{
    if (!page || offset > page->bytes || bytes > page->bytes - offset)
        return -EINVAL;
    if (!bytes)
        return 0;
    if (faults_.sync_page_table_fail || faults_.unbind_fail_sync)
        return -EIO;
    struct idw_dma_test_pt request = {};
    request.size = sizeof(request);
    request.cookie = page->allocation_cookie;
    return ioctl_call(fd_, IDW_DMA_TEST_IOCTL_SYNC_PT, &request);
}

int NativeDmaMemoryBackend::alloc_directory_cb(
    void *context, struct idw_hsw_ppgtt_directory *directory)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(context);
    return self->alloc_directory(directory);
}

int NativeDmaMemoryBackend::alloc_directory(
    struct idw_hsw_ppgtt_directory *directory)
{
    if (!directory)
        return -EINVAL;
    void *address = ::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED)
        return -errno;
    directory_.address = address;
    directory_.ggtt_offset = kDirectoryGgttOffset;
    directory_.bytes = kPageSize;
    directory_.cookie = 1;
    directory->ggtt_offset = directory_.ggtt_offset;
    directory->bytes = directory_.bytes;
    directory->scope = scope_;
    directory->allocation_cookie = directory_.cookie;
    return 0;
}

void NativeDmaMemoryBackend::free_directory_cb(
    void *context, struct idw_hsw_ppgtt_directory *directory)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(context);
    self->free_directory(directory);
}

void NativeDmaMemoryBackend::free_directory(
    struct idw_hsw_ppgtt_directory *directory)
{
    (void)directory;
    if (directory_.address) {
        ::munmap(directory_.address, kPageSize);
        directory_.address = nullptr;
    }
    directory_ = {};
}

int NativeDmaMemoryBackend::write_pde_gttaddr_cb(
    void *context, const struct idw_hsw_ppgtt_directory *directory,
    uint32_t index, uint32_t pde)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(context);
    return self->write_pde_gttaddr(directory, index, pde);
}

int NativeDmaMemoryBackend::write_pde_gttaddr(
    const struct idw_hsw_ppgtt_directory *directory, uint32_t index,
    uint32_t pde)
{
    (void)directory;
    if (!directory_.address || index >= kPdes)
        return -EINVAL;
    static_cast<uint32_t *>(directory_.address)[index] = pde;
    return 0;
}

int NativeDmaMemoryBackend::read_pde_gttaddr_cb(
    void *context, const struct idw_hsw_ppgtt_directory *directory,
    uint32_t index, uint32_t *pde)
{
    auto *self = static_cast<NativeDmaMemoryBackend *>(context);
    return self->read_pde_gttaddr(directory, index, pde);
}

int NativeDmaMemoryBackend::read_pde_gttaddr(
    const struct idw_hsw_ppgtt_directory *directory, uint32_t index,
    uint32_t *pde)
{
    (void)directory;
    if (!directory_.address || !pde || index >= kPdes)
        return -EINVAL;
    *pde = static_cast<const uint32_t *>(directory_.address)[index];
    return 0;
}

int NativeDmaMemoryBackend::flush_gtt_cb(
    void *context, const struct idw_hsw_ppgtt_directory *directory)
{
    (void)context;
    (void)directory;
    return 0;
}

int NativeDmaMemoryBackend::close_all()
{
    if (!allocations_.empty())
        return -EBUSY;
    if (model_) {
        struct idw_hsw_ppgtt_pending_invalidation pending = {};
        pending.size = sizeof(pending);
        int ret = idw_hsw_ppgtt_pending(model_, &pending);
        if (ret)
            return ret;
        if (pending.pending) {
            ret = idw_hsw_ppgtt_invalidation_complete(model_,
                                                      pending.generation);
            if (ret)
                return ret;
        }
        ret = check_inactive();
        if (ret)
            return ret;
        ret = idw_hsw_ppgtt_model_destroy_checked(model_, false);
        if (ret)
            return ret;
        model_ = nullptr;
    }
    if (!page_tables_.empty()) {
        int ret = release_remaining_page_tables();
        if (ret)
            return ret;
    }
    if (counters_.allocated_pages || counters_.mapped_pages ||
        counters_.populated_ptes || counters_.pt_pages ||
        counters_.va_reservations || counters_.quarantined_pages)
        return -EBUSY;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    scope_ = {};
    return 0;
}

int NativeDmaMemoryBackend::translate(idw_gpuva va,
                                      uint64_t& dma_address) const
{
    if (!model_)
        return -ENODEV;
    struct idw_hsw_ppgtt_translation translation = {};
    translation.size = sizeof(translation);
    int ret = idw_hsw_ppgtt_translate(model_, va, &translation);
    if (ret)
        return ret;
    dma_address = translation.dma_address;
    return 0;
}

int NativeDmaMemoryBackend::resource_dma(uint64_t backing, uint64_t offset,
                                         uint64_t& dma_address) const
{
    const Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    if (offset >= allocation->bytes)
        return -EINVAL;
    const uint64_t page_offset = offset % kPageSize;
    const uint64_t page_start = offset - page_offset;

    std::vector<idw_dma_test_segment> segments(1);
    struct idw_dma_test_segments request = {};
    request.size = sizeof(request);
    request.capacity = 1;
    request.cookie = allocation->backing;
    request.offset = page_start;
    request.bytes = kPageSize;
    request.segments = reinterpret_cast<uintptr_t>(segments.data());
    int ret = ioctl_call(fd_, IDW_DMA_TEST_IOCTL_SEGMENTS, &request);
    if (ret)
        return ret;
    if (request.count != 1)
        return -EIO;
    dma_address = segments[0].dma_address + page_offset;
    return 0;
}

int NativeDmaMemoryBackend::allocation_at(size_t index, uint64_t& backing,
                                          uint64_t& bytes,
                                          uint64_t& cookie) const
{
    if (index >= allocations_.size())
        return -EINVAL;
    backing = allocations_[index].backing;
    bytes = allocations_[index].bytes;
    cookie = allocations_[index].backing;
    return 0;
}

size_t NativeDmaMemoryBackend::allocation_count() const
{
    return allocations_.size();
}

size_t NativeDmaMemoryBackend::page_table_count() const
{
    return page_tables_.size();
}

NativeDmaMemoryBackend::OwnershipCounters
NativeDmaMemoryBackend::counters() const
{
    return counters_;
}

int NativeDmaMemoryBackend::allocation_state(uint64_t backing,
                                              ResourceState& state) const
{
    const Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    state = allocation->state;
    return 0;
}

int NativeDmaMemoryBackend::teardown_progress(
    uint64_t backing, TeardownProgress& progress) const
{
    const Allocation *allocation = find_allocation(backing);
    if (!allocation)
        return -ENOENT;
    progress.ptes_cleared = allocation->ptes_cleared;
    progress.ptes_accounted = allocation->ptes_accounted;
    progress.invalidation_done = allocation->invalidation_done;
    progress.va_released = allocation->va_released;
    progress.dma_unmapped = allocation->dma_unmapped;
    progress.backing_released = allocation->backing_released;
    progress.complete = allocation->state == ResourceState::Released;
    return 0;
}

void NativeDmaMemoryBackend::set_fault_plan(const FaultPlan& plan)
{
    faults_ = plan;
}

} // namespace idw
