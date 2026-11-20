/* SPDX-License-Identifier: MIT */
#ifndef IDW_VIDMEM_CONTRACT_H
#define IDW_VIDMEM_CONTRACT_H
#include "base.h"

enum idw_memory_class {
    IDW_MEMORY_SYSTEM = 1,
    IDW_MEMORY_APERTURE = 2,
    IDW_MEMORY_LOCAL = 3 /* Haswell backend must report unsupported if absent */
};

enum idw_memory_flags {
    IDW_MEM_CPU_VISIBLE = 1u << 0,
    IDW_MEM_GPU_VISIBLE = 1u << 1,
    IDW_MEM_COHERENT = 1u << 2,
    IDW_MEM_IMPORTED = 1u << 3,
    IDW_MEM_PINNED = 1u << 4
};

struct idw_memory_desc {
    uint32_t size;
    uint32_t memory_class;
    uint32_t flags;
    uint32_t alignment;
    uint64_t bytes;
    uint64_t placement_hint;
};

struct idw_memory_ops {
    uint32_t size;
    uint32_t version;
    int (*create)(const struct idw_memory_desc *desc, idw_handle *allocation);
    int (*import_dmabuf)(int fd, idw_handle *allocation);
    int (*pin)(idw_handle allocation);
    int (*unpin)(idw_handle allocation);
    int (*evict)(idw_handle allocation);
    int (*destroy)(idw_handle allocation);
};
#endif
