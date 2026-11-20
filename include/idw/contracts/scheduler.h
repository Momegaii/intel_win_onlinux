/* SPDX-License-Identifier: MIT */
#ifndef IDW_SCHEDULER_CONTRACT_H
#define IDW_SCHEDULER_CONTRACT_H
#include "base.h"

enum idw_engine_class {
    IDW_ENGINE_RENDER = 1,
    IDW_ENGINE_COPY = 2,
    IDW_ENGINE_VIDEO = 3,
    IDW_ENGINE_VIDEO_ENHANCE = 4
};

struct idw_submit_desc {
    uint32_t size;
    uint32_t engine_class;
    idw_handle device;
    idw_handle context;
    idw_handle vm;
    idw_handle batch_allocation;
    uint64_t batch_offset;
    uint32_t batch_bytes;
    uint32_t flags;
    idw_fence_value signal_value;
};

struct idw_scheduler_ops {
    uint32_t size;
    uint32_t version;
    int (*create_context)(idw_handle device, idw_handle vm, idw_handle *context);
    int (*submit)(const struct idw_submit_desc *desc, idw_handle *fence);
    int (*wait_fence)(idw_handle fence, uint64_t timeout_ns, int *completion_status);
    int (*destroy_fence)(idw_handle fence);
    int (*destroy_context)(idw_handle context);
};
#endif
