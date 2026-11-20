/* Software tests only: synthetic DMA addresses, no device or GPU operation. */
#include "../include/idw/pt_dma_adapter.h"
#include "../include/idw/contracts/native_dma.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define FAKE_MAX_PAGES 16
#define FAKE_SCOPE_COOKIE 0x4944570000002001ULL
#define FAKE_SCOPE_GENERATION 7ULL

struct fake_page {
    struct idw_dma_page page;
    int live;
};

struct fake_provider {
    struct idw_device_scope scope;
    uint64_t next_dma;
    unsigned int calls;
    unsigned int allocated;
    unsigned int freed;
    unsigned int sync_calls;
    unsigned int alloc_fail_call;
    unsigned int wrong_scope_call;
    unsigned int bad_dma_call;
    unsigned int bad_align_call;
    unsigned int bad_cookie_call;
    unsigned int bad_type_call;
    unsigned int bad_bytes_call;
    int free_fail_once;
    int sync_fail;
    struct fake_page pages[FAKE_MAX_PAGES];
};

static int fake_alloc(void *context, struct idw_dma_page *page)
{
    struct fake_provider *fake = context;
    struct fake_page *slot = NULL;
    void *memory = NULL;
    unsigned int i;

    ++fake->calls;
    if (fake->alloc_fail_call == fake->calls)
        return -ENOMEM;
    for (i = 0; i < FAKE_MAX_PAGES; ++i) {
        if (!fake->pages[i].live) {
            slot = &fake->pages[i];
            break;
        }
    }
    if (!slot)
        return -ENOSPC;
    memory = aligned_alloc(IDW_PT_PAGE_BYTES, IDW_PT_PAGE_BYTES);
    if (!memory)
        return -ENOMEM;
    memset(memory, 0, IDW_PT_PAGE_BYTES);

    page->size = sizeof(*page);
    page->memory_type = IDW_PT_PAGE_COHERENT;
    page->cpu_address = memory;
    page->dma_address = fake->next_dma;
    page->bytes = IDW_PT_PAGE_BYTES;
    page->scope = fake->scope;
    page->allocation_cookie = fake->calls;

    if (fake->wrong_scope_call == fake->calls)
        ++page->scope.generation;
    if (fake->bad_dma_call == fake->calls)
        page->dma_address = 1ULL << 40;
    if (fake->bad_align_call == fake->calls)
        ++page->dma_address;
    if (fake->bad_cookie_call == fake->calls)
        page->allocation_cookie = 0;
    if (fake->bad_type_call == fake->calls)
        page->memory_type = 0;
    if (fake->bad_bytes_call == fake->calls)
        page->bytes = 0;

    slot->page = *page;
    slot->live = 1;
    fake->next_dma += IDW_PT_PAGE_BYTES;
    ++fake->allocated;
    return 0;
}

static int fake_free(void *context, struct idw_dma_page *page)
{
    struct fake_provider *fake = context;
    unsigned int i;

    if (fake->free_fail_once) {
        fake->free_fail_once = 0;
        return -EIO;
    }
    for (i = 0; i < FAKE_MAX_PAGES; ++i) {
        if (fake->pages[i].live &&
            fake->pages[i].page.allocation_cookie == page->allocation_cookie) {
            free(fake->pages[i].page.cpu_address);
            fake->pages[i].live = 0;
            ++fake->freed;
            return 0;
        }
    }
    return -ENOENT;
}

static int fake_sync(void *context, const struct idw_dma_page *page,
                     uint32_t offset, uint32_t bytes)
{
    struct fake_provider *fake = context;

    (void)page;
    (void)offset;
    (void)bytes;
    ++fake->sync_calls;
    return fake->sync_fail;
}

static unsigned int fake_live(const struct fake_provider *fake)
{
    unsigned int i;
    unsigned int live = 0;

    for (i = 0; i < FAKE_MAX_PAGES; ++i)
        live += fake->pages[i].live ? 1u : 0u;
    return live;
}

