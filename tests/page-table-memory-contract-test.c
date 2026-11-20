/* Contract validation only; descriptors use synthetic identifiers and addresses. */
#include "../include/idw/contracts/i915_assisted.h"
#include "../include/idw/contracts/native_dma.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int no_op_page_alloc(void *context, struct idw_dma_page *page)
{
    (void)context;
    (void)page;
    return -ENOSYS;
}

static void no_op_page_free(void *context, struct idw_dma_page *page)
{
    (void)context;
    (void)page;
}

static int no_op_map(void *context, idw_handle allocation, uint64_t offset,
                     uint64_t bytes, struct idw_dma_segment *segments,
                     uint32_t capacity, uint32_t *count)
{
    (void)context;
    (void)allocation;
    (void)offset;
    (void)bytes;
    (void)segments;
    (void)capacity;
    (void)count;
    return -ENOSYS;
}

static void no_op_unmap(void *context, idw_handle allocation,
                        const struct idw_dma_segment *segments,
                        uint32_t count)
{
    (void)context;
    (void)allocation;
    (void)segments;
    (void)count;
}

static int no_op_sync(void *context, const struct idw_dma_page *page,
                      uint32_t offset, uint32_t bytes)
{
    (void)context;
    (void)page;
    (void)offset;
    (void)bytes;
    return -ENOSYS;
}

static int no_op_query_caps(uint32_t adapter_id,
                            struct idw_gen75_ppgtt_caps *caps)
{
    (void)adapter_id;
    (void)caps;
    return -ENOSYS;
}

static int no_op_invalidate(uint32_t adapter_id, idw_handle vm,
                            idw_gpuva start, uint64_t bytes)
{
    (void)adapter_id;
    (void)vm;
    (void)start;
    (void)bytes;
    return -ENOSYS;
}

static int no_op_vm_engine(uint32_t adapter_id, idw_handle vm,
                           uint32_t engine_id)
{
    (void)adapter_id;
    (void)vm;
    (void)engine_id;
    return -ENOSYS;
}

static int test_i915_boundary(void)
{
    const struct idw_device_scope scope = {
        .device_cookie = 0x11,
        .generation = 7
    };
    struct idw_pt_backend_desc backend = {0};
    struct idw_i915_assisted_contract assisted = {0};

    CHECK(idw_i915_assisted_backend_init(&scope, &backend, &assisted) == 0);
    CHECK(idw_pt_backend_validate(&backend, IDW_PT_CAP_CONTEXT_VM_BIND) == 0);
    CHECK(idw_pt_backend_validate(&backend, IDW_PT_CAP_DMA_PAGE_TABLE) ==
          -EOPNOTSUPP);
    CHECK(assisted.size == sizeof(assisted));
    CHECK(assisted.version == IDW_I915_ASSISTED_CONTRACT_VERSION);
    CHECK(!(backend.capabilities & IDW_PT_CAP_DMA_PAGE_TABLE));
    CHECK(!(backend.capabilities & IDW_PT_CAP_SYNC_FOR_DEVICE));
    CHECK(!(backend.capabilities & IDW_PT_CAP_GEN75_INVALIDATE));

    backend.capabilities |= IDW_PT_CAP_DMA_PAGE_TABLE;
    CHECK(idw_pt_backend_validate(&backend, 0) == -EPERM);
    return 0;
}

