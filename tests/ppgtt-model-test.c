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
#define TEST_DIRECTORY_OFFSET 0x40000ULL

struct fake_page_allocator {
    uint64_t next_dma;
    unsigned calls;
    unsigned fail_call;
    unsigned duplicate_dma_on_call;
    unsigned wrong_scope_on_call;
    unsigned allocated;
    unsigned freed;
    unsigned syncs;
    unsigned sync_fail_call;
    uint64_t last_dma;
};

struct fake_directory {
    struct idw_device_scope scope;
    uint32_t hardware[IDW_HSW_PPGTT_PDES];
    uint64_t ggtt_offset;
    uint64_t cookie;
    unsigned allocations;
    unsigned frees;
    unsigned writes;
    unsigned reads;
    unsigned flushes;
    unsigned write_calls;
    unsigned read_calls;
    unsigned flush_calls;
    unsigned write_fail_call;
    unsigned read_fail_call;
    unsigned flush_fail_call;
    int allocated;
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

static int fake_sync_page(void *context, const struct idw_dma_page *page,
                          uint32_t offset, uint32_t bytes)
{
    struct fake_page_allocator *allocator = context;

    if (!page || offset || bytes != IDW_HASWELL_PPGTT_PAGE_SIZE)
        return -EINVAL;
    ++allocator->syncs;
    return allocator->sync_fail_call == allocator->syncs ? -EIO : 0;
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

static int fake_directory_alloc(void *context,
                                struct idw_hsw_ppgtt_directory *directory)
{
    struct fake_directory *fake = context;

    if (fake->allocated)
        return -EBUSY;
    memset(fake->hardware, 0, sizeof(fake->hardware));
    directory->size = sizeof(*directory);
    directory->version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    directory->ggtt_offset = fake->ggtt_offset;
    directory->bytes = IDW_HSW_PPGTT_DIRECTORY_BYTES;
    directory->scope = fake->scope;
    directory->allocation_cookie = fake->cookie;
    fake->allocated = 1;
    ++fake->allocations;
    return 0;
}

static void fake_directory_free(void *context,
                                struct idw_hsw_ppgtt_directory *directory)
{
    struct fake_directory *fake = context;

    (void)directory;
    if (fake->allocated) {
        fake->allocated = 0;
        ++fake->frees;
    }
}

static int fake_directory_write(void *context,
                                const struct idw_hsw_ppgtt_directory *directory,
                                uint32_t index, uint32_t pde)
{
    struct fake_directory *fake = context;

    (void)directory;
    ++fake->write_calls;
    if (fake->write_fail_call == fake->write_calls)
        return -EIO;
    if (index >= IDW_HSW_PPGTT_PDES)
        return -EINVAL;
    fake->hardware[index] = pde;
    ++fake->writes;
    return 0;
}

static int fake_directory_read(void *context,
                               const struct idw_hsw_ppgtt_directory *directory,
                               uint32_t index, uint32_t *pde)
{
    struct fake_directory *fake = context;

    (void)directory;
    ++fake->read_calls;
    if (fake->read_fail_call == fake->read_calls)
        return -EIO;
    if (index >= IDW_HSW_PPGTT_PDES || !pde)
        return -EINVAL;
    *pde = fake->hardware[index];
    ++fake->reads;
    return 0;
}

static int fake_directory_flush(void *context,
                                const struct idw_hsw_ppgtt_directory *directory)
{
    struct fake_directory *fake = context;

    (void)directory;
    ++fake->flush_calls;
    if (fake->flush_fail_call == fake->flush_calls)
        return -EIO;
    ++fake->flushes;
    return 0;
}

static void fake_directory_init(struct fake_directory *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->scope = (struct idw_device_scope) TEST_DEVICE_SCOPE;
    fake->ggtt_offset = TEST_DIRECTORY_OFFSET;
    fake->cookie = 0x4449524543544f52ULL;
}

static void fake_ops_init(struct idw_hsw_ppgtt_page_ops *ops,
                          struct fake_page_allocator *allocator)
{
    *ops = (struct idw_hsw_ppgtt_page_ops) {
        .size = sizeof(*ops),
        .version = 1,
        .scope = TEST_DEVICE_SCOPE,
        .alloc_page_table_page = fake_alloc_page,
        .free_page_table_page = fake_free_page,
        .sync_page_table_for_device = fake_sync_page,
        .context = allocator
    };
}

static void fake_directory_ops_init(struct idw_hsw_ppgtt_directory_ops *ops,
                                    struct fake_directory *fake)
{
    *ops = (struct idw_hsw_ppgtt_directory_ops) {
        .size = sizeof(*ops),
        .version = IDW_HSW_PPGTT_DIRECTORY_VERSION,
        .scope = TEST_DEVICE_SCOPE,
        .alloc_directory = fake_directory_alloc,
        .free_directory = fake_directory_free,
        .write_pde_gttaddr = fake_directory_write,
        .read_pde_gttaddr = fake_directory_read,
        .flush_gtt = fake_directory_flush,
        .context = fake
    };
}

static int create_model(struct idw_hsw_ppgtt_model **model,
                        struct fake_page_allocator *allocator,
                        struct fake_directory *fake)
{
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_hsw_ppgtt_directory_ops directory_ops;

    fake_ops_init(&page_ops, allocator);
    fake_directory_ops_init(&directory_ops, fake);
    return idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, model);
}

static int drain_invalidation(struct idw_hsw_ppgtt_model *model)
{
    struct idw_hsw_ppgtt_pending_invalidation pending;
    int ret;

    memset(&pending, 0, sizeof(pending));
    pending.size = sizeof(pending);
    ret = idw_hsw_ppgtt_pending(model, &pending);
    if (ret || !pending.pending)
        return ret;
    return idw_hsw_ppgtt_invalidation_complete(model, pending.generation);
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

/*
 * These expectations are hand-derived from the PRM bit layouts, not from the
 * encoder under test.
 *
 * PDE 0x12_3456_7000: address 31:12 = 0x34567000, address 39:32 = 0x12.
 *   entry = 0x34567000 | (0x12 << 4) | valid = 0x34567121.
 * PTE 0x7f_ffff_f000: address 31:12 = 0xfffff000, address 38:32 = 0x7f.
 *   entry = 0xfffff000 | (0x7f << 4) | valid = 0xfffff7f1.
 * PTE cacheability 0x8: bit 3 of the control maps to entry bit 11.
 *   entry = 0x00000000 | 0x800 | valid = 0x00000801.
 */
static int test_golden_encoding(void)
{
    uint32_t entry = 0;

    CHECK(idw_hsw_ppgtt_encode_pde(0x1234567000ULL, &entry) == 0);
    CHECK(entry == 0x34567121u);
    CHECK(idw_hsw_ppgtt_decode_pde(entry) == 0x1234567000ULL);

    CHECK(idw_hsw_ppgtt_encode_pde(0x8000000000ULL, &entry) == 0);
    CHECK(entry == 0x00000801u);
    CHECK(idw_hsw_ppgtt_decode_pde(entry) == 0x8000000000ULL);

    CHECK(idw_hsw_ppgtt_encode_pde(0xfffffff000ULL, &entry) == 0);
    CHECK(entry == 0xfffffff1u);
    CHECK(idw_hsw_ppgtt_decode_pde(entry) == 0xfffffff000ULL);

    CHECK(idw_hsw_ppgtt_encode_pde(1ULL << 40, &entry) == -ERANGE);
    CHECK(idw_hsw_ppgtt_encode_pde(0x1001ULL, &entry) == -EINVAL);

    CHECK(idw_hsw_ppgtt_encode_pte(0x1234567000ULL, 0, &entry) == 0);
    CHECK(entry == 0x34567121u);
    CHECK(idw_hsw_ppgtt_decode_pte(entry) == 0x1234567000ULL);

    CHECK(idw_hsw_ppgtt_encode_pte(0x7ffffff000ULL, 0, &entry) == 0);
    CHECK(entry == 0xfffff7f1u);
    CHECK(idw_hsw_ppgtt_decode_pte(entry) == 0x7ffffff000ULL);

    CHECK(idw_hsw_ppgtt_encode_pte(1ULL << 39, 0, &entry) == -ERANGE);
    CHECK(idw_hsw_ppgtt_encode_pte(0x1001ULL, 0, &entry) == -EINVAL);

    CHECK(idw_hsw_ppgtt_encode_pte(0, 0x8, &entry) == 0);
    CHECK(entry == 0x00000801u);
    CHECK(idw_hsw_ppgtt_pte_cacheability(entry) == 0x8u);
    CHECK(idw_hsw_ppgtt_decode_pte(entry) == 0);

    CHECK(idw_hsw_ppgtt_encode_pte(0, 0xf, &entry) == 0);
    CHECK(entry == 0x0000080fu);
    CHECK(idw_hsw_ppgtt_pte_cacheability(entry) == 0xfu);
    CHECK(idw_hsw_ppgtt_encode_pte(0, 0x10, &entry) == -EINVAL);

    CHECK(idw_hsw_ppgtt_validate_pde_entry(0x0u) == 0);
    CHECK(idw_hsw_ppgtt_validate_pde_entry(0x1u) == 0);
    CHECK(idw_hsw_ppgtt_validate_pde_entry(0x4u) == -EINVAL);
    CHECK(idw_hsw_ppgtt_validate_pde_entry(0x2u) == -EOPNOTSUPP);
    CHECK(idw_hsw_ppgtt_validate_pte_entry(0xffffffffu) == 0);
    return 0;
}

static int test_geometry_and_lifecycle(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x00800000 };
    struct fake_directory fake;
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
    uint32_t hardware_pde = 0;

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_directory_ggtt_offset(model) == TEST_DIRECTORY_OFFSET);
    CHECK(fake.writes == IDW_HSW_PPGTT_PDES);
    CHECK(fake.reads == IDW_HSW_PPGTT_PDES);
    CHECK(fake.flushes == 1);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, 0, &hardware_pde) == 0);
    CHECK(hardware_pde == 0);

    CHECK(idw_hsw_ppgtt_map(model, va, 8192, dma_pages, 2) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0x00800001u);
    CHECK(idw_hsw_ppgtt_pde(model, 1) == 0x00801001u);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, 0, &hardware_pde) == 0);
    CHECK(hardware_pde == 0x00800001u);
    CHECK(idw_hsw_ppgtt_translate(model, va, &translation) == 0);
    CHECK(translation.pde_index == 0);
    CHECK(translation.pte_index == 1023);
    CHECK(translation.pte == 0x34567121u);
    CHECK(translation.dma_address == dma_pages[0].dma_address);
    CHECK(idw_hsw_ppgtt_translate(model, va + 8191, &translation) == 0);
    CHECK(translation.pde_index == 1);
    CHECK(translation.pte_index == 0);
    CHECK(translation.dma_address == dma_pages[1].dma_address + 4095);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -ENODEV);
    CHECK(idw_hsw_ppgtt_set_active(model, true) == -EAGAIN);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(pending.pending == 1);
    CHECK(pending.start == va && pending.bytes == 8192);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model,
                                               pending.generation - 1) == -ESTALE);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_set_active(model, true) == 0);
    CHECK(idw_hsw_ppgtt_check_submit(model) == 0);

    CHECK(idw_hsw_ppgtt_unmap(model, va, 4096) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0x00800001u);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, 0, &hardware_pde) == 0);
    CHECK(hardware_pde == 0x00800001u);
    CHECK(allocator.freed == 0);
    CHECK(idw_hsw_ppgtt_translate(model, va, &translation) == -ENOENT);
    CHECK(idw_hsw_ppgtt_translate(model, va + 4096, &translation) == 0);

    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == -EBUSY);
    CHECK(idw_hsw_ppgtt_set_active(model, false) == 0);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == -EAGAIN);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) != 0x00800001u);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, 0, &hardware_pde) == 0);
    CHECK(hardware_pde == idw_hsw_ppgtt_pde(model, 0));
    CHECK(allocator.freed == 0);
    CHECK(idw_hsw_ppgtt_set_active(model, true) == -EAGAIN);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(allocator.freed == 1);
    CHECK(idw_hsw_ppgtt_set_active(model, true) == 0);
    CHECK(idw_hsw_ppgtt_check_submit(model) == 0);

    CHECK(idw_hsw_ppgtt_set_active(model, false) == 0);
    CHECK(idw_hsw_ppgtt_map(model, va, 4096, dma_pages, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_unmap(model, va, 4096) == 0);
    CHECK(idw_hsw_ppgtt_unmap(model, va + 4096, 4096) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(allocator.allocated == allocator.freed + 1);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    CHECK(fake.frees == 1 && fake.allocated == 0);
    return 0;
}

static int test_invalid_maps_and_rollback(void)
{
    struct fake_page_allocator allocator = {
        .next_dma = 0x01000000,
        .fail_call = 2
    };
    struct fake_directory fake;
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
    const struct idw_dma_page_mapping bit_39[] = {
        TEST_DMA_PAGE(1ULL << 39)
    };
    struct idw_dma_page_mapping wrong_device[] = {
        TEST_DMA_PAGE(0x40000000)
    };
    const struct idw_dma_page_mapping one_page[] = {
        TEST_DMA_PAGE(0x40000000)
    };

    fake_directory_init(&fake);
    wrong_device[0].scope.device_cookie++;
    CHECK(create_model(&model, &allocator, &fake) == 0);
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
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, bad_alignment, 1) == -EINVAL);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, too_wide, 1) == -ERANGE);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, bit_39, 1) == -ERANGE);
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
    struct fake_directory fake;
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

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_unmap(model, 0, 4096) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) != 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_failed(model, pending.generation) == -EIO);
    CHECK(allocator.freed == 0);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -EIO);
    CHECK(idw_hsw_ppgtt_translate(model, 0, &translation) == -EIO);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == -EIO);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == -EIO);
    CHECK(idw_hsw_ppgtt_model_destroy_checked(model, false) == -EBUSY);
    CHECK(allocator.allocated != allocator.freed);
    CHECK(idw_hsw_ppgtt_model_destroy_checked(model, true) == 0);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_invalidation_generation_coalescing(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x03000000 };
    struct fake_directory fake;
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

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page_a, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    first_generation = pending.generation;
    CHECK(idw_hsw_ppgtt_map(model, 8ULL << 20, 4096, page_b, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(pending.generation == first_generation + 1);
    CHECK(pending.start == 0);
    CHECK(pending.bytes == (8ULL << 20) + 4096);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, first_generation) == -ESTALE);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -ENODEV);
    CHECK(idw_hsw_ppgtt_set_active(model, true) == -EAGAIN);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_set_active(model, true) == 0);
    CHECK(idw_hsw_ppgtt_check_submit(model) == 0);
    CHECK(idw_hsw_ppgtt_set_active(model, false) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_directory_residency_and_writes(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x06000000 };
    struct fake_directory fake;
    struct idw_hsw_ppgtt_model *model = NULL;
    const struct idw_dma_page_mapping page[] = {
        TEST_DMA_PAGE(0x60000000)
    };
    uint32_t hardware = 0;

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(fake.writes == IDW_HSW_PPGTT_PDES);
    CHECK(fake.reads == IDW_HSW_PPGTT_PDES);
    CHECK(fake.flushes == 1);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page, 1) == 0);
    CHECK(allocator.syncs == 2);
    CHECK(fake.writes == IDW_HSW_PPGTT_PDES + 1);
    CHECK(fake.flushes == 2);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0x06000001u);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, 0, &hardware) == 0);
    CHECK(hardware == 0x06000001u);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, IDW_HSW_PPGTT_PDES,
                                     &hardware) == -EINVAL);
    CHECK(idw_hsw_ppgtt_directory_ggtt_offset(model) == TEST_DIRECTORY_OFFSET);
    {
        struct idw_hsw_ppgtt_pending_invalidation pending = {
            .size = sizeof(pending)
        };

        CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
        CHECK(idw_hsw_ppgtt_invalidation_complete(model,
                                                  pending.generation) == 0);
    }
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(fake.frees == 1 && fake.allocated == 0);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_directory_validation(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x07000000 };
    struct fake_directory fake;
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_hsw_ppgtt_directory_ops directory_ops;
    struct idw_hsw_ppgtt_model *model = NULL;

    fake_directory_init(&fake);
    fake_ops_init(&page_ops, &allocator);
    fake_directory_ops_init(&directory_ops, &fake);
    CHECK(idw_hsw_ppgtt_model_create(NULL, &directory_ops, &model) == -EINVAL);
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, NULL, &model) == -EINVAL);
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, NULL) ==
          -EINVAL);

    directory_ops.scope.generation++;
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model) ==
          -EXDEV);
    directory_ops.scope = (struct idw_device_scope) TEST_DEVICE_SCOPE;

    fake.ggtt_offset = TEST_DIRECTORY_OFFSET + 1;
    CHECK(create_model(&model, &allocator, &fake) == -EINVAL);
    CHECK(fake.allocations == 1 && fake.frees == 1);

    fake_directory_init(&fake);
    fake.cookie = 0;
    CHECK(create_model(&model, &allocator, &fake) == -EINVAL);
    CHECK(fake.allocations == 1 && fake.frees == 1);

    fake_directory_init(&fake);
    fake.read_fail_call = 1;
    CHECK(create_model(&model, &allocator, &fake) == -EIO);
    CHECK(fake.allocations == 1 && fake.frees == 1);

    fake_directory_init(&fake);
    fake.flush_fail_call = 1;
    CHECK(create_model(&model, &allocator, &fake) == -EIO);
    CHECK(fake.allocations == 1 && fake.frees == 1);
    return 0;
}

