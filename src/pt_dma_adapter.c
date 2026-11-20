/* SPDX-License-Identifier: MIT */
#include "../include/idw/pt_dma_adapter.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define IDW_PT_DMA_ADDRESS_LIMIT (1ULL << 40)

static int validate_page(const struct idw_pt_dma_adapter *adapter,
                         const struct idw_dma_page *page)
{
    unsigned long cpu_address;

    if (!adapter || !adapter->active)
        return -ENODEV;
    if (!page || page->size != sizeof(*page) || !page->cpu_address ||
        page->bytes < IDW_PT_PAGE_BYTES ||
        (page->bytes & (IDW_PT_PAGE_BYTES - 1)) ||
        !page->allocation_cookie ||
        (page->memory_type != IDW_PT_PAGE_COHERENT &&
         page->memory_type != IDW_PT_PAGE_STREAMING))
        return -EINVAL;
    if (page->bytes > IDW_PT_DMA_ADDRESS_LIMIT ||
        page->dma_address > IDW_PT_DMA_ADDRESS_LIMIT - page->bytes)
        return -ERANGE;
    if ((page->dma_address & (IDW_PT_PAGE_BYTES - 1)) ||
        ((uint64_t)(uintptr_t)page->cpu_address &
         (IDW_PT_PAGE_BYTES - 1)))
        return -EINVAL;
    cpu_address = (unsigned long)page->cpu_address;
    if (page->bytes > (uint64_t)(~(unsigned long)0) - cpu_address)
        return -ERANGE;
    if (!idw_pt_scope_equal(&adapter->scope, &page->scope))
        return -EXDEV;
    return 0;
}

int idw_pt_dma_adapter_validate_page(const struct idw_pt_dma_adapter *adapter,
                                     const struct idw_dma_page *page)
{
    return validate_page(adapter, page);
}

static int adapter_alloc_page(struct idw_pt_dma_adapter *adapter,
                              struct idw_dma_page *page)
{
    struct idw_dma_page candidate = {0};
    int ret;

    if (!adapter || !adapter->active)
        return -ENODEV;
    if (!page)
        return -EINVAL;

    candidate.size = sizeof(candidate);
    ret = adapter->ops.alloc_page_table_page(adapter->context, &candidate);
    if (ret)
        return ret;
    ret = validate_page(adapter, &candidate);
    if (ret) {
        (void)adapter->ops.free_page_table_page(adapter->context, &candidate);
        return ret;
    }

    memset(candidate.cpu_address, 0, candidate.bytes);
    *page = candidate;
    ++adapter->outstanding;
    return 0;
}

int idw_pt_dma_adapter_release_page(struct idw_pt_dma_adapter *adapter,
                                    struct idw_dma_page *page)
{
    int ret;

    ret = validate_page(adapter, page);
    if (ret)
        return ret;
    ret = adapter->ops.free_page_table_page(adapter->context, page);
    if (ret)
        return ret;
    if (adapter->outstanding)
        --adapter->outstanding;
    return 0;
}

static void free_page_callback(void *context, struct idw_dma_page *page)
{
    (void)idw_pt_dma_adapter_release_page(context, page);
}

static int alloc_page_callback(void *context, struct idw_dma_page *page)
{
    return adapter_alloc_page(context, page);
}

static int sync_page_for_device(void *context,
                                const struct idw_dma_page *page,
                                uint32_t offset, uint32_t bytes)
{
    struct idw_pt_dma_adapter *adapter = context;
    int ret;

    ret = validate_page(adapter, page);
    if (ret)
        return ret;
    if (offset > page->bytes || bytes > page->bytes - offset)
        return -EINVAL;
    if (!bytes)
        return 0;
    return adapter->ops.sync_page_for_device(adapter->context, page,
                                             offset, bytes);
}

static int map_allocation(void *context, idw_handle allocation,
                          uint64_t allocation_offset, uint64_t bytes,
                          struct idw_dma_segment *segments,
                          uint32_t segment_capacity, uint32_t *segment_count)
{
    (void)context;
    (void)allocation;
    (void)allocation_offset;
    (void)bytes;
    (void)segments;
    (void)segment_capacity;
    (void)segment_count;
    return -EOPNOTSUPP;
}

static void unmap_allocation(void *context, idw_handle allocation,
                             const struct idw_dma_segment *segments,
                             uint32_t segment_count)
{
    (void)context;
    (void)allocation;
    (void)segments;
    (void)segment_count;
}

int idw_pt_dma_adapter_init(struct idw_pt_dma_adapter *adapter,
                            const struct idw_pt_dma_provider_ops *ops,
                            void *context,
                            const struct idw_device_scope *scope)
{
    if (!adapter || !ops || !context || !scope ||
        !scope->device_cookie || !scope->generation ||
        ops->size != sizeof(*ops) ||
        ops->version != IDW_PT_DMA_PROVIDER_VERSION ||
        !ops->alloc_page_table_page || !ops->free_page_table_page ||
        !ops->sync_page_for_device)
        return -EINVAL;
    if (adapter->active)
        return -EBUSY;

    adapter->ops = *ops;
    adapter->context = context;
    adapter->scope = *scope;
    adapter->outstanding = 0;
    adapter->active = 1;
    return 0;
}

int idw_pt_dma_adapter_fini(struct idw_pt_dma_adapter *adapter)
{
    if (!adapter || !adapter->active)
        return -EINVAL;
    if (adapter->outstanding)
        return -EBUSY;
    memset(adapter, 0, sizeof(*adapter));
    return 0;
}

void idw_pt_dma_adapter_memory_ops(const struct idw_pt_dma_adapter *adapter,
                                   struct idw_pt_memory_ops *ops)
{
    (void)adapter;
    if (!ops)
        return;
    *ops = (struct idw_pt_memory_ops) {
        .size = sizeof(*ops),
        .version = IDW_PT_MEMORY_VERSION,
        .alloc_page_table_page = alloc_page_callback,
        .free_page_table_page = free_page_callback,
        .map_allocation = map_allocation,
        .unmap_allocation = unmap_allocation,
        .sync_page_table_for_device = sync_page_for_device
    };
}

void *idw_pt_dma_adapter_memory_context(struct idw_pt_dma_adapter *adapter)
{
    return adapter;
}

int idw_pt_dma_adapter_backend(const struct idw_pt_dma_adapter *adapter,
                               struct idw_pt_backend_desc *backend)
{
    if (!adapter || !adapter->active || !backend)
        return -EINVAL;

    *backend = (struct idw_pt_backend_desc) {
        .size = sizeof(*backend),
        .version = IDW_PT_MEMORY_VERSION,
        .kind = IDW_PT_BACKEND_NATIVE_DMA,
        .flags = IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER |
                 IDW_PT_BACKEND_PAGE_TABLE_ONLY,
        .capabilities = IDW_PT_CAP_DMA_PAGE_TABLE |
                        IDW_PT_CAP_SYNC_FOR_DEVICE,
        .scope = adapter->scope
    };
    return 0;
}

int idw_pt_dma_adapter_page_ops(struct idw_pt_dma_adapter *adapter,
                                struct idw_hsw_ppgtt_page_ops *page_ops)
{
    if (!adapter || !adapter->active || !page_ops)
        return -EINVAL;

    *page_ops = (struct idw_hsw_ppgtt_page_ops) {
        .size = sizeof(*page_ops),
        .version = 1,
        .scope = adapter->scope,
        .alloc_page_table_page = alloc_page_callback,
        .free_page_table_page = free_page_callback,
        .context = adapter
    };
    return 0;
}
