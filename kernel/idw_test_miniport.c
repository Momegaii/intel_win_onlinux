// SPDX-License-Identifier: GPL-2.0
/* Disposable lifecycle test miniport. No physical GPU access. */
#include <linux/module.h>
#include <linux/slab.h>
#include "idw_miniport.h"

struct test_adapter { bool started; u64 next_id; };
static int test_add(void *device, void **out)
{
    struct test_adapter *a;
    (void)device;
    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a) return -ENOMEM;
    a->next_id = 1;
    *out = a;
    pr_info("idw_test_miniport: AddDevice\n");
    return 0;
}
static int test_start(void *ctx)
{
    struct test_adapter *a = ctx;
    a->started = true;
    pr_info("idw_test_miniport: StartDevice\n");
    return 0;
}
static int test_alloc(void *ctx, u64 bytes, u64 *id)
{
    struct test_adapter *a = ctx;
    if (!a->started) return -EINVAL;
    *id = a->next_id++;
    pr_info("idw_test_miniport: CreateAllocation bytes=%llu id=%llu (metadata only)\n",
            (unsigned long long)bytes, (unsigned long long)*id);
    return 0;
}
static void test_stop(void *ctx)
{
    ((struct test_adapter *)ctx)->started = false;
    pr_info("idw_test_miniport: StopDevice\n");
}
static void test_remove(void *ctx)
{
    kfree(ctx);
    pr_info("idw_test_miniport: RemoveDevice\n");
}
static const struct idw_miniport_ops test_ops = {
    .add_device = test_add, .start_device = test_start,
    .create_allocation = test_alloc, .stop_device = test_stop,
    .remove_device = test_remove,
};
static const struct idw_miniport test_mp = { .name = "idw-test-miniport", .ops = &test_ops };
static int __init test_init(void)
{
    int ret = idw_register_miniport(&test_mp);
    if (ret) return ret;
    ret = idw_dispatch_add_device(NULL);
    if (!ret) ret = idw_dispatch_start_device();
    if (!ret) {
        u64 id;
        ret = idw_dispatch_create_allocation(4096, &id);
    }
    if (ret) {
        idw_dispatch_remove_device();
        idw_unregister_miniport(&test_mp);
    }
    return ret;
}
static void __exit test_exit(void)
{
    idw_dispatch_remove_device();
    idw_unregister_miniport(&test_mp);
}
module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IDW disposable callback lifecycle test miniport");
