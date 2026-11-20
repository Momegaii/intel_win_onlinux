/* SPDX-License-Identifier: MIT */
#include "../include/idw/contracts/page_table_memory.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdint.h>
#endif

#define IDW_PT_I915_CAPS (IDW_PT_CAP_GEM_BACKING | IDW_PT_CAP_DMABUF_LIFETIME | \
                          IDW_PT_CAP_CONTEXT_VM_BIND | IDW_PT_CAP_I915_EXEC_WAIT)
#define IDW_PT_NATIVE_CAPS (IDW_PT_CAP_DMA_PAGE_TABLE | \
                            IDW_PT_CAP_DMA_RESOURCE_MAPPING | \
                            IDW_PT_CAP_SYNC_FOR_DEVICE | \
                            IDW_PT_CAP_GEN75_INVALIDATE | \
                            IDW_PT_CAP_GEN75_VM_ACTIVATE)
#define IDW_PT_KNOWN_CAPS (IDW_PT_I915_CAPS | IDW_PT_NATIVE_CAPS)
#define IDW_PT_DMA_LIMIT (1ULL << 40)

int idw_pt_scope_equal(const struct idw_device_scope *left,
                       const struct idw_device_scope *right)
{
    return left && right && left->device_cookie &&
           left->generation && left->device_cookie == right->device_cookie &&
           left->generation == right->generation;
}

int idw_pt_backend_validate(const struct idw_pt_backend_desc *backend,
                            uint64_t required_capabilities)
{
    uint64_t capabilities;

    if (!backend || backend->size != sizeof(*backend) ||
        backend->version != IDW_PT_MEMORY_VERSION ||
        !backend->scope.device_cookie || !backend->scope.generation ||
        (backend->flags & ~(IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER |
                            IDW_PT_BACKEND_PAGE_TABLE_ONLY)))
        return -EINVAL;
    capabilities = backend->capabilities;
    if ((capabilities & ~IDW_PT_KNOWN_CAPS) ||
        (required_capabilities & ~IDW_PT_KNOWN_CAPS))
        return -EINVAL;

    switch (backend->kind) {
    case IDW_PT_BACKEND_I915_ASSISTED:
        if (backend->flags || (capabilities & ~IDW_PT_I915_CAPS))
            return -EPERM;
        break;
    case IDW_PT_BACKEND_NATIVE_DMA:
        if (!(backend->flags & IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER) ||
            (capabilities & ~IDW_PT_NATIVE_CAPS))
            return -EPERM;
        if (backend->flags & IDW_PT_BACKEND_PAGE_TABLE_ONLY) {
            if (capabilities & ~(IDW_PT_CAP_DMA_PAGE_TABLE |
                                 IDW_PT_CAP_SYNC_FOR_DEVICE))
                return -EPERM;
            if (!(capabilities & IDW_PT_CAP_DMA_PAGE_TABLE) ||
                !(capabilities & IDW_PT_CAP_SYNC_FOR_DEVICE))
                return -EINVAL;
            break;
        }
        if ((capabilities & IDW_PT_CAP_DMA_PAGE_TABLE) &&
            (!(capabilities & IDW_PT_CAP_SYNC_FOR_DEVICE) ||
             !(capabilities & IDW_PT_CAP_DMA_RESOURCE_MAPPING)))
            return -EINVAL;
        if ((capabilities & IDW_PT_CAP_GEN75_INVALIDATE) &&
            !(capabilities & IDW_PT_CAP_DMA_PAGE_TABLE))
            return -EINVAL;
        if ((capabilities & IDW_PT_CAP_GEN75_VM_ACTIVATE) &&
            !(capabilities & IDW_PT_CAP_GEN75_INVALIDATE))
            return -EINVAL;
        break;
    default:
        return -EINVAL;
    }

    return (required_capabilities & capabilities) == required_capabilities ?
           0 : -EOPNOTSUPP;
}

int idw_pt_page_validate(const struct idw_pt_backend_desc *backend,
                         const struct idw_dma_page *page)
{
    int ret;

    if (!page || page->size != sizeof(*page) || !page->cpu_address ||
        page->bytes < IDW_PT_PAGE_BYTES ||
        (page->bytes & (IDW_PT_PAGE_BYTES - 1)) ||
        !page->allocation_cookie ||
        (page->memory_type != IDW_PT_PAGE_COHERENT &&
         page->memory_type != IDW_PT_PAGE_STREAMING) ||
        ((uintptr_t)page->cpu_address & (IDW_PT_PAGE_BYTES - 1)) ||
        (page->dma_address & (IDW_PT_PAGE_BYTES - 1)) ||
        page->bytes > IDW_PT_DMA_LIMIT ||
        page->dma_address > IDW_PT_DMA_LIMIT - page->bytes)
        return -EINVAL;
    ret = idw_pt_backend_validate(backend, IDW_PT_CAP_DMA_PAGE_TABLE);
    if (ret)
        return ret;
    if (!idw_pt_scope_equal(&backend->scope, &page->scope))
        return -EXDEV;
    return 0;
}

int idw_pt_mapping_validate(const struct idw_pt_backend_desc *backend,
                            const struct idw_dma_page_mapping *mapping)
{
    int ret;

    if (!mapping || mapping->bytes != IDW_PT_PAGE_BYTES ||
        !mapping->mapping_cookie ||
        (mapping->dma_address & (IDW_PT_PAGE_BYTES - 1)) ||
        mapping->dma_address >= IDW_PT_DMA_LIMIT)
        return -EINVAL;
    ret = idw_pt_backend_validate(backend,
                                  IDW_PT_CAP_DMA_RESOURCE_MAPPING);
    if (ret)
        return ret;
    if (!idw_pt_scope_equal(&backend->scope, &mapping->scope))
        return -EXDEV;
    return 0;
}
