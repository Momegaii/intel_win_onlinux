/* SPDX-License-Identifier: MIT */
#ifndef IDW_GGTT_DIRECTORY_H
#define IDW_GGTT_DIRECTORY_H

#include "ggtt_allocator.h"
#include "haswell_gtt_hw.h"
#include "haswell_ppgtt_model.h"

#define IDW_GGTT_DIRECTORY_VERSION 1u

struct idw_ggtt_directory_provider {
    uint32_t size;
    uint32_t version;
    struct idw_ggtt *ggtt;
    struct idw_hsw_hw *hw;
    struct idw_device_scope scope;
    uint64_t alignment;
    uint64_t next_cookie;
    uint32_t active;
    uint32_t poisoned;
};

/*
 * Supplies GGTT-resident 4 KiB page directories to the CPU PPGTT model.
 * Allocation, zeroing, PDE read/write, and flush go through the GGTT
 * allocator and the hardware abstraction. This is software infrastructure;
 * it does not claim that a physical GPU is using the directory.
 */
int idw_ggtt_directory_init(struct idw_ggtt_directory_provider *provider,
                            struct idw_ggtt *ggtt, struct idw_hsw_hw *hw,
                            const struct idw_device_scope *scope,
                            uint64_t alignment);
int idw_ggtt_directory_fini(struct idw_ggtt_directory_provider *provider);
int idw_ggtt_directory_is_poisoned(
    const struct idw_ggtt_directory_provider *provider);
void idw_ggtt_directory_ops(struct idw_ggtt_directory_provider *provider,
                            struct idw_hsw_ppgtt_directory_ops *ops);

#endif
