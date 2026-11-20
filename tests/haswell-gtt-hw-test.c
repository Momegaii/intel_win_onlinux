/* SPDX-License-Identifier: MIT */
#include "idw/ggtt_allocator.h"
#include "idw/ggtt_directory.h"
#include "idw/haswell_gtt_hw.h"
#include "idw/haswell_ppgtt_model.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

#define TEST_GGTT_BYTES (2u * 1024u * 1024u)

struct fake_pages {
    struct idw_dma_page pages[32];
    uint32_t count;
    uint32_t frees;
    struct idw_device_scope scope;
};

static int fake_alloc_page(void *context, struct idw_dma_page *page)
{
    struct fake_pages *pages = context;
    void *memory;

    if (!page || pages->count >= 32)
        return -ENOMEM;
    memory = aligned_alloc(IDW_GGTT_PAGE_BYTES, IDW_GGTT_PAGE_BYTES);
    if (!memory)
        return -ENOMEM;
    memset(memory, 0, IDW_GGTT_PAGE_BYTES);
    page->size = sizeof(*page);
    page->memory_type = IDW_PT_PAGE_COHERENT;
    page->cpu_address = memory;
    page->dma_address = 0x100000u + pages->count * IDW_GGTT_PAGE_BYTES;
    page->bytes = IDW_GGTT_PAGE_BYTES;
    page->scope = pages->scope;
    page->allocation_cookie = 0x5000u + pages->count;
    ++pages->count;
    return 0;
}

static void fake_free_page(void *context, struct idw_dma_page *page)
{
    struct fake_pages *pages = context;

    if (!page)
        return;
    free(page->cpu_address);
    memset(page, 0, sizeof(*page));
    ++pages->frees;
}

static int test_register_vectors(void)
{
    CHECK(IDW_HSW_REG_PGTBL_CTL == 0x2020u);
    CHECK(IDW_HSW_REG_INSTPM == 0x20c0u);
    CHECK(IDW_HSW_REG_PP_DCLV == 0x2220u);
    CHECK(IDW_HSW_REG_PP_DIR_BASE == 0x2228u);
    CHECK(IDW_HSW_REG_RING_MODE_GEN7 == 0x229cu);
    CHECK(IDW_HSW_REG_ECOCHK == 0x4090u);
    CHECK(IDW_HSW_REG_GAC_ECO_BITS == 0x14090u);
    CHECK(IDW_HSW_REG_GFX_FLSH_CNTL_GEN6 == 0x101008u);
    CHECK(IDW_HSW_RING_MODE_PPGTT_ENABLE == (1u << 9));
    CHECK(IDW_HSW_INSTPM_TLB_INVALIDATE == (1u << 9));
    CHECK(IDW_HSW_INSTPM_SYNC_FLUSH == (1u << 5));
    CHECK(IDW_HSW_GFX_FLSH_CNTL_GEN6_FLUSH == 1u);
    CHECK(IDW_HSW_PP_DIR_DCLV_2G == 0xffffffffu);
    return 0;
}

