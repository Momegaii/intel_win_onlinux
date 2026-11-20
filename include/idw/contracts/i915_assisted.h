/* SPDX-License-Identifier: MIT */
#ifndef IDW_I915_ASSISTED_BACKEND_CONTRACT_H
#define IDW_I915_ASSISTED_BACKEND_CONTRACT_H

#include "page_table_memory.h"

#define IDW_I915_ASSISTED_CONTRACT_VERSION 1u

enum idw_i915_assisted_features {
    IDW_I915_ASSISTED_GEM_BACKING = 1u << 0,
    IDW_I915_ASSISTED_DMABUF_LIFETIME = 1u << 1,
    IDW_I915_ASSISTED_CONTEXT_VM_BIND = 1u << 2,
    IDW_I915_ASSISTED_EXEC_WAIT = 1u << 3
};

/*
 * i915 remains the hardware owner. This adapter reports only the DRM/GEM
 * validation surface; it cannot expose i915's DMA mappings, PPGTT tables,
 * synchronization, invalidation, or activation as IDW-owned operations.
 */
struct idw_i915_assisted_contract {
    uint32_t size;
    uint32_t version;
    uint32_t features;
    uint32_t reserved;
    struct idw_device_scope scope;
};

int idw_i915_assisted_backend_init(
    const struct idw_device_scope *scope,
    struct idw_pt_backend_desc *backend,
    struct idw_i915_assisted_contract *contract);

#endif
