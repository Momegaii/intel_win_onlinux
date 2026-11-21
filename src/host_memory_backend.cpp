/* SPDX-License-Identifier: MIT */
#include "../include/idw/host_memory_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/memfd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace idw {
namespace {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kMaxBackingBytes = 256ULL * 1024 * 1024;

int create_memfd()
{
#ifdef SYS_memfd_create
    return static_cast<int>(
        ::syscall(SYS_memfd_create, "idw-host-backing", MFD_CLOEXEC));
#else
    errno = ENOSYS;
    return -1;
#endif
}

} // namespace

HostMemoryBackend::HostMemoryBackend()
    : allocations_(), next_backing_(1), next_fence_(1)
{
}

HostMemoryBackend::~HostMemoryBackend()
{
    if (close_all() != 0)
        std::fprintf(stderr,
                     "idw: host memory backend cleanup was incomplete\n");
}

MemoryBackendOps HostMemoryBackend::operations() const
{
    return {create_backing, destroy_backing, bind, unbind, invalidate,
            submit, wait, quiesce, cpu_read_dword, cpu_write_dword};
}

int HostMemoryBackend::find_allocation(uint64_t backing, size_t& index) const
{
    for (size_t i = 0; i < allocations_.size(); ++i) {
        if (allocations_[i].id == backing) {
            index = i;
            return 0;
        }
    }
    return -ENOENT;
}

int HostMemoryBackend::create_backing(void *userdata, uint64_t bytes,
                                      uint64_t *backing)
{
    if (!userdata || !backing)
        return -EINVAL;
    auto& self = *static_cast<HostMemoryBackend *>(userdata);
    *backing = 0;
    if (!bytes || (bytes & (kPageSize - 1)) || bytes > kMaxBackingBytes ||
        bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return -EINVAL;
    if (self.next_backing_ == 0 ||
        self.allocations_.size() == self.allocations_.max_size())
        return -ENOSPC;
    try {
        self.allocations_.reserve(self.allocations_.size() + 1);
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    }

    const int fd = create_memfd();
    if (fd < 0)
        return -errno;
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        const int ret = -errno;
        (void)close(fd);
        return ret;
    }
    void *address = mmap(nullptr, static_cast<size_t>(bytes),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        const int ret = -errno;
        (void)close(fd);
        return ret;
    }

    /* Fault every page so the allocation is real, zeroed host memory now. */
    std::memset(address, 0, static_cast<size_t>(bytes));

    const uint64_t id = self.next_backing_++;
    self.allocations_.push_back({id, fd, address, bytes, 0, false});
    *backing = id;
    return 0;
}

int HostMemoryBackend::release_allocation(size_t index)
{
    Allocation& allocation = allocations_[index];
    if (allocation.bound)
        return -EBUSY;
    if (munmap(allocation.address, static_cast<size_t>(allocation.bytes)) != 0)
        return -errno;
    (void)close(allocation.fd);
    allocations_.erase(allocations_.begin() +
                       static_cast<std::ptrdiff_t>(index));
    return 0;
}

int HostMemoryBackend::destroy_backing(void *userdata, uint64_t backing)
{
    if (!userdata || !backing)
        return -EINVAL;
    auto& self = *static_cast<HostMemoryBackend *>(userdata);
    size_t index = 0;
    const int ret = self.find_allocation(backing, index);
    if (ret)
        return ret;
    return self.release_allocation(index);
}

int HostMemoryBackend::bind(void *userdata, uint64_t backing,
                            idw_gpuva requested_va, uint64_t bytes,
                            idw_gpuva *actual_va)
{
    if (!userdata || !actual_va)
        return -EINVAL;
    auto& self = *static_cast<HostMemoryBackend *>(userdata);
    size_t index = 0;
    const int ret = self.find_allocation(backing, index);
    if (ret)
        return ret;
    Allocation& allocation = self.allocations_[index];
    if (allocation.bound)
        return -EBUSY;
    if (!requested_va || (requested_va & (kPageSize - 1)) ||
        bytes != allocation.bytes)
        return -EINVAL;
    allocation.va = requested_va;
    allocation.bound = true;
    *actual_va = requested_va;
    return 0;
}

int HostMemoryBackend::unbind(void *userdata, uint64_t backing,
                              idw_gpuva va, uint64_t bytes)
{
    if (!userdata)
        return -EINVAL;
    auto& self = *static_cast<HostMemoryBackend *>(userdata);
    size_t index = 0;
    const int ret = self.find_allocation(backing, index);
    if (ret)
        return ret;
    Allocation& allocation = self.allocations_[index];
    if (!allocation.bound || allocation.va != va || bytes != allocation.bytes)
        return -EINVAL;
    allocation.va = 0;
    allocation.bound = false;
    return 0;
}

int HostMemoryBackend::invalidate(void *userdata, idw_gpuva va, uint64_t bytes)
{
    if (!userdata || !va || (va & (kPageSize - 1)) || !bytes ||
        (bytes & (kPageSize - 1)))
        return -EINVAL;
    return 0;
}

