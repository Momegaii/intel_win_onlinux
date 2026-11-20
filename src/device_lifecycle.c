/* SPDX-License-Identifier: MIT */
#include "../include/idw/device_lifecycle.h"

#include <errno.h>
#include <string.h>

static int rollback(struct idw_device_lifecycle *lifecycle)
{
    uint32_t i = lifecycle->stage_count;
    int first_error = 0;
    int ret;

    while (i) {
        --i;
        if (!(lifecycle->completed_mask & (1u << i)))
            continue;
        if (!lifecycle->stages[i].fini) {
            if (!first_error)
                first_error = -EIO;
            continue;
        }
        ret = lifecycle->stages[i].fini(lifecycle->context);
        if (ret) {
            if (ret > 0)
                ret = -EIO;
            if (!first_error)
                first_error = ret;
        } else {
            lifecycle->completed_mask &= ~(1u << i);
        }
    }
    return first_error;
}

int idw_device_lifecycle_init(struct idw_device_lifecycle *lifecycle,
                              const struct idw_device_lifecycle_config *config,
                              void *context)
{
    if (!lifecycle || !config || config->size != sizeof(*config) ||
        config->version != IDW_DEVICE_LIFECYCLE_VERSION ||
        (config->flags & ~IDW_DEVICE_LIFECYCLE_REQUIRE_HASWELL_GT2) ||
        !config->vendor_id || !config->device_id || !config->stage_count ||
        config->stage_count > IDW_DEVICE_LIFECYCLE_MAX_STAGES ||
        !config->stages || !config->scope.device_cookie ||
        !config->scope.generation)
        return -EINVAL;

    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->size = sizeof(*lifecycle);
    lifecycle->version = IDW_DEVICE_LIFECYCLE_VERSION;
    lifecycle->state = IDW_DEVICE_STATE_DORMANT;
    lifecycle->failed_stage = UINT32_MAX;
    lifecycle->stage_count = config->stage_count;
    lifecycle->vendor_id = config->vendor_id;
    lifecycle->device_id = config->device_id;
    lifecycle->flags = config->flags;
    lifecycle->i915_bound = config->i915_bound ? 1u : 0u;
    lifecycle->exclusive = config->exclusive ? 1u : 0u;
    lifecycle->scope = config->scope;
    lifecycle->stages = config->stages;
    lifecycle->context = context;
    return 0;
}

int idw_device_lifecycle_set_ownership(struct idw_device_lifecycle *lifecycle,
                                       bool i915_bound, bool exclusive)
{
    if (!lifecycle || lifecycle->size != sizeof(*lifecycle) ||
        lifecycle->version != IDW_DEVICE_LIFECYCLE_VERSION)
        return -EINVAL;
    if (lifecycle->poisoned)
        return -EIO;
    if (lifecycle->state != IDW_DEVICE_STATE_DORMANT &&
        lifecycle->state != IDW_DEVICE_STATE_STOPPED &&
        lifecycle->state != IDW_DEVICE_STATE_FAILED)
        return -EBUSY;
    if (lifecycle->completed_mask)
        return -EBUSY;
    lifecycle->i915_bound = i915_bound ? 1u : 0u;
    lifecycle->exclusive = exclusive ? 1u : 0u;
    return 0;
}

int idw_device_lifecycle_start(struct idw_device_lifecycle *lifecycle)
{
    uint32_t i;
    int ret;
    int rollback_ret;

    if (!lifecycle || lifecycle->size != sizeof(*lifecycle) ||
        lifecycle->version != IDW_DEVICE_LIFECYCLE_VERSION)
        return -EINVAL;
    if (lifecycle->poisoned)
        return -EIO;
    if (lifecycle->state != IDW_DEVICE_STATE_DORMANT &&
        lifecycle->state != IDW_DEVICE_STATE_STOPPED)
        return -EBUSY;
    if (lifecycle->i915_bound)
        return -EBUSY;
    if (!lifecycle->exclusive)
        return -EPERM;
    if ((lifecycle->flags & IDW_DEVICE_LIFECYCLE_REQUIRE_HASWELL_GT2) &&
        (lifecycle->vendor_id != IDW_VENDOR_INTEL ||
         lifecycle->device_id != IDW_DEVICE_HASWELL_GT2))
        return -EOPNOTSUPP;

    lifecycle->state = IDW_DEVICE_STATE_INITIALIZING;
    lifecycle->completed_mask = 0;
    lifecycle->failed_stage = UINT32_MAX;
    for (i = 0; i < lifecycle->stage_count; ++i) {
        if (!lifecycle->stages[i].init)
            continue;
        ret = lifecycle->stages[i].init(lifecycle->context);
        if (ret) {
            if (ret > 0)
                ret = -EIO;
            lifecycle->failed_stage = i;
            rollback_ret = rollback(lifecycle);
            if (rollback_ret)
                lifecycle->poisoned = 1;
            else
                lifecycle->completed_mask = 0;
            lifecycle->state = IDW_DEVICE_STATE_FAILED;
            return ret;
        }
        lifecycle->completed_mask |= 1u << i;
    }
    lifecycle->state = IDW_DEVICE_STATE_READY;
    return 0;
}

