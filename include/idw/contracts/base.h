/* SPDX-License-Identifier: MIT */
#ifndef IDW_CONTRACT_BASE_H
#define IDW_CONTRACT_BASE_H

/* Internal IDW subsystem contracts. Not a Windows ABI or stable UAPI yet. */
#include <stdint.h>
#include <stddef.h>

#define IDW_CONTRACT_VERSION_MAJOR 1u
#define IDW_CONTRACT_VERSION_MINOR 0u
#define IDW_MAX_ADAPTERS 8u
#define IDW_MAX_ENGINES 8u

typedef uint64_t idw_handle;
typedef uint64_t idw_gpuva;
typedef uint64_t idw_fence_value;

struct idw_uuid { uint8_t bytes[16]; };

struct idw_status {
    uint32_t size;
    int32_t code;       /* 0 success; negative errno-style failure */
    uint32_t subsystem; /* stable IDW subsystem number */
    uint32_t detail;    /* subsystem-specific diagnostic */
};

#endif
