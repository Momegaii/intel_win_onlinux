/* SPDX-License-Identifier: MIT */
#ifndef IDW_DEVICE_LIFECYCLE_H
#define IDW_DEVICE_LIFECYCLE_H

#include "contracts/base.h"
#include "contracts/page_table_memory.h"

#include <stdbool.h>

#define IDW_DEVICE_LIFECYCLE_VERSION 1u
#define IDW_DEVICE_LIFECYCLE_MAX_STAGES 32u
#define IDW_VENDOR_INTEL 0x8086u
#define IDW_DEVICE_HASWELL_GT2 0x0412u

enum idw_device_lifecycle_flags {
    IDW_DEVICE_LIFECYCLE_REQUIRE_HASWELL_GT2 = 1u << 0
};

enum idw_device_lifecycle_state {
    IDW_DEVICE_STATE_DORMANT = 0,
    IDW_DEVICE_STATE_INITIALIZING = 1,
    IDW_DEVICE_STATE_READY = 2,
    IDW_DEVICE_STATE_QUIESCING = 3,
    IDW_DEVICE_STATE_STOPPED = 4,
    IDW_DEVICE_STATE_FAILED = 5
};

struct idw_device_stage {
    const char *name;
    int (*init)(void *context);
    int (*fini)(void *context);
};

struct idw_device_lifecycle_config {
    uint32_t size;
    uint32_t version;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t flags;
    uint32_t stage_count;
    bool i915_bound;
    bool exclusive;
    uint16_t reserved;
    struct idw_device_scope scope;
    const struct idw_device_stage *stages;
};

struct idw_device_lifecycle {
    uint32_t size;
    uint32_t version;
    uint32_t state;
    uint32_t completed_mask;
    uint32_t failed_stage;
    uint32_t poisoned;
    uint32_t stage_count;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t flags;
    uint32_t i915_bound;
    uint32_t exclusive;
    struct idw_device_scope scope;
    const struct idw_device_stage *stages;
    void *context;
};

/*
 * Staged ownership lifecycle for an independent IDW PCI driver. The caller
 * supplies observed i915 ownership and its own exclusive-ownership decision;
 * this module never unbinds i915, maps a BAR, or touches the physical device.
 *
 * Stages run in order. A failed start rolls back completed stages in reverse
 * order. Quiesce/recover tears down in reverse order. A rollback/teardown
 * failure poisons the lifecycle so recovery cannot report a success-shaped
 * fallback.
 */
int idw_device_lifecycle_init(struct idw_device_lifecycle *lifecycle,
                              const struct idw_device_lifecycle_config *config,
                              void *context);
int idw_device_lifecycle_set_ownership(struct idw_device_lifecycle *lifecycle,
                                       bool i915_bound, bool exclusive);
int idw_device_lifecycle_start(struct idw_device_lifecycle *lifecycle);
int idw_device_lifecycle_quiesce(struct idw_device_lifecycle *lifecycle);
int idw_device_lifecycle_recover(struct idw_device_lifecycle *lifecycle);
int idw_device_lifecycle_destroy(struct idw_device_lifecycle *lifecycle);

int idw_device_lifecycle_is_ready(const struct idw_device_lifecycle *lifecycle);
int idw_device_lifecycle_is_poisoned(
    const struct idw_device_lifecycle *lifecycle);
uint32_t idw_device_lifecycle_completed(
    const struct idw_device_lifecycle *lifecycle);

#endif
