/* CPU-only tests. Synthetic DMA addresses are not hardware mappings. */
#include "../include/idw/gpuva_model.h"
#include "../include/idw/haswell_ppgtt_model.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define TEST_SCOPE_COOKIE 0x4944570000000001ULL
#define TEST_SCOPE_GENERATION 1ULL
#define TEST_DEVICE_SCOPE { \
    .device_cookie = TEST_SCOPE_COOKIE, \
    .generation = TEST_SCOPE_GENERATION \
}
#define TEST_DMA_PAGE(address) { \
    .dma_address = (address), \
    .bytes = IDW_HASWELL_PPGTT_PAGE_SIZE, \
    .scope = TEST_DEVICE_SCOPE, \
    .mapping_cookie = (address) \
}

struct fake_page_allocator {
    uint64_t next_dma;
    unsigned calls;
    unsigned fail_call;
    unsigned duplicate_dma_on_call;
    unsigned wrong_scope_on_call;
    unsigned allocated;
    unsigned freed;
    uint64_t last_dma;
};

static int fake_alloc_page(void *context, struct idw_dma_page *page)
{
    struct fake_page_allocator *allocator = context;
    void *memory;

    ++allocator->calls;
    if (allocator->fail_call == allocator->calls)
        return -ENOMEM;
    memory = aligned_alloc(IDW_HASWELL_PPGTT_PAGE_SIZE,
                           IDW_HASWELL_PPGTT_PAGE_SIZE);
    if (!memory)
        return -ENOMEM;
    page->size = sizeof(*page);
    page->memory_type = IDW_PT_PAGE_COHERENT;
    page->cpu_address = memory;
    page->dma_address = allocator->duplicate_dma_on_call == allocator->calls ?
                        allocator->last_dma : allocator->next_dma;
    page->bytes = IDW_HASWELL_PPGTT_PAGE_SIZE;
    page->scope = (struct idw_device_scope) TEST_DEVICE_SCOPE;
    if (allocator->wrong_scope_on_call == allocator->calls)
        ++page->scope.device_cookie;
    page->allocation_cookie = allocator->calls;
    if (allocator->duplicate_dma_on_call != allocator->calls) {
        allocator->last_dma = allocator->next_dma;
        allocator->next_dma += IDW_HASWELL_PPGTT_PAGE_SIZE;
    }
    ++allocator->allocated;
    return 0;
}

static void fake_free_page(void *context, struct idw_dma_page *page)
{
    struct fake_page_allocator *allocator = context;

    if (page->cpu_address) {
        free(page->cpu_address);
        page->cpu_address = NULL;
        ++allocator->freed;
    }
}

static uint32_t encode_gen67_address(uint64_t address)
{
    return (uint32_t)address | (uint32_t)((address >> 28) & 0xff0u);
}

static int test_gpuva_space(void)
{
    struct idw_gpuva_space space;
    idw_gpuva first = 0;
    idw_gpuva second = 0;

    CHECK(idw_gpuva_space_init(&space, 0, 1ULL << 31) == 0);
    CHECK(idw_gpuva_alloc(&space, 8192, 4096, 4096, &first) == 0);
    CHECK(first == 4096);
    CHECK(idw_gpuva_alloc(&space, 4096, 65536, 4096, &second) == 0);
    CHECK((second & 65535) == 0);
    CHECK(second > first);
    CHECK(idw_gpuva_reserve(&space, first, 4096, 0) == -EEXIST);
    CHECK(idw_gpuva_reserve(&space, 0x100000, 4096, 4096) == 0);
    CHECK(idw_gpuva_reserve(&space, 0x101000, 4096, 0) == -EEXIST);
    CHECK(idw_gpuva_free(&space, first) == 0);
    CHECK(idw_gpuva_free(&space, first) == -ENOENT);
    CHECK(idw_gpuva_reserve(&space, 0x7ffff000, 8192, 0) == -ERANGE);
    CHECK(idw_gpuva_reserve(&space, 0x2001, 4096, 0) == -EINVAL);
    idw_gpuva_space_fini(&space);

    CHECK(idw_gpuva_space_init(&space, 0, UINT64_MAX & ~4095ULL) == 0);
    CHECK(idw_gpuva_reserve(&space, UINT64_MAX & ~8191ULL, 8192, 0) ==
          -EOVERFLOW);
    idw_gpuva_space_fini(&space);
    return 0;
}

