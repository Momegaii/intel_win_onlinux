/* SPDX-License-Identifier: MIT */
#ifndef IDW_GPUVA_MODEL_H
#define IDW_GPUVA_MODEL_H

#include "contracts/base.h"

struct idw_gpuva_range;

struct idw_gpuva_space {
    uint64_t min_va;
    uint64_t max_va;
    struct idw_gpuva_range *ranges;
};

#ifdef __cplusplus
extern "C" {
#endif

/* CPU-only allocator; callers provide external serialization. max_va is exclusive. */
int idw_gpuva_space_init(struct idw_gpuva_space *space,
                         uint64_t min_va, uint64_t max_va);
void idw_gpuva_space_fini(struct idw_gpuva_space *space);
int idw_gpuva_alloc(struct idw_gpuva_space *space, uint64_t bytes,
                    uint64_t alignment, uint64_t guard_bytes,
                    idw_gpuva *va);
int idw_gpuva_reserve(struct idw_gpuva_space *space, idw_gpuva va,
                      uint64_t bytes, uint64_t guard_bytes);
int idw_gpuva_free(struct idw_gpuva_space *space, idw_gpuva va);

#ifdef __cplusplus
}
#endif

#endif
