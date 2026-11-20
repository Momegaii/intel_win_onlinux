// SPDX-License-Identifier: GPL-2.0
/*
 * Binds the kernel coherent-DMA page provider to the storage-only page-table
 * adapter. It does not publish page tables, invalidate translations, activate a
 * VM, or transfer GPU-VA/backing ownership to the page-table layer.
 */
#include "idw_dma_pt_adapter.h"

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/string.h>

static int dma_pt_alloc(void *context, struct idw_dma_page *page)
{
    struct idw_dma_provider *provider = context;
    struct idw_dma_page_info info = {};
    int ret;

    if (!provider || !page)
        return -EINVAL;
    ret = idw_dma_page_alloc(provider, &info);
    if (ret)
        return ret;

    page->size = sizeof(*page);
    page->memory_type = IDW_PT_PAGE_COHERENT;
    page->cpu_address = info.cpu_address;
    page->dma_address = info.dma_address;
    page->bytes = info.bytes;
    page->scope.device_cookie = info.device_cookie;
    page->scope.generation = info.generation;
    page->allocation_cookie = info.allocation_cookie;
    return 0;
}

static int dma_pt_free(void *context, struct idw_dma_page *page)
{
    struct idw_dma_provider *provider = context;
    struct idw_dma_page_info info;

    if (!provider || !page)
        return -EINVAL;
    info.cpu_address = page->cpu_address;
    info.dma_address = page->dma_address;
    info.bytes = page->bytes;
    info.device_cookie = page->scope.device_cookie;
    info.generation = page->scope.generation;
    info.allocation_cookie = page->allocation_cookie;
    return idw_dma_page_free(provider, &info);
}

static int dma_pt_sync(void *context, const struct idw_dma_page *page,
                       u32 offset, u32 bytes)
{
    struct idw_dma_provider *provider = context;

    if (!provider || !provider->dev || !page || !bytes)
        return -EINVAL;
    dma_wmb();
    dma_sync_single_for_device(provider->dev,
                               (dma_addr_t)(page->dma_address + offset),
                               bytes, DMA_TO_DEVICE);
    return 0;
}

int idw_dma_pt_adapter_init(struct idw_dma_pt_adapter *owner,
                            struct idw_dma_provider *provider)
{
    const struct idw_pt_dma_provider_ops ops = {
        .size = sizeof(ops),
        .version = IDW_PT_DMA_PROVIDER_VERSION,
        .alloc_page_table_page = dma_pt_alloc,
        .free_page_table_page = dma_pt_free,
        .sync_page_for_device = dma_pt_sync
    };
    struct idw_device_scope scope;
    int ret;

    if (!owner || !provider)
        return -EINVAL;
    if (!provider->active || !provider->dev ||
        !provider->device_cookie || !provider->generation)
        return -ENODEV;

    scope.device_cookie = provider->device_cookie;
    scope.generation = provider->generation;
    ret = idw_pt_dma_adapter_init(&owner->adapter, &ops, provider, &scope);
    if (ret)
        return ret;
    owner->provider = provider;
    return 0;
}

int idw_dma_pt_adapter_fini(struct idw_dma_pt_adapter *owner)
{
    int ret;

    if (!owner)
        return -EINVAL;
    ret = idw_pt_dma_adapter_fini(&owner->adapter);
    if (!ret)
        owner->provider = NULL;
    return ret;
}

int idw_dma_pt_adapter_validate_page(const struct idw_dma_pt_adapter *owner,
                                     const struct idw_dma_page *page)
{
    if (!owner)
        return -EINVAL;
    return idw_pt_dma_adapter_validate_page(&owner->adapter, page);
}

int idw_dma_pt_adapter_release_page(struct idw_dma_pt_adapter *owner,
                                    struct idw_dma_page *page)
{
    if (!owner)
        return -EINVAL;
    return idw_pt_dma_adapter_release_page(&owner->adapter, page);
}

int idw_dma_pt_adapter_sync_for_cpu(struct idw_dma_pt_adapter *owner,
                                    const struct idw_dma_page *page,
                                    u32 offset, u32 bytes)
{
    int ret;

    if (!owner || !owner->provider || !owner->provider->dev)
        return -EINVAL;
    ret = idw_dma_pt_adapter_validate_page(owner, page);
    if (ret)
        return ret;
    if (offset > page->bytes || bytes > page->bytes - offset)
        return -EINVAL;
    if (!bytes)
        return 0;
    dma_sync_single_for_cpu(owner->provider->dev,
                            (dma_addr_t)(page->dma_address + offset),
                            bytes, DMA_FROM_DEVICE);
    dma_rmb();
    return 0;
}

u32 idw_dma_pt_adapter_outstanding(const struct idw_dma_pt_adapter *owner)
{
    if (!owner)
        return 0;
    return owner->adapter.outstanding;
}

void idw_dma_pt_adapter_memory_ops(const struct idw_dma_pt_adapter *owner,
                                   struct idw_pt_memory_ops *ops)
{
    if (!owner || !ops)
        return;
    idw_pt_dma_adapter_memory_ops(&owner->adapter, ops);
}

void *idw_dma_pt_adapter_memory_context(struct idw_dma_pt_adapter *owner)
{
    if (!owner)
        return NULL;
    return idw_pt_dma_adapter_memory_context(&owner->adapter);
}

int idw_dma_pt_adapter_backend(const struct idw_dma_pt_adapter *owner,
                               struct idw_pt_backend_desc *backend)
{
    if (!owner)
        return -EINVAL;
    return idw_pt_dma_adapter_backend(&owner->adapter, backend);
}

int idw_dma_pt_adapter_page_ops(struct idw_dma_pt_adapter *owner,
                                struct idw_hsw_ppgtt_page_ops *page_ops)
{
    if (!owner)
        return -EINVAL;
    return idw_pt_dma_adapter_page_ops(&owner->adapter, page_ops);
}
