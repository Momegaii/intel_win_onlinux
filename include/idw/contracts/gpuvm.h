/* SPDX-License-Identifier: MIT */
#ifndef IDW_GPUVM_CONTRACT_H
#define IDW_GPUVM_CONTRACT_H
#include "base.h"

enum idw_vm_map_flags {
    IDW_VM_READ = 1u << 0,
    IDW_VM_WRITE = 1u << 1,
    IDW_VM_EXEC = 1u << 2,
    IDW_VM_FIXED = 1u << 3
};

struct idw_vm_map_desc {
    uint32_t size;
    uint32_t flags;
    idw_handle allocation;
    uint64_t allocation_offset;
    uint64_t bytes;
    idw_gpuva requested_va;
};

struct idw_gpuvm_ops {
    uint32_t size;
    uint32_t version;
    int (*create)(uint32_t adapter_id, idw_handle *vm);
    int (*map)(idw_handle vm, const struct idw_vm_map_desc *desc, idw_gpuva *va);
    int (*unmap)(idw_handle vm, idw_gpuva va, uint64_t bytes);
    int (*destroy)(idw_handle vm);
    int (*query_caps)(uint32_t adapter_id, uint64_t *min_va,
                      uint64_t *max_va, uint64_t *page_size_mask);
};

/* Generic GPU-VM contract. The Gen7.5 DMA and invalidation boundary is defined
 * separately in haswell_ppgtt.h; neither header implies a runtime backend. */
#endif