static int test_reclaim_failure_rollback(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x08000000 };
    struct fake_directory fake;
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_pending_invalidation pending = {
        .size = sizeof(pending)
    };
    const struct idw_dma_page_mapping page[] = {
        TEST_DMA_PAGE(0x70000000)
    };
    uint32_t before;

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_unmap(model, 0, 4096) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    before = idw_hsw_ppgtt_pde(model, 0);

    fake.write_fail_call = fake.write_calls + 1;
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == -EIO);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == before);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -ENODEV);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) != before);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_model_address_boundaries(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0xfffffff000ULL };
    struct fake_directory fake;
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_translation translation = {
        .size = sizeof(translation)
    };
    const struct idw_dma_page_mapping max_page[] = {
        TEST_DMA_PAGE(0x7ffffff000ULL)
    };
    const struct idw_dma_page_mapping one_page[] = {
        TEST_DMA_PAGE(0x1000)
    };

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, max_page, 1) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0xfffffff1u);
    CHECK(idw_hsw_ppgtt_translate(model, 0, &translation) == 0);
    CHECK(translation.pte == 0xfffff7f1u);
    CHECK(translation.dma_address == 0x7ffffff000ULL);
    CHECK(drain_invalidation(model) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);

    allocator.next_dma = 1ULL << 40;
    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, one_page, 1) == -ERANGE);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);

    allocator.next_dma = 0x1001;
    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, one_page, 1) == -EINVAL);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