static int test_geometry_and_lifecycle(void)
{
    struct fake_page_allocator allocator = {
        .next_dma = 0x00800000
    };
    struct idw_hsw_ppgtt_page_ops ops = {
        .size = sizeof(ops),
        .version = 1,
        .alloc_page = fake_alloc_page,
        .free_page = fake_free_page,
        .context = &allocator
    };
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_translation translation = {
        .size = sizeof(translation)
    };
    struct idw_hsw_ppgtt_pending_invalidation pending = {
        .size = sizeof(pending)
    };
    const uint64_t va = (1ULL << 22) - 4096;
    const struct idw_dma_page_mapping dma_pages[] = {
        TEST_DMA_PAGE(0x1234567000ULL), TEST_DMA_PAGE(0x2345678000ULL)
    };

    ops.scope = (struct idw_device_scope) TEST_DEVICE_SCOPE;
    CHECK(encode_gen67_address(dma_pages[0].dma_address) == 0x34567120u);
    CHECK(encode_gen67_address(dma_pages[1].dma_address) == 0x45678230u);
    CHECK(idw_hsw_ppgtt_model_create(&ops, &model) == 0);
    CHECK(idw_hsw_ppgtt_map(model, va, 8192, dma_pages, 2) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) ==
          encode_gen67_address(0x00800000) + 1);
    CHECK(idw_hsw_ppgtt_pde(model, 1) ==
          encode_gen67_address(0x00801000) + 1);
    CHECK(idw_hsw_ppgtt_translate(model, va, &translation) == 0);
    CHECK(translation.pde_index == 0);
    CHECK(translation.pte_index == 1023);
    CHECK(translation.pte == 0x34567121u);
    CHECK(translation.dma_address == dma_pages[0].dma_address);
    CHECK(idw_hsw_ppgtt_translate(model, va + 8191, &translation) == 0);
    CHECK(translation.pde_index == 1);
    CHECK(translation.pte_index == 0);
    CHECK(translation.dma_address == dma_pages[1].dma_address + 4095);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -EAGAIN);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(pending.pending == 1);
    CHECK(pending.start == va && pending.bytes == 8192);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model,
                                               pending.generation - 1) == -ESTALE);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_check_submit(model) == 0);

    CHECK(idw_hsw_ppgtt_unmap(model, va, 4096) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0);
    CHECK(allocator.freed == 0);
    CHECK(idw_hsw_ppgtt_translate(model, va, &translation) == -ENOENT);
    CHECK(idw_hsw_ppgtt_translate(model, va + 4096, &translation) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model,
        allocator.calls + 100) == -ESTALE);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(allocator.freed == 1);

    CHECK(idw_hsw_ppgtt_map(model, va, 4096, dma_pages, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_unmap(model, va, 8192) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(allocator.allocated == allocator.freed);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_invalid_maps_and_rollback(void)
{
    struct fake_page_allocator allocator = {
        .next_dma = 0x01000000,
        .fail_call = 2
    };
    struct idw_hsw_ppgtt_page_ops ops = {
        .size = sizeof(ops),
        .version = 1,
        .scope = TEST_DEVICE_SCOPE,
        .alloc_page = fake_alloc_page,
        .free_page = fake_free_page,
        .context = &allocator
    };
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_translation translation = {
        .size = sizeof(translation)
    };
    struct idw_hsw_ppgtt_pending_invalidation pending = {
        .size = sizeof(pending)
    };
    const uint64_t split_va = (1ULL << 22) - 4096;
    const struct idw_dma_page_mapping good_pages[] = {
        TEST_DMA_PAGE(0x40000000), TEST_DMA_PAGE(0x40001000)
    };
    const struct idw_dma_page_mapping bad_alignment[] = {
        TEST_DMA_PAGE(0x40000001)
    };
    const struct idw_dma_page_mapping too_wide[] = {
        TEST_DMA_PAGE(1ULL << 40)
    };
    struct idw_dma_page_mapping wrong_device[] = {
        TEST_DMA_PAGE(0x40000000)
    };
    const struct idw_dma_page_mapping one_page[] = {
        TEST_DMA_PAGE(0x40000000)
    };

    wrong_device[0].scope.device_cookie++;
    CHECK(idw_hsw_ppgtt_model_create(&ops, &model) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, NULL, 1) == -EINVAL);
    CHECK(idw_hsw_ppgtt_map(model, 1, 4096, one_page, 1) == -EINVAL);
    CHECK(idw_hsw_ppgtt_map(model, 0, 0, one_page, 1) == -EINVAL);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, one_page, 0) == -EINVAL);
    CHECK(idw_hsw_ppgtt_map(model, 1ULL << 31, 4096, one_page, 1) == -ERANGE);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, wrong_device, 1) == -EXDEV);
    CHECK(idw_hsw_ppgtt_map(model, split_va, 8192, good_pages, 2) == -ENOMEM);
    CHECK(allocator.allocated == 1 && allocator.freed == 1);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 1) == 0);
    CHECK(idw_hsw_ppgtt_translate(model, split_va, &translation) == -ENOENT);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(pending.pending == 0 && pending.generation == 0);

    allocator.fail_call = 0;
    allocator.duplicate_dma_on_call = allocator.calls + 2;
    CHECK(idw_hsw_ppgtt_map(model, split_va, 8192, good_pages, 2) == -EEXIST);
    CHECK(allocator.allocated == allocator.freed);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 1) == 0);
    allocator.duplicate_dma_on_call = 0;
    allocator.wrong_scope_on_call = allocator.calls + 1;
    CHECK(idw_hsw_ppgtt_map(model, 4ULL << 22, 4096, one_page, 1) ==
          -EXDEV);
    CHECK(allocator.allocated == allocator.freed);
    CHECK(idw_hsw_ppgtt_pde(model, 4) == 0);
    allocator.wrong_scope_on_call = 0;
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, bad_alignment, 1) == -ERANGE);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, too_wide, 1) == -ERANGE);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, one_page, 1) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, one_page, 1) == -EEXIST);
    CHECK(idw_hsw_ppgtt_unmap(model, 0, 8192) == -ENOENT);
    CHECK(idw_hsw_ppgtt_translate(model, 0, &translation) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_failed_invalidation_poison(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x02000000 };
    struct idw_hsw_ppgtt_page_ops ops = {
        .size = sizeof(ops),
        .version = 1,
        .scope = TEST_DEVICE_SCOPE,
        .alloc_page = fake_alloc_page,
        .free_page = fake_free_page,
        .context = &allocator
    };
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_pending_invalidation pending = {
        .size = sizeof(pending)
    };
    struct idw_hsw_ppgtt_translation translation = {
        .size = sizeof(translation)
    };
    const struct idw_dma_page_mapping page[] = {
        TEST_DMA_PAGE(0x30000000)
    };

    CHECK(idw_hsw_ppgtt_model_create(&ops, &model) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_unmap(model, 0, 4096) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_failed(model, pending.generation) == -EIO);
    CHECK(allocator.freed == 0);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -EIO);
    CHECK(idw_hsw_ppgtt_translate(model, 0, &translation) == -EIO);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == -EIO);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_invalidation_generation_coalescing(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x03000000 };
    struct idw_hsw_ppgtt_page_ops ops = {
        .size = sizeof(ops),
        .version = 1,
        .scope = TEST_DEVICE_SCOPE,
        .alloc_page = fake_alloc_page,
        .free_page = fake_free_page,
        .context = &allocator
    };
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_pending_invalidation pending = {
        .size = sizeof(pending)
    };
    const struct idw_dma_page_mapping page_a[] = {
        TEST_DMA_PAGE(0x50000000)
    };
    const struct idw_dma_page_mapping page_b[] = {
        TEST_DMA_PAGE(0x50001000)
    };
    uint64_t first_generation;

    CHECK(idw_hsw_ppgtt_model_create(&ops, &model) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page_a, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    first_generation = pending.generation;
    CHECK(idw_hsw_ppgtt_map(model, 8ULL << 20, 4096, page_b, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(pending.generation == first_generation + 1);
    CHECK(pending.start == 0);
    CHECK(pending.bytes == (8ULL << 20) + 4096);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, first_generation) == -ESTALE);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -EAGAIN);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_check_submit(model) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

int main(void)
{
    if (test_gpuva_space() || test_geometry_and_lifecycle() ||
        test_invalid_maps_and_rollback() || test_failed_invalidation_poison() ||
        test_invalidation_generation_coalescing())
        return 1;
    puts("PASS: CPU-only GPU-VA allocator and Haswell Gen7.5 PPGTT model tests");
    puts("NOTE: synthetic DMA addresses validate software structures only; no GPU mapping or hardware control was tested.");
    return 0;
}
