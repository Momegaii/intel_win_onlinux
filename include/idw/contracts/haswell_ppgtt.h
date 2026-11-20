/* SPDX-License-Identifier: MIT */
#ifndef IDW_HASWELL_PPGTT_CONTRACT_H
#define IDW_HASWELL_PPGTT_CONTRACT_H
#include "page_table_memory.h"

#define IDW_HASWELL_PPGTT_PAGE_SIZE 4096u
#define IDW_HASWELL_PPGTT_PTES_PER_TABLE 1024u
#define IDW_HASWELL_PPGTT_PDE_SHIFT 22u
#define IDW_HASWELL_PPGTT_GEN7_ADDRESS_SPACE_SIZE (1ULL << 31)

enum idw_haswell_ppgtt_features {
    IDW_HASWELL_PPGTT_FEATURE_GEN75 = 1u << 0,
    IDW_HASWELL_PPGTT_FEATURE_32BIT_PTE = 1u << 1,
    IDW_HASWELL_PPGTT_FEATURE_4K_PAGES = 1u << 2
};

struct idw_gen75_ppgtt_caps {
    uint32_t size;
    uint32_t features;
    uint64_t gpu_va_min;
    uint64_t gpu_va_max;
    uint32_t page_size;
    uint32_t ptes_per_table;
    uint32_t pde_shift;
    uint32_t reserved;
};

struct idw_gen75_ppgtt_ops {
    uint32_t size;
    uint32_t version;
    int (*query_caps)(uint32_t adapter_id,
                      struct idw_gen75_ppgtt_caps *caps);
    int (*invalidate_vm)(uint32_t adapter_id, idw_handle vm,
                         idw_gpuva start, uint64_t bytes);
    int (*activate_vm)(uint32_t adapter_id, idw_handle vm,
                       uint32_t engine_id);
    int (*deactivate_vm)(uint32_t adapter_id, idw_handle vm,
                         uint32_t engine_id);
};

/*
 * Contracts only. They do not allocate DMA memory, write PPGTT entries,
 * invalidate hardware translations, or claim Haswell hardware ownership.
 */
#endif