static void fake_init(struct fake_provider *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->scope.device_cookie = FAKE_SCOPE_COOKIE;
    fake->scope.generation = FAKE_SCOPE_GENERATION;
    fake->next_dma = 0x20000000;
}

struct fake_directory {
    struct idw_device_scope scope;
    uint32_t hardware[IDW_HSW_PPGTT_PDES];
    int allocated;
    unsigned int allocations;
    unsigned int frees;
};

static int fake_directory_alloc(void *context,
                                struct idw_hsw_ppgtt_directory *directory)
{
    struct fake_directory *fake = context;

    if (fake->allocated)
        return -EBUSY;
    memset(fake->hardware, 0, sizeof(fake->hardware));
    directory->size = sizeof(*directory);
    directory->version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    directory->ggtt_offset = 0x40000;
    directory->bytes = IDW_HSW_PPGTT_DIRECTORY_BYTES;
    directory->scope = fake->scope;
    directory->allocation_cookie = 0x4449524543544f52ULL;
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
    if (index >= IDW_HSW_PPGTT_PDES)
        return -EINVAL;
    fake->hardware[index] = pde;
    return 0;
}

static int fake_directory_read(void *context,
                               const struct idw_hsw_ppgtt_directory *directory,
                               uint32_t index, uint32_t *pde)
{
    struct fake_directory *fake = context;

    (void)directory;
    if (index >= IDW_HSW_PPGTT_PDES || !pde)
        return -EINVAL;
    *pde = fake->hardware[index];
    return 0;
}

static int fake_directory_flush(void *context,
                                const struct idw_hsw_ppgtt_directory *directory)
{
    (void)context;
    (void)directory;
    return 0;
}

static void fake_directory_init(struct fake_directory *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->scope.device_cookie = FAKE_SCOPE_COOKIE;
    fake->scope.generation = FAKE_SCOPE_GENERATION;
}

static void fake_directory_ops_init(struct idw_hsw_ppgtt_directory_ops *ops,
                                    struct fake_directory *fake)
{
    *ops = (struct idw_hsw_ppgtt_directory_ops) {
        .size = sizeof(*ops),
        .version = IDW_HSW_PPGTT_DIRECTORY_VERSION,
        .scope = { FAKE_SCOPE_COOKIE, FAKE_SCOPE_GENERATION },
        .alloc_directory = fake_directory_alloc,
        .free_directory = fake_directory_free,
        .write_pde_gttaddr = fake_directory_write,
        .read_pde_gttaddr = fake_directory_read,
        .flush_gtt = fake_directory_flush,
        .context = fake
    };
}

static void fake_ops_init(struct idw_pt_dma_provider_ops *ops)
{
    *ops = (struct idw_pt_dma_provider_ops) {
        .size = sizeof(*ops),
        .version = IDW_PT_DMA_PROVIDER_VERSION,
        .alloc_page_table_page = fake_alloc,
        .free_page_table_page = fake_free,
        .sync_page_for_device = fake_sync
    };
}

static int adapter_start(struct idw_pt_dma_adapter *adapter,
                         struct fake_provider *fake)
{
    struct idw_pt_dma_provider_ops ops;

    fake_ops_init(&ops);
    return idw_pt_dma_adapter_init(adapter, &ops, fake, &fake->scope);
}

