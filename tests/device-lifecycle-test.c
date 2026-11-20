/* SPDX-License-Identifier: MIT */
#include "idw/device_lifecycle.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

struct fake_context {
    int init_count;
    int fini_count;
    int fail_init;
    int fail_fini;
    int order[64];
    int order_count;
};

static void reset_context(struct fake_context *context)
{
    memset(context, 0, sizeof(*context));
    context->fail_init = -1;
    context->fail_fini = -1;
}

static int record_init(struct fake_context *context, int stage)
{
    if (context->order_count < 64)
        context->order[context->order_count++] = stage;
    ++context->init_count;
    return context->fail_init == stage ? -EIO : 0;
}

static int record_fini(struct fake_context *context, int stage)
{
    if (context->order_count < 64)
        context->order[context->order_count++] = 100 + stage;
    ++context->fini_count;
    return context->fail_fini == stage ? -EIO : 0;
}

#define DEFINE_STAGE(n) \
    static int stage##n##_init(void *context) \
    { return record_init(context, n); } \
    static int stage##n##_fini(void *context) \
    { return record_fini(context, n); }

DEFINE_STAGE(0)
DEFINE_STAGE(1)
DEFINE_STAGE(2)
DEFINE_STAGE(3)

static const struct idw_device_stage stages[4] = {
    { "claim", stage0_init, stage0_fini },
    { "bars", stage1_init, stage1_fini },
    { "dma", stage2_init, stage2_fini },
    { "engines", stage3_init, stage3_fini }
};

static int init_lifecycle(struct idw_device_lifecycle *lifecycle,
                          struct fake_context *context, bool i915_bound,
                          bool exclusive, uint32_t flags,
                          uint16_t vendor, uint16_t device)
{
    struct idw_device_lifecycle_config config;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.version = IDW_DEVICE_LIFECYCLE_VERSION;
    config.vendor_id = vendor;
    config.device_id = device;
    config.flags = flags;
    config.stage_count = 4;
    config.i915_bound = i915_bound;
    config.exclusive = exclusive;
    config.scope.device_cookie = 0x1234;
    config.scope.generation = 1;
    config.stages = stages;
    return idw_device_lifecycle_init(lifecycle, &config, context);
}

static int test_ownership_rejection(void)
{
    struct idw_device_lifecycle lifecycle;
    struct fake_context context;

    reset_context(&context);
    CHECK(init_lifecycle(&lifecycle, &context, true, true, 0,
                         IDW_VENDOR_INTEL, IDW_DEVICE_HASWELL_GT2) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == -EBUSY);
    CHECK(context.init_count == 0);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == 0);

    reset_context(&context);
    CHECK(init_lifecycle(&lifecycle, &context, false, false, 0,
                         IDW_VENDOR_INTEL, IDW_DEVICE_HASWELL_GT2) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == -EPERM);
    CHECK(context.init_count == 0);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == 0);

    reset_context(&context);
    CHECK(init_lifecycle(&lifecycle, &context, false, true,
                         IDW_DEVICE_LIFECYCLE_REQUIRE_HASWELL_GT2,
                         IDW_VENDOR_INTEL, 0x0413) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == -EOPNOTSUPP);
    CHECK(context.init_count == 0);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == 0);

    reset_context(&context);
    CHECK(init_lifecycle(&lifecycle, &context, false, true, 0,
                         IDW_VENDOR_INTEL, IDW_DEVICE_HASWELL_GT2) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_is_ready(&lifecycle));
    CHECK(context.init_count == 4);
    CHECK(context.order_count == 4);
    CHECK(context.order[0] == 0 && context.order[1] == 1 &&
          context.order[2] == 2 && context.order[3] == 3);
    CHECK(idw_device_lifecycle_start(&lifecycle) == -EBUSY);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == -EBUSY);
    return 0;
}

