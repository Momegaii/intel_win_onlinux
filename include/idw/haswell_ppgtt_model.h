/* SPDX-License-Identifier: MIT */
#ifndef IDW_HASWELL_PPGTT_MODEL_H
#define IDW_HASWELL_PPGTT_MODEL_H

#include "contracts/haswell_ppgtt.h"
#include "haswell_ppgtt_format.h"

#ifndef __KERNEL__
#include <stdbool.h>
#endif

#define IDW_HSW_PPGTT_PDES 512u
#define IDW_HSW_PPGTT_DIRECTORY_BYTES IDW_HASWELL_PPGTT_PAGE_SIZE
#define IDW_HSW_PPGTT_DIRECTORY_ALIGN IDW_HASWELL_PPGTT_PAGE_SIZE
#define IDW_HSW_PPGTT_DIRECTORY_RECOMMENDED_ALIGN (64u * 1024u)
#define IDW_HSW_PPGTT_DIRECTORY_VERSION 1u

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
 * Hardware-visible page directory storage. The provider owns a 4 KiB,
 * 4 KiB-aligned region inside the GGTT aperture. The model keeps its own CPU
 * shadow array; it must never treat that shadow as the hardware directory.
 * PDE writes and reads go through GTTADDR callbacks.
 */
struct idw_hsw_ppgtt_directory {
    uint32_t size;
    uint32_t version;
    uint64_t ggtt_offset;
    uint64_t bytes;
    struct idw_device_scope scope;
    uint64_t allocation_cookie;
};

struct idw_hsw_ppgtt_directory_ops {
    uint32_t size;
    uint32_t version;
    struct idw_device_scope scope;
    int (*alloc_directory)(void *context,
                           struct idw_hsw_ppgtt_directory *directory);
    void (*free_directory)(void *context,
                           struct idw_hsw_ppgtt_directory *directory);
    int (*write_pde_gttaddr)(void *context,
                             const struct idw_hsw_ppgtt_directory *directory,
                             uint32_t index, uint32_t pde);
    int (*read_pde_gttaddr)(void *context,
                            const struct idw_hsw_ppgtt_directory *directory,
                            uint32_t index, uint32_t *pde);
    int (*flush_gtt)(void *context,
                     const struct idw_hsw_ppgtt_directory *directory);
    void *context;
};

/*
 * Software model only. Callers serialize all operations. The page provider
 * supplies aligned CPU memory and a DMA address below 2^40 for page tables.
 * The directory provider supplies GGTT-resident storage; its scope must match
 * the page provider. Tests use synthetic scope/address tokens, which are not
 * hardware mappings.
 *
 * The model never removes a PDE on unmap. Empty page tables remain reachable
 * until idw_hsw_ppgtt_reclaim_empty() is called with the hardware quiesced and
 * no invalidation outstanding. Reclaim points the PDE at a zeroed scratch page
 * table and retires the old table; the old table is freed only after the
 * resulting invalidation completes.
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
    const struct idw_hsw_ppgtt_directory_ops *directory_ops,
    struct idw_hsw_ppgtt_model **model);
/*
 * Checked teardown refuses while the model is active or an invalidation is
 * outstanding unless hardware_quiesced is true. Set hardware_quiesced only
 * after a device reset or confirmed loss of hardware access; it permits
 * releasing tables that were retired by a failed invalidation. The void
 * wrapper passes false and leaves the model allocated if teardown refuses.
 */
int idw_hsw_ppgtt_model_destroy_checked(struct idw_hsw_ppgtt_model *model,
                                        bool hardware_quiesced);
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

/*
 * The caller asserts hardware quiescence. Reclaim is rejected while the model
 * is marked active or while an invalidation is outstanding.
 */
int idw_hsw_ppgtt_set_active(struct idw_hsw_ppgtt_model *model, bool active);
int idw_hsw_ppgtt_is_active(const struct idw_hsw_ppgtt_model *model);
int idw_hsw_ppgtt_reclaim_empty(struct idw_hsw_ppgtt_model *model);

/* CPU shadow accessor, not a hardware read. */
uint32_t idw_hsw_ppgtt_pde(const struct idw_hsw_ppgtt_model *model,
                           uint32_t pde_index);
int idw_hsw_ppgtt_hardware_pde(const struct idw_hsw_ppgtt_model *model,
                               uint32_t pde_index, uint32_t *pde);
uint64_t idw_hsw_ppgtt_directory_ggtt_offset(
    const struct idw_hsw_ppgtt_model *model);

#endif
