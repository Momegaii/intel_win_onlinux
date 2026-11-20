/* SPDX-License-Identifier: MIT */
#include "idw/ggtt_allocator.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static int make_ggtt(struct idw_ggtt **ggtt, uint64_t bytes)
{
    struct idw_ggtt_config config;

    config.size = sizeof(config);
    config.version = IDW_GGTT_VERSION;
    config.base_offset = 0;
    config.bytes = bytes;
    config.page_bytes = IDW_GGTT_PAGE_BYTES;
    config.max_allocations = 64;
    return idw_ggtt_create(&config, ggtt);
}

static int test_allocator(void)
{
    struct idw_ggtt *ggtt;
    struct idw_ggtt_allocation_info info;
    struct idw_device_scope scope = { 0x1234, 1 };
    uint64_t offset;
    uint64_t second;

    CHECK(make_ggtt(&ggtt, 2u * 1024u * 1024u) == 0);
    CHECK(idw_ggtt_count(ggtt) == 0);
    CHECK(idw_ggtt_free_bytes(ggtt) == 2u * 1024u * 1024u);

    CHECK(idw_ggtt_alloc(ggtt, 4096, 4096, 1, 0, &offset) == 0);
    CHECK(offset == 0);
    CHECK(idw_ggtt_alloc(ggtt, 65536, 65536, 2, 0, &second) == 0);
    CHECK(second == 0x10000);
    CHECK(idw_ggtt_alloc(ggtt, 8192, 4096, 3, 0, &offset) == 0);
    CHECK(offset == 0x1000);
    CHECK(idw_ggtt_count(ggtt) == 3);
    CHECK(idw_ggtt_free_bytes(ggtt) == 2u * 1024u * 1024u - 4096 - 65536 - 8192);

    info.size = sizeof(info);
    CHECK(idw_ggtt_query(ggtt, 0x10000, 2, &info) == 0);
    CHECK(info.offset == 0x10000 && info.bytes == 65536);
    CHECK(info.alignment == 65536 && info.cookie == 2);
    CHECK(info.resident == 0 && info.pin_count == 0);
    info.size = sizeof(info) - 1;
    CHECK(idw_ggtt_query(ggtt, 0x10000, 2, &info) == -EINVAL);

    CHECK(idw_ggtt_free(ggtt, 0x1000, 8192, 99) == -ENOENT);
    CHECK(idw_ggtt_free(ggtt, 0x1000, 4096, 3) == -EINVAL);
    CHECK(idw_ggtt_free(ggtt, 0x1000, 8192, 3) == 0);
    CHECK(idw_ggtt_alloc(ggtt, 4096, 4096, 4, 0, &offset) == 0);
    CHECK(offset == 0x1000);
    CHECK(idw_ggtt_free(ggtt, 0x1000, 4096, 4) == 0);
    CHECK(idw_ggtt_free(ggtt, 0, 4096, 1) == 0);

    CHECK(idw_ggtt_pin(ggtt, 0x10000, 65536, 2) == 0);
    CHECK(idw_ggtt_free(ggtt, 0x10000, 65536, 2) == -EBUSY);
    CHECK(idw_ggtt_set_resident(ggtt, 0x10000, 65536, 2, false) == -EBUSY);
    CHECK(idw_ggtt_set_resident(ggtt, 0x10000, 65536, 2, true) == 0);
    info.size = sizeof(info);
    CHECK(idw_ggtt_query(ggtt, 0x10000, 2, &info) == 0);
    CHECK(info.resident == 1 && info.pin_count == 1);
    CHECK(idw_ggtt_bind(ggtt, 0x10000, 65536, 2, 0x200000, &scope) == 0);
    info.size = sizeof(info);
    CHECK(idw_ggtt_query(ggtt, 0x10000, 2, &info) == 0);
    CHECK(info.bound == 1 && info.resident == 1);
    CHECK(idw_ggtt_set_resident(ggtt, 0x10000, 65536, 2, false) == -EBUSY);
    CHECK(idw_ggtt_bind(ggtt, 0x10000, 65536, 2, 0x200000, &scope) == -EEXIST);
    CHECK(idw_ggtt_free(ggtt, 0x10000, 65536, 2) == -EBUSY);
    CHECK(idw_ggtt_unbind(ggtt, 0x10000, 65536, 2) == -EBUSY);
    CHECK(idw_ggtt_unpin(ggtt, 0x10000, 65536, 2) == 0);
    CHECK(idw_ggtt_unbind(ggtt, 0x10000, 65536, 2) == 0);
    CHECK(idw_ggtt_unbind(ggtt, 0x10000, 65536, 2) == -ENOENT);
    CHECK(idw_ggtt_free(ggtt, 0x10000, 65536, 2) == 0);

    CHECK(idw_ggtt_alloc_fixed(ggtt, 1, 4096, 4096, 5, 0) == -EINVAL);
    CHECK(idw_ggtt_alloc_fixed(ggtt, 2u * 1024u * 1024u - 4096, 8192, 4096,
                               5, 0) == -ERANGE);
    CHECK(idw_ggtt_alloc_fixed(ggtt, 0x100000, 4096, 4096, 5, 0) == 0);
    CHECK(idw_ggtt_alloc_fixed(ggtt, 0x100000, 4096, 4096, 6, 0) == -EEXIST);
    CHECK(idw_ggtt_alloc_fixed(ggtt, 0x101000, 4096, 4096, 6, 0) == 0);
    CHECK(idw_ggtt_alloc_fixed(ggtt, 0x102000, 4096, 4096, 7, 0) == 0);
    CHECK(idw_ggtt_free(ggtt, 0x101000, 4096, 6) == 0);
    CHECK(idw_ggtt_free(ggtt, 0x102000, 4096, 7) == 0);
    CHECK(idw_ggtt_free(ggtt, 0x100000, 4096, 5) == 0);

    CHECK(idw_ggtt_alloc(ggtt, 4096, 3000, 8, 0, &offset) == -EINVAL);
    CHECK(idw_ggtt_alloc(ggtt, 0, 4096, 8, 0, &offset) == -EINVAL);
    CHECK(idw_ggtt_alloc(ggtt, 4097, 4096, 8, 0, &offset) == -EINVAL);
    CHECK(idw_ggtt_alloc(ggtt, 4096, 4096, 0, 0, &offset) == -EINVAL);
    CHECK(idw_ggtt_alloc(ggtt, 4096, 4096, 1, 0, &offset) == 0);
    CHECK(idw_ggtt_alloc(ggtt, 4096, 4096, 1, 0, &offset) == -EEXIST);
    CHECK(idw_ggtt_alloc(ggtt, 8192, 4096, 9, IDW_GGTT_ALLOC_PAGE_DIRECTORY,
                         &offset) == -EINVAL);
    CHECK(idw_ggtt_alloc(ggtt, 4096, 4096, 9,
                         IDW_GGTT_ALLOC_PAGE_DIRECTORY, &offset) == 0);
    CHECK(idw_ggtt_free(ggtt, offset, 4096, 9) == 0);
    CHECK(idw_ggtt_free(ggtt, 0, 4096, 1) == 0);

    CHECK(idw_ggtt_validate_range(ggtt, 0, 4096) == 0);
    CHECK(idw_ggtt_validate_range(ggtt, 2u * 1024u * 1024u - 4096, 4096) == 0);
    CHECK(idw_ggtt_validate_range(ggtt, 2u * 1024u * 1024u, 4096) == -ERANGE);
    CHECK(idw_ggtt_validate_range(ggtt, 0, 2u * 1024u * 1024u + 1) == -ERANGE);
    CHECK(idw_ggtt_validate_range(ggtt, 0, 0) == -ERANGE);

    CHECK(idw_ggtt_destroy(ggtt) == 0);
    return 0;
}

