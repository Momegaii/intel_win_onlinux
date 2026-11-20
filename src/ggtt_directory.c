/* SPDX-License-Identifier: MIT */
#include "../include/idw/ggtt_directory.h"

#include <errno.h>
#include <string.h>

static int provider_check(const struct idw_ggtt_directory_provider *provider)
{
    if (!provider || provider->size != sizeof(*provider) ||
        provider->version != IDW_GGTT_DIRECTORY_VERSION ||
        !provider->ggtt || !provider->hw ||
        !provider->scope.device_cookie || !provider->scope.generation)
        return -EINVAL;
    if (provider->poisoned)
        return -EIO;
    return idw_hsw_hw_check(provider->hw);
}

static int zero_directory(struct idw_ggtt_directory_provider *provider,
                          uint64_t ggtt_offset)
{
    uint32_t index;
    int ret;

    for (index = 0; index < IDW_HSW_PPGTT_DIRECTORY_PDES; ++index) {
        ret = idw_hsw_hw_write_pde(provider->hw, ggtt_offset, index, 0);
        if (ret)
            return ret;
    }
    return idw_hsw_hw_flush_gtt(provider->hw);
}

static int alloc_directory(void *context,
                           struct idw_hsw_ppgtt_directory *directory)
{
    struct idw_ggtt_directory_provider *provider = context;
    uint64_t cookie;
    uint64_t offset;
    int ret;

    ret = provider_check(provider);
    if (ret)
        return ret;
    if (!directory || directory->size != sizeof(*directory) ||
        directory->version != IDW_HSW_PPGTT_DIRECTORY_VERSION)
        return -EINVAL;
    if (provider->next_cookie == UINT64_MAX ||
        provider->active == UINT32_MAX)
        return -EOVERFLOW;
    cookie = ++provider->next_cookie;
    ret = idw_ggtt_alloc(provider->ggtt, IDW_HSW_PPGTT_DIRECTORY_BYTES,
                         provider->alignment, cookie,
                         IDW_GGTT_ALLOC_RESIDENT |
                             IDW_GGTT_ALLOC_PAGE_DIRECTORY,
                         &offset);
    if (ret)
        return ret;
    ret = zero_directory(provider, offset);
    if (ret) {
        if (idw_ggtt_free(provider->ggtt, offset,
                          IDW_HSW_PPGTT_DIRECTORY_BYTES, cookie))
            provider->poisoned = 1;
        return ret;
    }
    memset(directory, 0, sizeof(*directory));
    directory->size = sizeof(*directory);
    directory->version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    directory->ggtt_offset = offset;
    directory->bytes = IDW_HSW_PPGTT_DIRECTORY_BYTES;
    directory->scope = provider->scope;
    directory->allocation_cookie = cookie;
    ++provider->active;
    return 0;
}

static void free_directory(void *context,
                           struct idw_hsw_ppgtt_directory *directory)
{
    struct idw_ggtt_directory_provider *provider = context;
    int ret;

    if (!provider || provider->size != sizeof(*provider) ||
        provider->version != IDW_GGTT_DIRECTORY_VERSION ||
        !provider->ggtt || !provider->hw || !directory ||
        directory->size != sizeof(*directory) ||
        directory->version != IDW_HSW_PPGTT_DIRECTORY_VERSION ||
        !directory->allocation_cookie ||
        !idw_pt_scope_equal(&provider->scope, &directory->scope))
        return;
    ret = zero_directory(provider, directory->ggtt_offset);
    if (ret)
        provider->poisoned = 1;
    ret = idw_ggtt_free(provider->ggtt, directory->ggtt_offset,
                        IDW_HSW_PPGTT_DIRECTORY_BYTES,
                        directory->allocation_cookie);
    if (ret)
        provider->poisoned = 1;
    if (!ret) {
        if (provider->active)
            --provider->active;
        memset(directory, 0, sizeof(*directory));
    }
}