static int test_adapter_validation(void)
{
    struct fake_provider fake;
    struct idw_pt_dma_adapter adapter = {0};
    struct idw_pt_dma_provider_ops ops;
    struct idw_pt_backend_desc backend;
    struct idw_pt_memory_ops memory;
    struct idw_dma_page page = {0};
    struct idw_dma_page bad;
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_dma_page_mapping mapping = {
        .dma_address = 0x20000000,
        .bytes = IDW_PT_PAGE_BYTES,
        .scope = { FAKE_SCOPE_COOKIE, FAKE_SCOPE_GENERATION },
        .mapping_cookie = 0x100
    };
    unsigned int i;

    fake_init(&fake);
    fake_ops_init(&ops);
    CHECK(idw_pt_dma_adapter_init(NULL, &ops, &fake, &fake.scope) == -EINVAL);
    CHECK(idw_pt_dma_adapter_init(&adapter, NULL, &fake, &fake.scope) ==
          -EINVAL);
    CHECK(idw_pt_dma_adapter_init(&adapter, &ops, NULL, &fake.scope) ==
          -EINVAL);
    CHECK(idw_pt_dma_adapter_init(&adapter, &ops, &fake, NULL) == -EINVAL);
    ops.alloc_page_table_page = NULL;
    CHECK(idw_pt_dma_adapter_init(&adapter, &ops, &fake, &fake.scope) ==
          -EINVAL);
    fake_ops_init(&ops);
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(adapter_start(&adapter, &fake) == -EBUSY);
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0);

    page.size = sizeof(page);
    CHECK(page_ops.alloc_page_table_page(page_ops.context, &page) == 0);
    CHECK(page.size == sizeof(page));
    CHECK(page.bytes == IDW_PT_PAGE_BYTES);
    CHECK(page.dma_address == 0x20000000);
    CHECK(page.scope.device_cookie == FAKE_SCOPE_COOKIE);
    CHECK(page.scope.generation == FAKE_SCOPE_GENERATION);
    CHECK(page.allocation_cookie != 0);
    CHECK(adapter.outstanding == 1);
    for (i = 0; i < IDW_PT_PAGE_BYTES; ++i)
        CHECK(((uint8_t *)page.cpu_address)[i] == 0);

    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &page) == 0);
    bad = page;
    bad.scope.generation++;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EXDEV);
    bad = page;
    bad.scope.device_cookie++;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EXDEV);
    bad = page;
    bad.dma_address = 1ULL << 40;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -ERANGE);
    bad = page;
    bad.dma_address++;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EINVAL);
    bad = page;
    bad.bytes = 0;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EINVAL);
    bad = page;
    bad.bytes = 2 * IDW_PT_PAGE_BYTES;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EINVAL);
    bad = page;
    bad.allocation_cookie = 0;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EINVAL);
    bad = page;
    bad.memory_type = 0;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EINVAL);
    bad = page;
    bad.size = 0;
    CHECK(idw_pt_dma_adapter_validate_page(&adapter, &bad) == -EINVAL);

    CHECK(idw_pt_dma_adapter_backend(&adapter, &backend) == 0);
    CHECK(backend.size == sizeof(backend));
    CHECK(backend.version == IDW_PT_MEMORY_VERSION);
    CHECK(backend.kind == IDW_PT_BACKEND_NATIVE_DMA);
    CHECK(backend.flags == (IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER |
                            IDW_PT_BACKEND_PAGE_TABLE_ONLY));
    CHECK(backend.capabilities == (IDW_PT_CAP_DMA_PAGE_TABLE |
                                   IDW_PT_CAP_SYNC_FOR_DEVICE));
    CHECK(idw_pt_backend_validate(&backend, IDW_PT_CAP_DMA_PAGE_TABLE |
                                             IDW_PT_CAP_SYNC_FOR_DEVICE) == 0);
    CHECK(idw_pt_backend_validate(&backend, IDW_PT_CAP_DMA_RESOURCE_MAPPING) ==
          -EOPNOTSUPP);
    CHECK(idw_pt_mapping_validate(&backend, &mapping) == -EOPNOTSUPP);

    idw_pt_dma_adapter_memory_ops(&adapter, &memory);
    CHECK(memory.size == sizeof(memory));
    CHECK(memory.version == IDW_PT_MEMORY_VERSION);
    CHECK(memory.alloc_page_table_page && memory.free_page_table_page);
    CHECK(memory.map_allocation && memory.unmap_allocation);
    CHECK(memory.sync_page_table_for_device);
    CHECK(memory.map_allocation(&adapter, 1, 0, IDW_PT_PAGE_BYTES,
                                NULL, 0, NULL) == -EOPNOTSUPP);
    memory.unmap_allocation(&adapter, 1, NULL, 0);
    CHECK(memory.sync_page_table_for_device(&adapter, &page, 0,
                                            IDW_PT_PAGE_BYTES) == 0);
    CHECK(fake.sync_calls == 1);
    CHECK(memory.sync_page_table_for_device(&adapter, &page,
                                            IDW_PT_PAGE_BYTES, 1) == -EINVAL);
    fake.sync_fail = -EIO;
    CHECK(memory.sync_page_table_for_device(&adapter, &page, 0, 1) == -EIO);
    fake.sync_fail = 0;

    CHECK(idw_pt_dma_adapter_release_page(&adapter, &page) == 0);
    CHECK(adapter.outstanding == 0);
    CHECK(fake.freed == 1);
    CHECK(fake_live(&fake) == 0);
    CHECK(idw_pt_dma_adapter_release_page(&adapter, &page) == -ENOENT);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == -EINVAL);
    return 0;
}