static int test_directory_and_activation(void)
{
    struct idw_device_scope scope = { 0x1234, 1 };
    struct idw_ggtt_config ggtt_config;
    struct idw_ggtt *ggtt = NULL;
    struct idw_hsw_hw *hw = NULL;
    struct idw_ggtt_directory_provider provider;
    struct idw_hsw_ppgtt_directory_ops directory_ops;
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_hsw_ppgtt_model *model = NULL;
    struct idw_dma_page_mapping mapping;
    struct idw_hsw_ppgtt_translation translation;
    struct fake_pages pages;
    uint32_t pde;
    uint32_t value;
    uint64_t directory_offset;

    memset(&pages, 0, sizeof(pages));
    pages.scope = scope;
    memset(&translation, 0, sizeof(translation));
    translation.size = sizeof(translation);
    memset(&mapping, 0, sizeof(mapping));
    mapping.dma_address = 0x300000;
    mapping.bytes = IDW_GGTT_PAGE_BYTES;
    mapping.scope = scope;
    mapping.mapping_cookie = 0x9001;

    ggtt_config.size = sizeof(ggtt_config);
    ggtt_config.version = IDW_GGTT_VERSION;
    ggtt_config.base_offset = 0;
    ggtt_config.bytes = TEST_GGTT_BYTES;
    ggtt_config.page_bytes = IDW_GGTT_PAGE_BYTES;
    ggtt_config.max_allocations = 64;
    CHECK(idw_ggtt_create(&ggtt_config, &ggtt) == 0);
    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    {
        struct idw_ggtt_directory_provider bad_provider;
        struct idw_device_scope bad_scope = { 0x1234, 2 };
        struct idw_device_scope readback;

        CHECK(idw_hsw_hw_get_scope(hw, &readback) == 0);
        CHECK(readback.device_cookie == scope.device_cookie);
        CHECK(readback.generation == scope.generation);
        CHECK(idw_ggtt_directory_init(&bad_provider, ggtt, hw, &bad_scope,
                                      IDW_HSW_GGTT_RECOMMENDED_ALIGN) ==
              -EXDEV);
        CHECK(idw_ggtt_directory_init(&bad_provider, ggtt, hw, &scope,
                                      IDW_HSW_GGTT_MIN_ALIGN - 1) == -EINVAL);
        CHECK(idw_ggtt_directory_init(&bad_provider, ggtt, hw, NULL,
                                      IDW_HSW_GGTT_RECOMMENDED_ALIGN) ==
              -EINVAL);
    }
    CHECK(idw_ggtt_directory_init(&provider, ggtt, hw, &scope,
                                  IDW_HSW_GGTT_RECOMMENDED_ALIGN) == 0);
    idw_ggtt_directory_ops(&provider, &directory_ops);

    page_ops.size = sizeof(page_ops);
    page_ops.version = 1;
    page_ops.scope = scope;
    page_ops.alloc_page_table_page = fake_alloc_page;
    page_ops.free_page_table_page = fake_free_page;
    page_ops.context = &pages;
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model) == 0);
    CHECK(provider.active == 1);
    CHECK(idw_ggtt_count(ggtt) == 1);
    CHECK(idw_ggtt_directory_fini(&provider) == -EBUSY);
    directory_offset = idw_hsw_ppgtt_directory_ggtt_offset(model);
    CHECK(directory_offset == 0);
    CHECK(idw_hsw_hw_sim_read_gtt(hw, directory_offset, &value) == 0);
    CHECK(value == 0);

    CHECK(idw_hsw_ppgtt_map(model, 0x200000, IDW_GGTT_PAGE_BYTES,
                            &mapping, 1) == 0);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, 0, &pde) == 0);
    CHECK(pde == 0x100001u);
    CHECK(idw_hsw_hw_sim_read_gtt(hw, directory_offset, &value) == 0);
    CHECK(value == pde);
    CHECK(idw_hsw_ppgtt_translate(model, 0x200000, &translation) == 0);
    CHECK(translation.dma_address == 0x300000);

    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER |
                                   IDW_HSW_ENGINE_BLITTER,
                              directory_offset) == 0);
    CHECK(idw_hsw_hw_is_active(hw));
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER, 0) == -EBUSY);
    CHECK(idw_hsw_hw_destroy(hw) == -EBUSY);
    CHECK(idw_hsw_hw_sim_engine_mask(hw) ==
          (IDW_HSW_ENGINE_RENDER | IDW_HSW_ENGINE_BLITTER));
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DCLV, &value) == 0);
    CHECK(value == IDW_HSW_PP_DIR_DCLV_2G);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DIR_BASE, &value) == 0);
    CHECK(value == 0);
    CHECK(idw_hsw_hw_invalidate(hw, IDW_HSW_ENGINE_RENDER) == 0);
    CHECK(idw_hsw_hw_deactivate(hw) == 0);
    CHECK(!idw_hsw_hw_is_active(hw));
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DCLV, &value) == 0);
    CHECK(value == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DIR_BASE, &value) == 0);
    CHECK(value == 0);

    CHECK(idw_hsw_ppgtt_unmap(model, 0x200000, IDW_GGTT_PAGE_BYTES) == 0);
    {
        struct idw_hsw_ppgtt_pending_invalidation pending;

        memset(&pending, 0, sizeof(pending));
        pending.size = sizeof(pending);
        CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
        CHECK(pending.pending == 1);
        CHECK(idw_hsw_ppgtt_invalidation_complete(
                  model, pending.generation) == 0);
    }
    CHECK(idw_hsw_ppgtt_set_active(model, true) == 0);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == -EBUSY);
    CHECK(idw_hsw_ppgtt_set_active(model, false) == 0);
    CHECK(idw_hsw_ppgtt_reclaim_empty(model) == 0);
    CHECK(idw_hsw_ppgtt_hardware_pde(model, 0, &pde) == 0);
    CHECK((pde & IDW_HSW_PPGTT_PDE_VALID) != 0);

    CHECK(idw_hsw_hw_sim_fail_next(hw, IDW_HSW_HW_SIM_FAIL_GTT_WRITE) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0x400000, IDW_GGTT_PAGE_BYTES,
                            &mapping, 1) == -EIO);
    CHECK(idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_ppgtt_translate(model, 0x400000, &translation) == -EIO);

    idw_hsw_ppgtt_model_destroy(model);
    model = NULL;
    CHECK(provider.active == 0);
    CHECK(idw_ggtt_count(ggtt) == 0);
    CHECK(idw_ggtt_directory_fini(&provider) == -EIO);
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    CHECK(idw_ggtt_destroy(ggtt) == 0);
    CHECK(pages.frees == 3);
    return 0;
}