static int write_pde_gttaddr(void *context,
                             const struct idw_hsw_ppgtt_directory *directory,
                             uint32_t index, uint32_t pde)
{
    struct idw_ggtt_directory_provider *provider = context;
    int ret;

    ret = provider_check(provider);
    if (ret)
        return ret;
    if (!directory || directory->size != sizeof(*directory) ||
        directory->version != IDW_HSW_PPGTT_DIRECTORY_VERSION ||
        !directory->allocation_cookie ||
        !idw_pt_scope_equal(&provider->scope, &directory->scope))
        return -EXDEV;
    return idw_hsw_hw_write_pde(provider->hw, directory->ggtt_offset,
                                index, pde);
}

static int read_pde_gttaddr(void *context,
                            const struct idw_hsw_ppgtt_directory *directory,
                            uint32_t index, uint32_t *pde)
{
    struct idw_ggtt_directory_provider *provider = context;
    int ret;

    ret = provider_check(provider);
    if (ret)
        return ret;
    if (!directory || directory->size != sizeof(*directory) ||
        directory->version != IDW_HSW_PPGTT_DIRECTORY_VERSION ||
        !directory->allocation_cookie ||
        !idw_pt_scope_equal(&provider->scope, &directory->scope))
        return -EXDEV;
    return idw_hsw_hw_read_pde(provider->hw, directory->ggtt_offset,
                               index, pde);
}

static int flush_gtt(void *context,
                     const struct idw_hsw_ppgtt_directory *directory)
{
    struct idw_ggtt_directory_provider *provider = context;
    int ret;

    ret = provider_check(provider);
    if (ret)
        return ret;
    if (!directory || directory->size != sizeof(*directory) ||
        directory->version != IDW_HSW_PPGTT_DIRECTORY_VERSION ||
        !directory->allocation_cookie ||
        !idw_pt_scope_equal(&provider->scope, &directory->scope))
        return -EXDEV;
    return idw_hsw_hw_flush_gtt(provider->hw);
}

int idw_ggtt_directory_init(struct idw_ggtt_directory_provider *provider,
                            struct idw_ggtt *ggtt, struct idw_hsw_hw *hw,
                            const struct idw_device_scope *scope,
                            uint64_t alignment)
{
    if (!provider || !ggtt || !hw || !scope ||
        !scope->device_cookie || !scope->generation ||
        alignment < IDW_HSW_GGTT_MIN_ALIGN ||
        (alignment & (alignment - 1)) ||
        (alignment & (IDW_HSW_GGTT_MIN_ALIGN - 1)))
        return -EINVAL;
    {
        struct idw_device_scope hw_scope;

        if (idw_hsw_hw_get_scope(hw, &hw_scope) ||
            !idw_pt_scope_equal(scope, &hw_scope))
            return -EXDEV;
    }
    if (idw_hsw_hw_check(hw))
        return -EIO;
    memset(provider, 0, sizeof(*provider));
    provider->size = sizeof(*provider);
    provider->version = IDW_GGTT_DIRECTORY_VERSION;
    provider->ggtt = ggtt;
    provider->hw = hw;
    provider->scope = *scope;
    provider->alignment = alignment;
    provider->next_cookie = 0;
    return 0;
}

int idw_ggtt_directory_fini(struct idw_ggtt_directory_provider *provider)
{
    if (!provider || provider->size != sizeof(*provider) ||
        provider->version != IDW_GGTT_DIRECTORY_VERSION)
        return -EINVAL;
    if (provider->active)
        return -EBUSY;
    if (provider->poisoned)
        return -EIO;
    memset(provider, 0, sizeof(*provider));
    return 0;
}

int idw_ggtt_directory_is_poisoned(
    const struct idw_ggtt_directory_provider *provider)
{
    return provider && provider->poisoned;
}

void idw_ggtt_directory_ops(struct idw_ggtt_directory_provider *provider,
                            struct idw_hsw_ppgtt_directory_ops *ops)
{
    if (!provider || !ops)
        return;
    memset(ops, 0, sizeof(*ops));
    ops->size = sizeof(*ops);
    ops->version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    ops->scope = provider->scope;
    ops->alloc_directory = alloc_directory;
    ops->free_directory = free_directory;
    ops->write_pde_gttaddr = write_pde_gttaddr;
    ops->read_pde_gttaddr = read_pde_gttaddr;
    ops->flush_gtt = flush_gtt;
    ops->context = provider;
}
