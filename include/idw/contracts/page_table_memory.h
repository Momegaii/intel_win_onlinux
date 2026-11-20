/* SPDX-License-Identifier: MIT */
#ifndef IDW_PAGE_TABLE_MEMORY_CONTRACT_H
#define IDW_PAGE_TABLE_MEMORY_CONTRACT_H

#include "base.h"

#define IDW_PT_MEMORY_VERSION 1u
#define IDW_PT_PAGE_BYTES 4096u

enum idw_pt_backend_kind {
    IDW_PT_BACKEND_I915_ASSISTED = 1,
    IDW_PT_BACKEND_NATIVE_DMA = 2
};

enum idw_pt_backend_flags {
    IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER = 1u << 0,
    /*
     * Explicit partial backend profile: DMA-backed page-table storage only.
     * It must not advertise resource mapping, invalidation, or VM activation.
     * A complete native backend omits this flag.
     */
    IDW_PT_BACKEND_PAGE_TABLE_ONLY = 1u << 1
};

enum idw_pt_backend_capabilities {
    IDW_PT_CAP_GEM_BACKING = 1ULL << 0,
    IDW_PT_CAP_DMABUF_LIFETIME = 1ULL << 1,
    IDW_PT_CAP_CONTEXT_VM_BIND = 1ULL << 2,
    IDW_PT_CAP_I915_EXEC_WAIT = 1ULL << 3,
    IDW_PT_CAP_DMA_PAGE_TABLE = 1ULL << 16,
    IDW_PT_CAP_DMA_RESOURCE_MAPPING = 1ULL << 17,
    IDW_PT_CAP_SYNC_FOR_DEVICE = 1ULL << 18,
    IDW_PT_CAP_GEN75_INVALIDATE = 1ULL << 19,
    IDW_PT_CAP_GEN75_VM_ACTIVATE = 1ULL << 20
};

enum idw_pt_page_memory_type {
    IDW_PT_PAGE_COHERENT = 1,
    IDW_PT_PAGE_STREAMING = 2
};

struct idw_device_scope {
    uint64_t device_cookie;
    uint64_t generation;
};

struct idw_pt_backend_desc {
    uint32_t size;
    uint32_t version;
    uint32_t kind;
    uint32_t flags;
    uint64_t capabilities;
    struct idw_device_scope scope;
};

struct idw_dma_page {
    uint32_t size;
    uint32_t memory_type;
    void *cpu_address;
    uint64_t dma_address;
    uint64_t bytes;
    struct idw_device_scope scope;
    uint64_t allocation_cookie;
};

struct idw_dma_segment {
    uint64_t dma_address;
    uint64_t bytes;
    struct idw_device_scope scope;
    uint64_t mapping_cookie;
};

struct idw_dma_page_mapping {
    uint64_t dma_address;
    uint64_t bytes;
    struct idw_device_scope scope;
    uint64_t mapping_cookie;
};

struct idw_pt_memory_ops {
    uint32_t size;
    uint32_t version;
    int (*alloc_page_table_page)(void *context, struct idw_dma_page *page);
    void (*free_page_table_page)(void *context, struct idw_dma_page *page);
    int (*map_allocation)(void *context, idw_handle allocation,
                          uint64_t allocation_offset, uint64_t bytes,
                          struct idw_dma_segment *segments,
                          uint32_t segment_capacity, uint32_t *segment_count);
    void (*unmap_allocation)(void *context, idw_handle allocation,
                             const struct idw_dma_segment *segments,
                             uint32_t segment_count);
    int (*sync_page_table_for_device)(void *context,
                                      const struct idw_dma_page *page,
                                      uint32_t offset, uint32_t bytes);
};

int idw_pt_backend_validate(const struct idw_pt_backend_desc *backend,
                            uint64_t required_capabilities);
int idw_pt_page_validate(const struct idw_pt_backend_desc *backend,
                         const struct idw_dma_page *page);
int idw_pt_mapping_validate(const struct idw_pt_backend_desc *backend,
                            const struct idw_dma_page_mapping *mapping);
int idw_pt_scope_equal(const struct idw_device_scope *left,
                       const struct idw_device_scope *right);

#endif
