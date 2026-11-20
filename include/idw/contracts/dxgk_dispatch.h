/* SPDX-License-Identifier: MIT */
#ifndef IDW_DXGK_DISPATCH_CONTRACT_H
#define IDW_DXGK_DISPATCH_CONTRACT_H
#include "base.h"
#include "vidmem.h"
#include "gpuvm.h"
#include "scheduler.h"

/* IDW-native DXGKRNL-inspired dispatch contract. Never cast Windows DDI
 * function tables into this structure: layouts, calling conventions and
 * lifetime rules are not binary-compatible. */
struct idw_adapter_desc {
    uint32_t size;
    uint32_t adapter_id;
    struct idw_uuid adapter_uuid;
    uint32_t flags;
    uint32_t reserved;
};

struct idw_device_desc {
    uint32_t size;
    uint32_t flags;
    idw_handle vm;
};

struct idw_miniport_ops_v1 {
    uint32_t size;
    uint32_t version;
    int (*add_adapter)(const struct idw_adapter_desc *desc, idw_handle *adapter);
    int (*start_adapter)(idw_handle adapter);
    int (*query_adapter_caps)(idw_handle adapter, void *caps, uint32_t caps_bytes);
    int (*create_device)(idw_handle adapter, const struct idw_device_desc *desc,
                         idw_handle *device);
    int (*destroy_device)(idw_handle device);
    int (*stop_adapter)(idw_handle adapter);
    int (*remove_adapter)(idw_handle adapter);
};

struct idw_dispatch_registration {
    uint32_t size;
    uint32_t version;
    const struct idw_miniport_ops_v1 *ops;
    const struct idw_memory_ops *memory;
    const struct idw_gpuvm_ops *gpuvm;
    const struct idw_scheduler_ops *scheduler;
};
#endif
