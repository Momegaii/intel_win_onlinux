/* SPDX-License-Identifier: MIT */
#ifndef IDW_NT_RUNTIME_CONTRACT_H
#define IDW_NT_RUNTIME_CONTRACT_H
#include "base.h"

/* NT semantic layer. Implementations must document IRQL/context constraints. */
enum idw_nt_pool_type {
    IDW_NT_POOL_PAGED = 1,
    IDW_NT_POOL_NONPAGED = 2
};

struct idw_nt_unicode_string {
    uint16_t length_bytes;
    uint16_t maximum_length_bytes;
    const uint16_t *buffer;
};

struct idw_nt_runtime_ops {
    uint32_t size;
    uint32_t version;
    void *(*allocate_pool)(enum idw_nt_pool_type type, size_t bytes, uint32_t tag);
    void (*free_pool)(void *ptr, uint32_t tag);
    int (*unicode_validate)(const struct idw_nt_unicode_string *value);
    int (*registry_query_u32)(const struct idw_nt_unicode_string *path,
                              const struct idw_nt_unicode_string *name,
                              uint32_t *value);
};

/* Spinlocks, IRQL and DPCs are deliberately separate contracts: Linux lock
 * primitives cannot safely emulate Windows IRQL by name substitution. */
struct idw_nt_execution_ops {
    uint32_t size;
    uint32_t version;
    int (*assert_passive_context)(void);
    int (*queue_dpc)(idw_handle dpc);
    int (*flush_dpcs)(void);
};
#endif