static int test_alloc_rollback(void)
{
    struct fake_provider fake;
    struct idw_pt_dma_adapter adapter;
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_dma_page page = {0};
    int ret;

    fake_init(&fake);
    fake.alloc_fail_call = 1;
    memset(&adapter, 0, sizeof(adapter));
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0);
    CHECK(page_ops.alloc_page_table_page(page_ops.context, &page) == -ENOMEM);
    CHECK(adapter.outstanding == 0 && fake_live(&fake) == 0);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);

#define ROLLBACK_CASE(field, expected) do { \
    fake_init(&fake); \
    fake.field = 1; \
    memset(&adapter, 0, sizeof(adapter)); \
    CHECK(adapter_start(&adapter, &fake) == 0); \
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0); \
    memset(&page, 0, sizeof(page)); \
    ret = page_ops.alloc_page_table_page(page_ops.context, &page); \
    CHECK(ret == expected); \
    CHECK(adapter.outstanding == 0); \
    CHECK(fake_live(&fake) == 0); \
    CHECK(fake.freed == 1); \
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0); \
} while (0)

    ROLLBACK_CASE(wrong_scope_call, -EXDEV);
    ROLLBACK_CASE(bad_dma_call, -ERANGE);
    ROLLBACK_CASE(bad_align_call, -EINVAL);
    ROLLBACK_CASE(bad_cookie_call, -EINVAL);
    ROLLBACK_CASE(bad_type_call, -EINVAL);
    ROLLBACK_CASE(bad_bytes_call, -EINVAL);
#undef ROLLBACK_CASE
    return 0;
}

