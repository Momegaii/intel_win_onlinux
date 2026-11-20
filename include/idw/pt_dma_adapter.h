/* SPDX-License-Identifier: MIT */
#ifndef IDW_PT_DMA_ADAPTER_H
#define IDW_PT_DMA_ADAPTER_H

#include "contracts/page_table_memory.h"
#include "haswell_ppgtt_model.h"

#define IDW_PT_DMA_PROVIDER_VERSION 1u

/*
 * Injectable DMA page provider. It must return zeroed, 4 KiB-aligned coherent
 * or streaming pages whose DMA address is below 2^40 and whose scope matches
 * the adapter's device scope. free_page returns zero only after the page can no
 * longer be reached by the device.
 */
struct idw_pt_dma_provider_ops {
    uint32_t size;
    uint32_t version;
    int (*alloc_page_table_page)(void *context, struct idw_dma_page *page);
    int (*free_page_table_page)(void *context, struct idw_dma_page *page);
    int (*sync_page_for_device)(void *context,
                                const struct idw_dma_page *page,
                                uint32_t offset, uint32_t bytes);
};

/*
 * The adapter is storage-only: it does not own GPU VA ranges, allocation
 * backing, residency, invalidation, or VM activation. It is not a complete
 * native DMA backend and advertises only IDW_PT_BACKEND_PAGE_TABLE_ONLY.
 * Callers must zero-initialize the adapter and release every page before fini.
 */
struct idw_pt_dma_adapter {
    struct idw_pt_dma_provider_ops ops;
    void *context;
    struct idw_device_scope scope;
    uint32_t outstanding;
    uint32_t active;
};

int idw_pt_dma_adapter_init(struct idw_pt_dma_adapter *adapter,
                            const struct idw_pt_dma_provider_ops *ops,
                            void *context,
                            const struct idw_device_scope *scope);
int idw_pt_dma_adapter_fini(struct idw_pt_dma_adapter *adapter);
int idw_pt_dma_adapter_validate_page(const struct idw_pt_dma_adapter *adapter,
                                     const struct idw_dma_page *page);
int idw_pt_dma_adapter_release_page(struct idw_pt_dma_adapter *adapter,
                                    struct idw_dma_page *page);
void idw_pt_dma_adapter_memory_ops(const struct idw_pt_dma_adapter *adapter,
                                   struct idw_pt_memory_ops *ops);
void *idw_pt_dma_adapter_memory_context(struct idw_pt_dma_adapter *adapter);
int idw_pt_dma_adapter_backend(const struct idw_pt_dma_adapter *adapter,
                               struct idw_pt_backend_desc *backend);
int idw_pt_dma_adapter_page_ops(struct idw_pt_dma_adapter *adapter,
                                struct idw_hsw_ppgtt_page_ops *page_ops);

#endif