int HostMemoryBackend::submit(void *userdata, uint64_t backing, idw_gpuva va,
                              uint64_t bytes,
                              const MemorySubmissionCommand& command,
                              uint64_t *fence)
{
    if (!userdata || !fence)
        return -EINVAL;
    auto& self = *static_cast<HostMemoryBackend *>(userdata);
    size_t index = 0;
    const int ret = self.find_allocation(backing, index);
    if (ret)
        return ret;
    const Allocation& allocation = self.allocations_[index];
    if (!allocation.bound || allocation.va != va || bytes != allocation.bytes)
        return -EINVAL;
    if (command.kind == MemorySubmissionKind::WriteDword) {
        if (command.offset > allocation.bytes - sizeof(uint32_t) ||
            (command.offset & (sizeof(uint32_t) - 1)))
            return -EINVAL;
        std::memcpy(static_cast<uint8_t *>(allocation.address) + command.offset,
                    &command.value, sizeof(command.value));
    } else if (command.kind == MemorySubmissionKind::Noop) {
        if (command.offset || command.value)
            return -EINVAL;
    } else {
        return -EINVAL;
    }
    if (self.next_fence_ == UINT64_MAX)
        return -EOVERFLOW;
    *fence = self.next_fence_++;
    return 0;
}

int HostMemoryBackend::wait(void *userdata, uint64_t fence, int64_t timeout_ns)
{
    if (!userdata || !fence || !timeout_ns)
        return -EINVAL;
    const auto& self = *static_cast<HostMemoryBackend *>(userdata);
    return fence < self.next_fence_ ? 0 : -EINVAL;
}

int HostMemoryBackend::quiesce(void *userdata, uint64_t fence)
{
    if (!userdata)
        return -EINVAL;
    const auto& self = *static_cast<HostMemoryBackend *>(userdata);
    return !fence || fence < self.next_fence_ ? 0 : -EINVAL;
}

int HostMemoryBackend::cpu_read_dword(void *userdata, uint64_t backing,
                                      uint64_t offset, uint32_t *value)
{
    if (!userdata || !value)
        return -EINVAL;
    const auto& self = *static_cast<HostMemoryBackend *>(userdata);
    size_t index = 0;
    const int ret = self.find_allocation(backing, index);
    if (ret)
        return ret;
    const Allocation& allocation = self.allocations_[index];
    if (offset > allocation.bytes - sizeof(uint32_t) ||
        (offset & (sizeof(uint32_t) - 1)))
        return -EINVAL;
    std::memcpy(value, static_cast<const uint8_t *>(allocation.address) + offset,
                sizeof(*value));
    return 0;
}

int HostMemoryBackend::cpu_write_dword(void *userdata, uint64_t backing,
                                       uint64_t offset, uint32_t value)
{
    if (!userdata)
        return -EINVAL;
    auto& self = *static_cast<HostMemoryBackend *>(userdata);
    size_t index = 0;
    const int ret = self.find_allocation(backing, index);
    if (ret)
        return ret;
    const Allocation& allocation = self.allocations_[index];
    if (offset > allocation.bytes - sizeof(uint32_t) ||
        (offset & (sizeof(uint32_t) - 1)))
        return -EINVAL;
    std::memcpy(static_cast<uint8_t *>(allocation.address) + offset, &value,
                sizeof(value));
    return 0;
}

int HostMemoryBackend::close_all()
{
    int first_error = 0;
    for (size_t i = 0; i < allocations_.size();) {
        if (allocations_[i].bound) {
            if (!first_error)
                first_error = -EBUSY;
            ++i;
            continue;
        }
        const int ret = release_allocation(i);
        if (ret) {
            if (!first_error)
                first_error = ret;
            ++i;
        }
    }
    return first_error;
}

size_t HostMemoryBackend::allocation_count() const
{
    return allocations_.size();
}

size_t HostMemoryBackend::mapped_count() const
{
    size_t count = 0;
    for (const Allocation& allocation : allocations_) {
        if (allocation.bound)
            ++count;
    }
    return count;
}

uint64_t HostMemoryBackend::bytes_allocated() const
{
    uint64_t bytes = 0;
    for (const Allocation& allocation : allocations_)
        bytes += allocation.bytes;
    return bytes;
}

int HostMemoryBackend::backing_info(uint64_t backing, void **address,
                                    uint64_t *bytes) const
{
    if (!address || !bytes)
        return -EINVAL;
    size_t index = 0;
    const int ret = find_allocation(backing, index);
    if (ret)
        return ret;
    *address = allocations_[index].address;
    *bytes = allocations_[index].bytes;
    return 0;
}

int HostMemoryBackend::backing_at(size_t index, uint64_t *backing,
                                  void **address, uint64_t *bytes) const
{
    if (index >= allocations_.size() || !backing || !address || !bytes)
        return -EINVAL;
    *backing = allocations_[index].id;
    *address = allocations_[index].address;
    *bytes = allocations_[index].bytes;
    return 0;
}

} // namespace idw