static int test_ppgtt_integration(void)
{
    const uint64_t va = (1ULL << 22) - IDW_HASWELL_PPGTT_PAGE_SIZE;
    struct fake_provider fake;
    struct fake_directory directory;
    struct idw_pt_dma_adapter adapter = {0};
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_hsw_ppgtt_directory_ops directory_ops;
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_hsw_ppgtt_translation translation = {
        .size = sizeof(translation)
    };
    struct idw_hsw_ppgtt_pending_invalidation pending = {
        .size = sizeof(pending)
    };
    const struct idw_dma_page_mapping mappings[] = {
        {
            .dma_address = 0x30000000,
            .bytes = IDW_PT_PAGE_BYTES,
            .scope = { FAKE_SCOPE_COOKIE, FAKE_SCOPE_GENERATION },
            .mapping_cookie = 0x3001
        },
        {
            .dma_address = 0x30001000,
            .bytes = IDW_PT_PAGE_BYTES,
            .scope = { FAKE_SCOPE_COOKIE, FAKE_SCOPE_GENERATION },
            .mapping_cookie = 0x3002
        }
    };

    fake_init(&fake);
    fake_directory_init(&directory);
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0);
    fake_directory_ops_init(&directory_ops, &directory);
    CHECK(page_ops.size == sizeof(page_ops));
    CHECK(page_ops.version == 1);
    CHECK(page_ops.scope.device_cookie == FAKE_SCOPE_COOKIE);
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model) == 0);
    CHECK(page_ops.sync_page_table_for_device != NULL);
    CHECK(idw_hsw_ppgtt_map(model, va, 2 * IDW_HASWELL_PPGTT_PAGE_SIZE,
                            mappings, 2) == 0);
    CHECK(fake.sync_calls > 0);
    CHECK(fake.allocated == 2 && fake_live(&fake) == 2);
    CHECK(adapter.outstanding == 2);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0x20000001u);
    CHECK(idw_hsw_ppgtt_pde(model, 1) == 0x20001001u);
    CHECK(((uint32_t *)fake.pages[0].page.cpu_address)[1023] == 0x30000001u);
    CHECK(((uint32_t *)fake.pages[1].page.cpu_address)[0] == 0x30001001u);
    CHECK(idw_hsw_ppgtt_translate(model, va, &translation) == 0);
    CHECK(translation.dma_address == mappings[0].dma_address);
    CHECK(idw_hsw_ppgtt_translate(model,
                                  va + IDW_HASWELL_PPGTT_PAGE_SIZE,
                                  &translation) == 0);
    CHECK(translation.dma_address == mappings[1].dma_address);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);

    CHECK(idw_hsw_ppgtt_unmap(model, va, IDW_HASWELL_PPGTT_PAGE_SIZE) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0x20000001u);
    CHECK(adapter.outstanding == 2);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 0) != 0x20000001u);
    CHECK(adapter.outstanding == 3 && fake.freed == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(adapter.outstanding == 2 && fake.freed == 1);

    CHECK(idw_hsw_ppgtt_unmap(model,
                              va + IDW_HASWELL_PPGTT_PAGE_SIZE,
                              IDW_HASWELL_PPGTT_PAGE_SIZE) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == 0);
    CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
    CHECK(idw_hsw_ppgtt_invalidation_complete(model, pending.generation) == 0);
    CHECK(adapter.outstanding == 1 && fake.freed == 2);

    idw_hsw_ppgtt_model_destroy(model);
    CHECK(fake.allocated == fake.freed);
    CHECK(adapter.outstanding == 0 && fake_live(&fake) == 0);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);
    return 0;
}

static int test_ppgtt_rollback_and_scope(void)
{
    const uint64_t va = (1ULL << 22) - IDW_HASWELL_PPGTT_PAGE_SIZE;
    const struct idw_dma_page_mapping mappings[] = {
        {
            .dma_address = 0x40000000,
            .bytes = IDW_PT_PAGE_BYTES,
            .scope = { FAKE_SCOPE_COOKIE, FAKE_SCOPE_GENERATION },
            .mapping_cookie = 0x4001
        },
        {
            .dma_address = 0x40001000,
            .bytes = IDW_PT_PAGE_BYTES,
            .scope = { FAKE_SCOPE_COOKIE, FAKE_SCOPE_GENERATION },
            .mapping_cookie = 0x4002
        }
    };
    struct fake_provider fake;
    struct fake_directory directory;
    struct idw_pt_dma_adapter adapter;
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_hsw_ppgtt_directory_ops directory_ops;
    struct idw_hsw_ppgtt_model *model = NULL;

    fake_init(&fake);
    fake_directory_init(&directory);
    fake.alloc_fail_call = 2;
    memset(&adapter, 0, sizeof(adapter));
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0);
    fake_directory_ops_init(&directory_ops, &directory);
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model) == 0);
    CHECK(idw_hsw_ppgtt_map(model, va, 2 * IDW_HASWELL_PPGTT_PAGE_SIZE,
                            mappings, 2) == -ENOMEM);
    CHECK(idw_hsw_ppgtt_pde(model, 0) == 0);
    CHECK(idw_hsw_ppgtt_pde(model, 1) == 0);
    CHECK(adapter.outstanding == 0 && fake_live(&fake) == 0);
    CHECK(fake.allocated == 1 && fake.freed == 1);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);

    fake_init(&fake);
    fake_directory_init(&directory);
    fake.wrong_scope_call = 1;
    memset(&adapter, 0, sizeof(adapter));
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0);
    fake_directory_ops_init(&directory_ops, &directory);
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model) == 0);
    CHECK(idw_hsw_ppgtt_map(model, va, IDW_HASWELL_PPGTT_PAGE_SIZE,
                            mappings, 1) == -EXDEV);
    CHECK(adapter.outstanding == 0 && fake_live(&fake) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);

    fake_init(&fake);
    fake_directory_init(&directory);
    fake.bad_dma_call = 1;
    memset(&adapter, 0, sizeof(adapter));
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0);
    fake_directory_ops_init(&directory_ops, &directory);
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model) == 0);
    CHECK(idw_hsw_ppgtt_map(model, va, IDW_HASWELL_PPGTT_PAGE_SIZE,
                            mappings, 1) == -ERANGE);
    CHECK(adapter.outstanding == 0 && fake_live(&fake) == 0);
    idw_hsw_ppgtt_model_destroy(model);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);
    return 0;
}