static int test_native_scope_and_contract(void)
{
    const struct idw_device_scope scope = {
        .device_cookie = 0x22,
        .generation = 3
    };
    struct idw_pt_backend_desc backend = {
        .size = sizeof(backend),
        .version = IDW_PT_MEMORY_VERSION,
        .kind = IDW_PT_BACKEND_NATIVE_DMA,
        .flags = IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER,
        .capabilities = IDW_PT_CAP_DMA_PAGE_TABLE |
                        IDW_PT_CAP_DMA_RESOURCE_MAPPING |
                        IDW_PT_CAP_SYNC_FOR_DEVICE |
                        IDW_PT_CAP_GEN75_INVALIDATE |
                        IDW_PT_CAP_GEN75_VM_ACTIVATE,
        .scope = { .device_cookie = 0x22, .generation = 3 }
    };
    struct idw_native_dma_contract native = {
        .size = sizeof(native),
        .version = IDW_NATIVE_DMA_CONTRACT_VERSION,
        .dma_address_bits = 40,
        .backend = backend,
        .memory = {
            .size = sizeof(struct idw_pt_memory_ops),
            .version = IDW_PT_MEMORY_VERSION,
            .alloc_page_table_page = no_op_page_alloc,
            .free_page_table_page = no_op_page_free,
            .map_allocation = no_op_map,
            .unmap_allocation = no_op_unmap,
            .sync_page_table_for_device = no_op_sync
        },
        .ppgtt = {
            .size = sizeof(struct idw_gen75_ppgtt_ops),
            .version = IDW_PT_MEMORY_VERSION,
            .query_caps = no_op_query_caps,
            .invalidate_vm = no_op_invalidate,
            .activate_vm = no_op_vm_engine,
            .deactivate_vm = no_op_vm_engine
        }
    };
    struct idw_dma_page_mapping mapping = {
        .dma_address = 0x12345000,
        .bytes = IDW_PT_PAGE_BYTES,
        .scope = { .device_cookie = 0x22, .generation = 3 },
        .mapping_cookie = 0x100
    };

    CHECK(idw_pt_backend_validate(&backend,
          IDW_PT_CAP_DMA_PAGE_TABLE | IDW_PT_CAP_SYNC_FOR_DEVICE) == 0);
    CHECK(idw_pt_mapping_validate(&backend, &mapping) == 0);
    CHECK(idw_native_dma_contract_validate(&native) == 0);

    mapping.scope.device_cookie = scope.device_cookie + 1;
    CHECK(idw_pt_mapping_validate(&backend, &mapping) == -EXDEV);
    mapping.scope = scope;
    mapping.mapping_cookie = 0;
    CHECK(idw_pt_mapping_validate(&backend, &mapping) == -EINVAL);

    native.backend.flags = 0;
    CHECK(idw_native_dma_contract_validate(&native) == -EPERM);
    native.backend = backend;
    native.backend.capabilities &= ~IDW_PT_CAP_SYNC_FOR_DEVICE;
    CHECK(idw_native_dma_contract_validate(&native) == -EINVAL);
    return 0;
}

static int test_page_validation(void)
{
    struct idw_pt_backend_desc backend = {
        .size = sizeof(backend),
        .version = IDW_PT_MEMORY_VERSION,
        .kind = IDW_PT_BACKEND_NATIVE_DMA,
        .flags = IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER,
        .capabilities = IDW_PT_CAP_DMA_PAGE_TABLE |
                        IDW_PT_CAP_DMA_RESOURCE_MAPPING |
                        IDW_PT_CAP_SYNC_FOR_DEVICE,
        .scope = { .device_cookie = 0x33, .generation = 1 }
    };
    struct idw_dma_page page = {
        .size = sizeof(page),
        .memory_type = IDW_PT_PAGE_COHERENT,
        .dma_address = 0x200000,
        .bytes = IDW_PT_PAGE_BYTES,
        .scope = { .device_cookie = 0x33, .generation = 1 },
        .allocation_cookie = 0x44
    };

    page.cpu_address = aligned_alloc(IDW_PT_PAGE_BYTES, IDW_PT_PAGE_BYTES);
    CHECK(page.cpu_address != NULL);
    CHECK(idw_pt_page_validate(&backend, &page) == 0);
    page.scope.generation++;
    CHECK(idw_pt_page_validate(&backend, &page) == -EXDEV);
    page.scope = backend.scope;
    page.dma_address = 1ULL << 40;
    CHECK(idw_pt_page_validate(&backend, &page) == -EINVAL);
    free(page.cpu_address);
    return 0;
}

int main(void)
{
    if (test_i915_boundary() || test_native_scope_and_contract() ||
        test_page_validation())
        return 1;
    puts("PASS: page-table backend capability and device-scope contract tests");
    puts("NOTE: synthetic descriptors validate contracts only; no DMA or GPU operation occurred.");
    return 0;
}
