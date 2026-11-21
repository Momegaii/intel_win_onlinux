/* SPDX-License-Identifier: MIT */
#ifndef IDW_NATIVE_DMA_BACKEND_HPP
#define IDW_NATIVE_DMA_BACKEND_HPP

#include "haswell_ppgtt_model.h"
#include "memory_manager.hpp"
#include "../uapi/idw_dma_test.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace idw {

/*
 * Test-only MemoryManager backend for the QEMU EDU DMA module. Resource pages
 * and page-table pages are real kernel allocations with real DMA mappings.
 * The page directory is a simulated GGTT page; no Intel GPU is bound and no
 * hardware page walk occurs.
 */
class NativeDmaMemoryBackend {
public:
    enum class ResourceState : uint32_t {
        New = 1,
        VaReserved,
        BackingAllocated,
        DmaMapped,
        PgttPopulated,
        PtSynchronized,
        ReadyInactive,
        Destroying,
        Quarantined,
        Released,
    };

    struct OwnershipCounters {
        uint64_t allocated_pages = 0;
        uint64_t mapped_pages = 0;
        uint64_t populated_ptes = 0;
        uint64_t pt_pages = 0;
        uint64_t va_reservations = 0;
        uint64_t quarantined_pages = 0;
    };

    struct TeardownProgress {
        bool ptes_cleared = false;
        bool ptes_accounted = false;
        bool invalidation_done = false;
        bool va_released = false;
        bool dma_unmapped = false;
        bool backing_released = false;
        bool complete = false;
    };

    struct FaultPlan {
        int create_fail_at = -1;
        int page_table_fail_at = -1;
        bool sync_page_table_fail = false;
        bool bind_fail_after_map = false;
        bool unbind_fail_invalidation = false;
        bool unbind_fail_sync = false;
        bool destroy_fail = false;
        bool destroy_fail_after_unmap = false;
        bool free_page_table_fail = false;
        bool mmap_fail = false;
    };

    explicit NativeDmaMemoryBackend(
        const char *device_path = IDW_DMA_TEST_DEVICE);
    NativeDmaMemoryBackend(const NativeDmaMemoryBackend&) = delete;
    NativeDmaMemoryBackend& operator=(const NativeDmaMemoryBackend&) = delete;
    ~NativeDmaMemoryBackend();

    MemoryBackendOps operations();

    int close_all();

    int translate(idw_gpuva va, uint64_t& dma_address) const;
    int resource_dma(uint64_t backing, uint64_t offset,
                     uint64_t& dma_address) const;
    int allocation_at(size_t index, uint64_t& backing, uint64_t& bytes,
                      uint64_t& cookie) const;

    size_t allocation_count() const;
    size_t page_table_count() const;
    OwnershipCounters counters() const;
    int allocation_state(uint64_t backing, ResourceState& state) const;
    int teardown_progress(uint64_t backing, TeardownProgress& progress) const;
    void set_fault_plan(const FaultPlan& plan);

private:
    struct Allocation {
        uint64_t backing;
        uint64_t device_cookie;
        uint64_t generation;
        uint64_t bytes;
        void *address;
        bool bound;
        bool ptes_cleared;
        bool ptes_accounted;
        bool invalidation_done;
        bool va_released;
        bool dma_unmapped;
        bool backing_released;
        bool release_safe;
        idw_gpuva va;
        ResourceState state = ResourceState::New;
        uint32_t page_count = 0;
    };

    struct PageTable {
        uint64_t cookie;
        uint64_t device_cookie;
        uint64_t generation;
        uint64_t dma_address;
        uint64_t bytes;
        void *address;
        bool unmapped = false;
        bool quarantined = false;
        bool released = false;
    };

    struct Directory {
        void *address;
        uint64_t ggtt_offset;
        uint64_t bytes;
        uint64_t cookie;
    };