static int test_fini_and_contract_boundary(void)
{
    struct fake_provider fake;
    struct idw_pt_dma_adapter adapter = {0};
    struct idw_dma_page page = {0};
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_pt_backend_desc backend;
    struct idw_native_dma_contract native = {
        .size = sizeof(native),
        .version = IDW_NATIVE_DMA_CONTRACT_VERSION,
        .dma_address_bits = 40
    };

    fake_init(&fake);
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(idw_pt_dma_adapter_page_ops(&adapter, &page_ops) == 0);
    page.size = sizeof(page);
    CHECK(page_ops.alloc_page_table_page(page_ops.context, &page) == 0);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == -EBUSY);
    CHECK(idw_pt_dma_adapter_release_page(&adapter, &page) == 0);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);

    CHECK(idw_pt_dma_adapter_backend(&adapter, &backend) == -EINVAL);
    fake_init(&fake);
    CHECK(adapter_start(&adapter, &fake) == 0);
    CHECK(idw_pt_dma_adapter_backend(&adapter, &backend) == 0);

    backend.flags |= IDW_PT_BACKEND_PAGE_TABLE_ONLY;
    backend.capabilities |= IDW_PT_CAP_DMA_RESOURCE_MAPPING;
    CHECK(idw_pt_backend_validate(&backend, 0) == -EPERM);

    backend.flags = IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER;
    backend.capabilities = IDW_PT_CAP_DMA_PAGE_TABLE |
                           IDW_PT_CAP_SYNC_FOR_DEVICE;
    CHECK(idw_pt_backend_validate(&backend, 0) == -EINVAL);

    native.backend = backend;
    native.backend.kind = IDW_PT_BACKEND_NATIVE_DMA;
    native.backend.flags = IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER |
                           IDW_PT_BACKEND_PAGE_TABLE_ONLY;
    native.backend.capabilities = IDW_PT_CAP_DMA_PAGE_TABLE |
                                  IDW_PT_CAP_DMA_RESOURCE_MAPPING |
                                  IDW_PT_CAP_SYNC_FOR_DEVICE |
                                  IDW_PT_CAP_GEN75_INVALIDATE |
                                  IDW_PT_CAP_GEN75_VM_ACTIVATE;
    CHECK(idw_native_dma_contract_validate(&native) == -EPERM);
    native.backend.capabilities = IDW_PT_CAP_DMA_PAGE_TABLE |
                                  IDW_PT_CAP_SYNC_FOR_DEVICE;
    CHECK(idw_native_dma_contract_validate(&native) == -EPERM);
    CHECK(idw_pt_dma_adapter_fini(&adapter) == 0);
    return 0;
}

int main(void)
{
    if (test_adapter_validation() || test_alloc_rollback() ||
        test_ppgtt_integration() || test_ppgtt_rollback_and_scope() ||
        test_fini_and_contract_boundary())
        return 1;
    puts("PASS: DMA page-table adapter validation, rollback, sync, cleanup, and PPGTT integration");
    puts("NOTE: synthetic DMA addresses and simulated provider; no device or GPU operation occurred.");
    return 0;
}