int idw_device_lifecycle_quiesce(struct idw_device_lifecycle *lifecycle)
{
    int ret;

    if (!lifecycle || lifecycle->size != sizeof(*lifecycle) ||
        lifecycle->version != IDW_DEVICE_LIFECYCLE_VERSION)
        return -EINVAL;
    if (lifecycle->poisoned)
        return -EIO;
    if (lifecycle->state == IDW_DEVICE_STATE_DORMANT ||
        lifecycle->state == IDW_DEVICE_STATE_STOPPED)
        return 0;
    if (lifecycle->state != IDW_DEVICE_STATE_READY &&
        lifecycle->state != IDW_DEVICE_STATE_FAILED)
        return -EBUSY;

    lifecycle->state = IDW_DEVICE_STATE_QUIESCING;
    ret = rollback(lifecycle);
    if (ret) {
        lifecycle->poisoned = 1;
        lifecycle->state = IDW_DEVICE_STATE_FAILED;
        return ret;
    }
    lifecycle->completed_mask = 0;
    lifecycle->failed_stage = UINT32_MAX;
    lifecycle->state = IDW_DEVICE_STATE_STOPPED;
    return 0;
}

int idw_device_lifecycle_recover(struct idw_device_lifecycle *lifecycle)
{
    int ret;

    if (!lifecycle || lifecycle->size != sizeof(*lifecycle) ||
        lifecycle->version != IDW_DEVICE_LIFECYCLE_VERSION)
        return -EINVAL;
    if (lifecycle->poisoned)
        return -EIO;
    if (lifecycle->state == IDW_DEVICE_STATE_READY)
        return -EBUSY;
    if (lifecycle->state == IDW_DEVICE_STATE_DORMANT)
        return 0;
    if (lifecycle->state != IDW_DEVICE_STATE_FAILED &&
        lifecycle->state != IDW_DEVICE_STATE_STOPPED)
        return -EBUSY;
    ret = idw_device_lifecycle_quiesce(lifecycle);
    if (ret)
        return ret;
    lifecycle->state = IDW_DEVICE_STATE_DORMANT;
    return 0;
}

int idw_device_lifecycle_destroy(struct idw_device_lifecycle *lifecycle)
{
    if (!lifecycle || lifecycle->size != sizeof(*lifecycle) ||
        lifecycle->version != IDW_DEVICE_LIFECYCLE_VERSION)
        return -EINVAL;
    if (lifecycle->state == IDW_DEVICE_STATE_READY ||
        lifecycle->state == IDW_DEVICE_STATE_INITIALIZING ||
        lifecycle->state == IDW_DEVICE_STATE_QUIESCING ||
        lifecycle->completed_mask || lifecycle->poisoned)
        return -EBUSY;
    memset(lifecycle, 0, sizeof(*lifecycle));
    return 0;
}

int idw_device_lifecycle_is_ready(const struct idw_device_lifecycle *lifecycle)
{
    return lifecycle && lifecycle->state == IDW_DEVICE_STATE_READY;
}

int idw_device_lifecycle_is_poisoned(
    const struct idw_device_lifecycle *lifecycle)
{
    return lifecycle && lifecycle->poisoned;
}

uint32_t idw_device_lifecycle_completed(
    const struct idw_device_lifecycle *lifecycle)
{
    return lifecycle ? lifecycle->completed_mask : 0;
}
