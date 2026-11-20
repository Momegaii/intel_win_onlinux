/* SPDX-License-Identifier: MIT */
#include "../include/idw/contracts/i915_assisted.h"

#include <errno.h>

int idw_i915_assisted_backend_init(
    const struct idw_device_scope *scope,
    struct idw_pt_backend_desc *backend,
    struct idw_i915_assisted_contract *contract)
{
    if (!scope || !backend || !contract || !scope->device_cookie ||
        !scope->generation)
        return -EINVAL;

    *backend = (struct idw_pt_backend_desc) {
        .size = sizeof(*backend),
        .version = IDW_PT_MEMORY_VERSION,
        .kind = IDW_PT_BACKEND_I915_ASSISTED,
        .capabilities = IDW_PT_CAP_GEM_BACKING |
                        IDW_PT_CAP_DMABUF_LIFETIME |
                        IDW_PT_CAP_CONTEXT_VM_BIND |
                        IDW_PT_CAP_I915_EXEC_WAIT,
        .scope = *scope
    };
    *contract = (struct idw_i915_assisted_contract) {
        .size = sizeof(*contract),
        .version = IDW_I915_ASSISTED_CONTRACT_VERSION,
        .features = IDW_I915_ASSISTED_GEM_BACKING |
                    IDW_I915_ASSISTED_DMABUF_LIFETIME |
                    IDW_I915_ASSISTED_CONTEXT_VM_BIND |
                    IDW_I915_ASSISTED_EXEC_WAIT,
        .scope = *scope
    };
    return 0;
}
