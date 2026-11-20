/* SPDX-License-Identifier: MIT */
#ifndef IDW_UMD_PROTOCOL_CONTRACT_H
#define IDW_UMD_PROTOCOL_CONTRACT_H
#include "base.h"

#define IDW_UMD_PROTOCOL_MAJOR 1u
#define IDW_UMD_PROTOCOL_MINOR 0u

enum idw_umd_opcode {
    IDW_UMD_QUERY_CAPS = 1,
    IDW_UMD_CREATE_DEVICE = 2,
    IDW_UMD_DESTROY_DEVICE = 3,
    IDW_UMD_CREATE_RESOURCE = 4,
    IDW_UMD_DESTROY_RESOURCE = 5,
    IDW_UMD_CREATE_CONTEXT = 6,
    IDW_UMD_DESTROY_CONTEXT = 7,
    IDW_UMD_SUBMIT = 8,
    IDW_UMD_WAIT_FENCE = 9
};

struct idw_umd_message {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
    uint32_t opcode;
    uint32_t flags;
    uint64_t sequence;
    uint64_t object;
    uint64_t payload_bytes;
};

/* This is an IDW-owned protocol proposal, not D3DKMT binary compatibility and
 * not a shader compiler or Direct3D UMD implementation. */
#endif