static int test_teardown_and_recovery(void)
{
    struct idw_device_lifecycle lifecycle;
    struct fake_context context;

    reset_context(&context);
    CHECK(init_lifecycle(&lifecycle, &context, false, true, 0,
                         IDW_VENDOR_INTEL, IDW_DEVICE_HASWELL_GT2) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_quiesce(&lifecycle) == 0);
    CHECK(context.fini_count == 4);
    CHECK(context.order[4] == 103 && context.order[5] == 102 &&
          context.order[6] == 101 && context.order[7] == 100);
    CHECK(idw_device_lifecycle_completed(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_quiesce(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == 0);

    reset_context(&context);
    context.fail_init = 2;
    CHECK(init_lifecycle(&lifecycle, &context, false, true, 0,
                         IDW_VENDOR_INTEL, IDW_DEVICE_HASWELL_GT2) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == -EIO);
    CHECK(context.init_count == 3);
    CHECK(context.fini_count == 2);
    CHECK(context.order[3] == 101 && context.order[4] == 100);
    CHECK(idw_device_lifecycle_completed(&lifecycle) == 0);
    CHECK(!idw_device_lifecycle_is_poisoned(&lifecycle));
    CHECK(idw_device_lifecycle_recover(&lifecycle) == 0);
    CHECK(!idw_device_lifecycle_is_ready(&lifecycle));
    context.fail_init = -1;
    CHECK(idw_device_lifecycle_start(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_quiesce(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == 0);
    return 0;
}

static int test_failure_poison(void)
{
    struct idw_device_lifecycle lifecycle;
    struct fake_context context;

    reset_context(&context);
    CHECK(init_lifecycle(&lifecycle, &context, false, true, 0,
                         IDW_VENDOR_INTEL, IDW_DEVICE_HASWELL_GT2) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == 0);
    context.fail_fini = 1;
    CHECK(idw_device_lifecycle_quiesce(&lifecycle) == -EIO);
    CHECK(idw_device_lifecycle_is_poisoned(&lifecycle));
    CHECK(idw_device_lifecycle_completed(&lifecycle) == (1u << 1));
    CHECK(idw_device_lifecycle_recover(&lifecycle) == -EIO);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == -EBUSY);

    reset_context(&context);
    context.fail_init = 1;
    context.fail_fini = -1;
    CHECK(init_lifecycle(&lifecycle, &context, false, true, 0,
                         IDW_VENDOR_INTEL, IDW_DEVICE_HASWELL_GT2) == 0);
    CHECK(idw_device_lifecycle_start(&lifecycle) == -EIO);
    CHECK(!idw_device_lifecycle_is_poisoned(&lifecycle));
    CHECK(idw_device_lifecycle_recover(&lifecycle) == 0);
    CHECK(idw_device_lifecycle_destroy(&lifecycle) == 0);
    return 0;
}

static int test_invalid_config(void)
{
    struct idw_device_lifecycle lifecycle;
    struct fake_context context;
    struct idw_device_lifecycle_config config;

    reset_context(&context);
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.version = IDW_DEVICE_LIFECYCLE_VERSION;
    config.vendor_id = IDW_VENDOR_INTEL;
    config.device_id = IDW_DEVICE_HASWELL_GT2;
    config.scope.device_cookie = 1;
    config.scope.generation = 1;
    config.stage_count = 0;
    config.stages = stages;
    CHECK(idw_device_lifecycle_init(&lifecycle, &config, &context) == -EINVAL);
    config.stage_count = 33;
    CHECK(idw_device_lifecycle_init(&lifecycle, &config, &context) == -EINVAL);
    config.stage_count = 4;
    config.stages = NULL;
    CHECK(idw_device_lifecycle_init(&lifecycle, &config, &context) == -EINVAL);
    config.stages = stages;
    config.flags = 1u << 31;
    CHECK(idw_device_lifecycle_init(&lifecycle, &config, &context) == -EINVAL);
    config.flags = 0;
    config.scope.device_cookie = 0;
    CHECK(idw_device_lifecycle_init(&lifecycle, &config, &context) == -EINVAL);
    config.scope.device_cookie = 1;
    config.version = 0;
    CHECK(idw_device_lifecycle_init(&lifecycle, &config, &context) == -EINVAL);
    config.version = IDW_DEVICE_LIFECYCLE_VERSION;
    config.size = sizeof(config) - 1;
    CHECK(idw_device_lifecycle_init(&lifecycle, &config, &context) == -EINVAL);
    return 0;
}

int main(void)
{
    if (test_ownership_rejection() || test_teardown_and_recovery() ||
        test_failure_poison() || test_invalid_config())
        return 1;
    printf("PASS: independent-device ownership, teardown, and recovery tests\n");
    return 0;
}
