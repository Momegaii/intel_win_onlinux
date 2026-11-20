/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDW_DMA_PT_ADAPTER_H
#define IDW_DMA_PT_ADAPTER_H

#include <linux/types.h>
#include "../include/idw/dma_memory.h"
#include "../include/idw/pt_dma_adapter.h"

/*
 * Kernel binding for the storage-only DMA page-table adapter. The owner must
 * be zero-initialized before init. The provider must remain initialized for
 * the adapter's lifetime; this wrapper does not take a provider or module
 * reference of its own.
 */
struct idw_dma_pt_adapter {
    struct idw_dma_provider *provider;
    struct idw_pt_dma_adapter adapter;
};

int idw_dma_pt_adapter_init(struct idw_dma_pt_adapter *owner,
                            struct idw_dma_provider *provider);
int idw_dma_pt_adapter_fini(struct idw_dma_pt_adapter *owner);
int idw_dma_pt_adapter_validate_page(const struct idw_dma_pt_adapter *owner,
                                     const struct idw_dma_page *page);
int idw_dma_pt_adapter_release_page(struct idw_dma_pt_adapter *owner,
                                    struct idw_dma_page *page);
int idw_dma_pt_adapter_sync_for_cpu(struct idw_dma_pt_adapter *owner,
                                    const struct idw_dma_page *page,
                                    u32 offset, u32 bytes);
u32 idw_dma_pt_adapter_outstanding(const struct idw_dma_pt_adapter *owner);
void idw_dma_pt_adapter_memory_ops(const struct idw_dma_pt_adapter *owner,
                                   struct idw_pt_memory_ops *ops);
void *idw_dma_pt_adapter_memory_context(struct idw_dma_pt_adapter *owner);
int idw_dma_pt_adapter_backend(const struct idw_dma_pt_adapter *owner,
                               struct idw_pt_backend_desc *backend);
int idw_dma_pt_adapter_page_ops(struct idw_dma_pt_adapter *owner,
                                struct idw_hsw_ppgtt_page_ops *page_ops);

#endif