static int test_enospc_and_teardown(void)
{
    struct idw_ggtt *ggtt;
    uint64_t offset;

    CHECK(make_ggtt(&ggtt, 64u * 1024u) == 0);
    CHECK(idw_ggtt_alloc(ggtt, 64u * 1024u, 4096, 1, 0, &offset) == 0);
    CHECK(offset == 0);
    CHECK(idw_ggtt_alloc(ggtt, 4096, 4096, 2, 0, &offset) == -ENOSPC);
    CHECK(idw_ggtt_destroy(ggtt) == -EBUSY);
    CHECK(idw_ggtt_free(ggtt, 0, 64u * 1024u, 1) == 0);
    CHECK(idw_ggtt_destroy(ggtt) == 0);
    return 0;
}

static int test_invalid_config(void)
{
    struct idw_ggtt_config config;
    struct idw_ggtt *ggtt = NULL;

    config.size = sizeof(config);
    config.version = IDW_GGTT_VERSION;
    config.base_offset = 1;
    config.bytes = 4096;
    config.page_bytes = 4096;
    config.max_allocations = 1;
    CHECK(idw_ggtt_create(&config, &ggtt) == -EINVAL);
    config.base_offset = 0;
    config.max_allocations = 0;
    CHECK(idw_ggtt_create(&config, &ggtt) == -EINVAL);
    config.max_allocations = 1;
    config.page_bytes = 0;
    CHECK(idw_ggtt_create(&config, &ggtt) == -EINVAL);
    CHECK(ggtt == NULL);
    return 0;
}

int main(void)
{
    if (test_allocator() || test_enospc_and_teardown() ||
        test_invalid_config())
        return 1;
    printf("PASS: GGTT allocator, residency, binding, and teardown tests\n");
    return 0;
}