    int open_device();
    int ensure_model();
    int get_segments(uint64_t backing,
                     std::vector<idw_dma_page_mapping>& mappings) const;
    Allocation *find_allocation(uint64_t backing);
    const Allocation *find_allocation(uint64_t backing) const;
    PageTable *find_page_table(uint64_t cookie);
    bool transition(Allocation& allocation, ResourceState next);
    void quarantine(Allocation& allocation);
    int check_inactive() const;
    int complete_unbind(Allocation& allocation);
    static bool add_counter(uint64_t& counter, uint64_t amount);
    static bool sub_counter(uint64_t& counter, uint64_t amount);
    int create_backing_cb_impl(uint64_t bytes, uint64_t *backing);
    int destroy_backing_cb_impl(uint64_t backing);
    int bind_cb_impl(uint64_t backing, idw_gpuva requested_va, uint64_t bytes,
                     idw_gpuva *actual_va);
    int unbind_cb_impl(uint64_t backing, idw_gpuva va, uint64_t bytes);
    int invalidate_cb_impl(idw_gpuva va, uint64_t bytes);
    int submit_cb_impl(uint64_t backing, idw_gpuva va, uint64_t bytes,
                       const MemorySubmissionCommand& command,
                       uint64_t *fence);
    int wait_cb_impl(uint64_t fence);
    int cpu_read_dword_cb_impl(uint64_t backing, uint64_t offset,
                               uint32_t *value);
    int cpu_write_dword_cb_impl(uint64_t backing, uint64_t offset,
                                uint32_t value);
    int alloc_page_table_page(struct idw_dma_page *page);
    void free_page_table_page(struct idw_dma_page *page);
    int release_remaining_page_tables();
    int sync_page_table_for_device(const struct idw_dma_page *page,
                                   uint32_t offset, uint32_t bytes);
    int alloc_directory(struct idw_hsw_ppgtt_directory *directory);
    void free_directory(struct idw_hsw_ppgtt_directory *directory);
    int write_pde_gttaddr(const struct idw_hsw_ppgtt_directory *directory,
                          uint32_t index, uint32_t pde);
    int read_pde_gttaddr(const struct idw_hsw_ppgtt_directory *directory,
                         uint32_t index, uint32_t *pde);
    int flush_gtt(const struct idw_hsw_ppgtt_directory *directory);

    static int create_backing_cb(void *userdata, uint64_t bytes,
                                 uint64_t *backing);
    static int destroy_backing_cb(void *userdata, uint64_t backing);
    static int bind_cb(void *userdata, uint64_t backing, idw_gpuva requested_va,
                       uint64_t bytes, idw_gpuva *actual_va);
    static int unbind_cb(void *userdata, uint64_t backing, idw_gpuva va,
                         uint64_t bytes);
    static int invalidate_cb(void *userdata, idw_gpuva va, uint64_t bytes);
    static int submit_cb(void *userdata, uint64_t backing, idw_gpuva va,
                         uint64_t bytes,
                         const MemorySubmissionCommand& command,
                         uint64_t *fence);
    static int wait_cb(void *userdata, uint64_t fence, int64_t timeout_ns);
    static int quiesce_cb(void *userdata, uint64_t fence);
    static int cpu_read_dword_cb(void *userdata, uint64_t backing,
                                 uint64_t offset, uint32_t *value);
    static int cpu_write_dword_cb(void *userdata, uint64_t backing,
                                  uint64_t offset, uint32_t value);

    static int alloc_page_table_page_cb(void *context,
                                        struct idw_dma_page *page);
    static void free_page_table_page_cb(void *context,
                                        struct idw_dma_page *page);
    static int sync_page_table_for_device_cb(void *context,
                                             const struct idw_dma_page *page,
                                             uint32_t offset, uint32_t bytes);
    static int alloc_directory_cb(void *context,
                                  struct idw_hsw_ppgtt_directory *directory);
    static void free_directory_cb(void *context,
                                  struct idw_hsw_ppgtt_directory *directory);
    static int write_pde_gttaddr_cb(
        void *context, const struct idw_hsw_ppgtt_directory *directory,
        uint32_t index, uint32_t pde);
    static int read_pde_gttaddr_cb(
        void *context, const struct idw_hsw_ppgtt_directory *directory,
        uint32_t index, uint32_t *pde);
    static int flush_gtt_cb(void *context,
                            const struct idw_hsw_ppgtt_directory *directory);

    std::string device_path_;
    int fd_;
    idw_device_scope scope_;
    uint64_t next_fence_;
    struct idw_hsw_ppgtt_model *model_;
    Directory directory_;
    std::vector<Allocation> allocations_;
    std::vector<PageTable> page_tables_;
    OwnershipCounters counters_;
    FaultPlan faults_;
};

} // namespace idw

#endif