static int test_active_pde_publication_rejected(void)
{
    struct fake_page_allocator allocator = { .next_dma = 0x09000000 };
    struct fake_directory fake;
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_pending_invalidation pending = {
        .size = sizeof(pending)
    };
    struct idw_hsw_ppgtt_translation translation = {
        .size = sizeof(translation)
    };
    const struct idw_dma_page_mapping page_a[] = {
        TEST_DMA_PAGE(0x90000000)
    };
    const struct idw_dma_page_mapping page_b[] = {
        TEST_DMA_PAGE(0x90001000)
    };

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page_a, 1) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_set_active(model, true) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 4ULL << 22, 4096, page_b, 1) == -EBUSY);
    CHECK(idw_hsw_ppgtt_pde(model, 4) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 4096, 4096, page_b, 1) == 0);
    CHECK(idw_hsw_ppgtt_translate(model, 4096, &translation) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_set_active(model, false) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}


static int test_page_table_sync_failure(void)
{
    struct fake_page_allocator allocator = {
        .next_dma = 0x0a000000,
        .sync_fail_call = 2
    };
    struct fake_directory fake;
    struct idw_hsw_ppgtt_model *model = NULL;
    const struct idw_dma_page_mapping page[] = {
        TEST_DMA_PAGE(0xa0000000)
    };

    fake_directory_init(&fake);
    CHECK(create_model(&model, &allocator, &fake) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0, 4096, page, 1) == -EIO);
    CHECK(idw_hsw_ppgtt_check_submit(model) == -EIO);
    CHECK(allocator.syncs == 2);
    CHECK(idw_hsw_ppgtt_model_destroy_checked(model, false) == 0);
    CHECK(allocator.allocated == allocator.freed);
    return 0;
}

int main(void)
{
    if (test_gpuva_space() || test_golden_encoding() ||
        test_geometry_and_lifecycle() || test_invalid_maps_and_rollback() ||
        test_failed_invalidation_poison() ||
        test_page_table_sync_failure() ||
        test_invalidation_generation_coalescing() ||
        test_directory_residency_and_writes() ||
        test_directory_validation() || test_reclaim_failure_rollback() ||
        test_model_address_boundaries() ||
        test_active_pde_publication_rejected())
        return 1;
    puts("PASS: CPU-only GPU-VA allocator and Haswell Gen7.5 PPGTT model tests");
    puts("NOTE: synthetic DMA addresses validate software structures only; no GPU mapping or hardware control was tested.");
    return 0;
}