static int test_activation_failures(void)
{
    struct idw_device_scope scope = { 0x1234, 1 };
    struct idw_hsw_hw *hw = NULL;
    uint32_t value;

    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    CHECK(idw_hsw_hw_write_pde(hw, 0x1000, 0, 0xdeadbeefu) == 0);
    CHECK(idw_hsw_hw_sim_read_gtt(hw, 0x1000, &value) == 0);
    CHECK(value == 0xdeadbeefu);
    CHECK(idw_hsw_hw_read_pde(hw, 0x1000, 0, &value) == 0);
    CHECK(value == 0xdeadbeefu);
    CHECK(idw_hsw_hw_read_pde(hw, 0x1000, 0, NULL) == -EINVAL);
    CHECK(idw_hsw_hw_write_pde(hw, 0x1000, IDW_HSW_PPGTT_DIRECTORY_PDES,
                               0) == -ERANGE);
    CHECK(idw_hsw_hw_write_pde(hw, 0x1001, 0, 0) == -ERANGE);
    CHECK(idw_hsw_hw_write_pde(hw, TEST_GGTT_BYTES, 0, 0) == -ERANGE);
    CHECK(idw_hsw_hw_invalidate(hw, 0) == -EINVAL);
    CHECK(idw_hsw_hw_invalidate(hw, 0x80) == -EINVAL);

    CHECK(idw_hsw_hw_sim_fail_next(hw, IDW_HSW_HW_SIM_FAIL_ENGINE) == 0);
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER |
                                   IDW_HSW_ENGINE_BLITTER, 0x1000) == -EIO);
    CHECK(!idw_hsw_hw_is_active(hw));
    CHECK(!idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_check(hw) == 0);
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DCLV, &value) == 0);
    CHECK(value == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DIR_BASE, &value) == 0);
    CHECK(value == 0);

    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER, 0x1000) == 0);
    CHECK(idw_hsw_hw_deactivate(hw) == 0);
    CHECK(idw_hsw_hw_activate(hw, 0, 0x1000) == -EINVAL);
    CHECK(idw_hsw_hw_activate(hw, 0x80, 0x1000) == -EINVAL);
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER, 0x1001) == -EINVAL);
    CHECK(idw_hsw_hw_deactivate(hw) == -EINVAL);

    CHECK(idw_hsw_hw_sim_set_fail_mask(hw,
                                       IDW_HSW_HW_SIM_FAIL_INVALIDATE) == 0);
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER, 0x1000) == -EIO);
    CHECK(idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_check(hw) == -EIO);
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    return 0;
}

int main(void)
{
    if (test_register_vectors() || test_directory_and_activation() ||
        test_activation_failures())
        return 1;
    printf("PASS: Haswell GGTT directory, hardware sequence, and rollback tests\n");
    return 0;
}
