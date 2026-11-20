/* SPDX-License-Identifier: MIT */
#ifndef IDW_HASWELL_PPGTT_MODEL_H
#define IDW_HASWELL_PPGTT_MODEL_H

#include "contracts/haswell_ppgtt.h"
#include "haswell_ppgtt_format.h"

#define IDW_HSW_PPGTT_PDES 512u
#define IDW_HSW_PPGTT_DMA_ADDRESS_BITS 40u

struct idw_hsw_ppgtt_model;

struct idw_hsw_ppgtt_page_ops {
    uint32_t size;
    uint32_t version;
    struct idw_device_scope scope;
    int (*alloc_page_table_page)(void *context, struct idw_dma_page *page);
    void (*free_page_table_page)(void *context, struct idw_dma_page *page);
    void *context;
};

/*
 * Software model only. Callers serialize all operations. The page provider
 * supplies aligned CPU memory and a 40-bit DMA address carrying the same
 * device scope as the provider. Tests use synthetic scope/address tokens,
 * which are not hardware mappings. On success, each page must have unique CPU
 * and DMA addresses and a nonzero allocation cookie. On allocation failure,
 * the provider must release any partial allocation.
 * This model does not hold VidMm allocation references or issue invalidation.
 */
struct idw_hsw_ppgtt_translation {
    uint32_t size;
    uint32_t pde_index;
    uint32_t pte_index;
    uint32_t pde;
    uint32_t pte;
    uint64_t dma_address;
};

struct idw_hsw_ppgtt_pending_invalidation {
    uint32_t size;
    uint32_t pending;
    uint64_t generation;
    idw_gpuva start;
    uint64_t bytes;
};

int idw_hsw_ppgtt_model_create(
    const struct idw_hsw_ppgtt_page_ops *page_ops,
    struct idw_hsw_ppgtt_model **model);
void idw_hsw_ppgtt_model_destroy(struct idw_hsw_ppgtt_model *model);
int idw_hsw_ppgtt_map(struct idw_hsw_ppgtt_model *model,
                      idw_gpuva va, uint64_t bytes,
                      const struct idw_dma_page_mapping *dma_pages,
                      uint64_t page_count);
int idw_hsw_ppgtt_unmap(struct idw_hsw_ppgtt_model *model,
                        idw_gpuva va, uint64_t bytes);
int idw_hsw_ppgtt_translate(const struct idw_hsw_ppgtt_model *model,
                            idw_gpuva va,
                            struct idw_hsw_ppgtt_translation *translation);
int idw_hsw_ppgtt_pending(
    const struct idw_hsw_ppgtt_model *model,
    struct idw_hsw_ppgtt_pending_invalidation *pending);
int idw_hsw_ppgtt_invalidation_complete(
    struct idw_hsw_ppgtt_model *model, uint64_t generation);
int idw_hsw_ppgtt_invalidation_failed(
    struct idw_hsw_ppgtt_model *model, uint64_t generation);
int idw_hsw_ppgtt_check_submit(const struct idw_hsw_ppgtt_model *model);
uint32_t idw_hsw_ppgtt_pde(const struct idw_hsw_ppgtt_model *model,
                           uint32_t pde_index);

#endif
