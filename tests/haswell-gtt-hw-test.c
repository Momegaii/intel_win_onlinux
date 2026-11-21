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
    uint32_t syncs;
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

static int fake_sync_page(void *context, const struct idw_dma_page *page,
                          uint32_t offset, uint32_t bytes)
{
    struct fake_pages *pages = context;

    if (!page || offset || bytes != IDW_GGTT_PAGE_BYTES)
        return -EINVAL;
    ++pages->syncs;
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

static int expect_event(struct idw_hsw_hw *hw, uint32_t index,
                        uint32_t type, uint32_t engine, uint64_t offset,
                        uint32_t value)
{
    struct idw_hsw_hw_sim_event event;

    CHECK(idw_hsw_hw_sim_trace_get(hw, index, &event) == 0);
    CHECK(event.type == type);
    CHECK(event.engine == engine);
    CHECK(event.offset == offset);
    CHECK(event.value == value);
    return 0;
}

static int test_register_vectors(void)
{
    CHECK(IDW_HSW_RENDER_RING_BASE == 0x2000u);
    CHECK(IDW_HSW_VIDEO_RING_BASE == 0x12000u);
    CHECK(IDW_HSW_BLITTER_RING_BASE == 0x22000u);
    CHECK(IDW_HSW_REG_PGTBL_CTL == 0x2020u);
    CHECK(IDW_HSW_REG_INSTPM == 0x20c0u);
    CHECK(IDW_HSW_RING_INSTPM(IDW_HSW_BLITTER_RING_BASE) == 0x220c0u);
    CHECK(IDW_HSW_RING_INSTPM(IDW_HSW_VIDEO_RING_BASE) == 0x120c0u);
    CHECK(IDW_HSW_REG_PP_DCLV == 0x2220u);
    CHECK(IDW_HSW_REG_PP_DIR_BASE == 0x2228u);
    CHECK(IDW_HSW_REG_PP_DIR_BASE_READ == 0x2518u);
    CHECK(IDW_HSW_REG_RING_MODE_GEN7 == 0x229cu);
    CHECK(IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE) == 0x22220u);
    CHECK(IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE) == 0x22228u);
    CHECK(IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_BLITTER_RING_BASE) ==
          0x22518u);
    CHECK(IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE) == 0x2229cu);
    CHECK(IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_VIDEO_RING_BASE) == 0x12220u);
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
    struct idw_ggtt_allocation_info info;
    struct idw_ggtt_directory_provider active_provider;
    struct idw_hsw_ppgtt_directory_ops active_ops;
    struct idw_hsw_ppgtt_directory second_directory;
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
    page_ops.sync_page_table_for_device = fake_sync_page;
    page_ops.context = &pages;
    CHECK(idw_hsw_ppgtt_model_create(&page_ops, &directory_ops, &model) == 0);
    CHECK(provider.active == 1);
    CHECK(idw_ggtt_count(ggtt) == 1);
    CHECK(idw_ggtt_directory_fini(&provider) == -EBUSY);
    directory_offset = idw_hsw_ppgtt_directory_ggtt_offset(model);
    CHECK(directory_offset == 0);
    info.size = sizeof(info);
    CHECK(idw_ggtt_query(ggtt, directory_offset, 1, &info) == 0);
    CHECK(info.pin_count == 1 && info.resident == 1);
    CHECK((info.flags & IDW_GGTT_ALLOC_PAGE_DIRECTORY) != 0);
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

    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DCLV,
                                   0x11111111u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DIR_BASE,
                                   0x1000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
              0x22222222u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE),
              0x2000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_VIDEO_RING_BASE),
              0x33333333u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_VIDEO_RING_BASE),
              0x3000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                   0x40u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              0x80u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_VIDEO_RING_BASE),
              0x100u) == 0);

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
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER |
                                   IDW_HSW_ENGINE_BLITTER,
                              directory_offset) == 0);
    CHECK(idw_hsw_ppgtt_model_destroy_checked(model, false) == -EBUSY);
    CHECK(provider.active == 1);
    CHECK(idw_ggtt_count(ggtt) == 1);
    CHECK(idw_ggtt_directory_init(&active_provider, ggtt, hw, &scope,
                                  IDW_HSW_GGTT_RECOMMENDED_ALIGN) == 0);
    idw_ggtt_directory_ops(&active_provider, &active_ops);
    memset(&second_directory, 0, sizeof(second_directory));
    second_directory.size = sizeof(second_directory);
    second_directory.version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    CHECK(active_ops.alloc_directory(active_ops.context,
                                     &second_directory) == 0);
    CHECK(active_provider.active == 1);
    CHECK(idw_ggtt_count(ggtt) == 2);
    CHECK(active_ops.write_pde_gttaddr(active_ops.context,
                                       &second_directory, 0, 0) == -EBUSY);
    active_ops.free_directory(active_ops.context, &second_directory);
    CHECK(active_provider.poisoned == 1);
    CHECK(active_provider.active == 1);
    CHECK(idw_ggtt_count(ggtt) == 2);
    CHECK(idw_ggtt_directory_fini(&active_provider) == -EBUSY);
    CHECK(idw_hsw_hw_is_active(hw));
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER, 0) == -EBUSY);
    CHECK(idw_hsw_hw_destroy(hw) == -EBUSY);
    CHECK(idw_hsw_hw_sim_engine_mask(hw) ==
          (IDW_HSW_ENGINE_RENDER | IDW_HSW_ENGINE_BLITTER));
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DCLV, &value) == 0);
    CHECK(value == IDW_HSW_PP_DIR_DCLV_2G);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DIR_BASE_READ,
                                  &value) == 0);
    CHECK(value == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                  &value) == 0);
    CHECK(value == (0x40u | IDW_HSW_RING_MODE_PPGTT_ENABLE));
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == (0x80u | IDW_HSW_RING_MODE_PPGTT_ENABLE));
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x100u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == IDW_HSW_PP_DIR_DCLV_2G);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x33333333u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x3000u);
    CHECK(idw_hsw_hw_invalidate(hw, IDW_HSW_ENGINE_RENDER) == 0);
    CHECK(idw_hsw_hw_deactivate(hw) == 0);
    CHECK(!idw_hsw_hw_is_active(hw));
    CHECK(idw_hsw_ppgtt_set_active(model, false) == 0);
    active_ops.free_directory(active_ops.context, &second_directory);
    CHECK(active_provider.active == 0);
    CHECK(idw_ggtt_count(ggtt) == 1);
    CHECK(idw_ggtt_directory_fini(&active_provider) == -EIO);
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DCLV, &value) == 0);
    CHECK(value == 0x11111111u);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DIR_BASE_READ,
                                  &value) == 0);
    CHECK(value == 0x1000u);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                  &value) == 0);
    CHECK(value == 0x40u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0x80u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x100u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0x22222222u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0x2000u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x33333333u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x3000u);

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
    {
        struct idw_hsw_ppgtt_pending_invalidation pending;

        memset(&pending, 0, sizeof(pending));
        pending.size = sizeof(pending);
        CHECK(idw_hsw_ppgtt_pending(model, &pending) == 0);
        CHECK(pending.pending == 1);
        CHECK(idw_hsw_ppgtt_invalidation_complete(
                  model, pending.generation) == 0);
    }

    CHECK(idw_hsw_hw_sim_fail_next(hw, IDW_HSW_HW_SIM_FAIL_GTT_WRITE) == 0);
    CHECK(idw_hsw_ppgtt_map(model, 0x400000, IDW_GGTT_PAGE_BYTES,
                            &mapping, 1) == -EIO);
    CHECK(idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_ppgtt_translate(model, 0x400000, &translation) == -EIO);

    CHECK(idw_hsw_ppgtt_model_destroy_checked(model, false) == 0);
    model = NULL;
    CHECK(provider.active == 0);
    CHECK(idw_ggtt_count(ggtt) == 0);
    CHECK(idw_ggtt_directory_fini(&provider) == -EIO);
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    CHECK(idw_ggtt_destroy(ggtt) == 0);
    CHECK(pages.frees == 3);
    CHECK(pages.syncs > 0);
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

    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                   IDW_HSW_RING_MODE_PPGTT_ENABLE) == 0);
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER, 0x1000) == -EBUSY);
    CHECK(!idw_hsw_hw_is_active(hw));
    CHECK(!idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                  &value) == 0);
    CHECK(value == IDW_HSW_RING_MODE_PPGTT_ENABLE);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_RING_MODE_GEN7, 0) == 0);

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

    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER |
                                   IDW_HSW_ENGINE_BLITTER, 0x1000) == 0);
    CHECK(idw_hsw_hw_sim_fail_next(hw, IDW_HSW_HW_SIM_FAIL_ENGINE) == 0);
    CHECK(idw_hsw_hw_deactivate(hw) == -EIO);
    CHECK(idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_is_active(hw));
    CHECK((idw_hsw_hw_sim_engine_mask(hw) & IDW_HSW_ENGINE_RENDER) != 0);
    CHECK((idw_hsw_hw_sim_engine_mask(hw) & IDW_HSW_ENGINE_BLITTER) == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DCLV, &value) == 0);
    CHECK(value == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DIR_BASE_READ,
                                  &value) == 0);
    CHECK(value == 0);
    CHECK(idw_hsw_hw_destroy(hw) == -EBUSY);
    CHECK(idw_hsw_hw_sim_reset(hw) == 0);
    CHECK(!idw_hsw_hw_is_active(hw));
    CHECK(!idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_destroy(hw) == 0);

    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    CHECK(idw_hsw_hw_sim_set_fail_mask(hw,
                                       IDW_HSW_HW_SIM_FAIL_ENGINE) == 0);
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER |
                                   IDW_HSW_ENGINE_BLITTER, 0x1000) == -EIO);
    CHECK(idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_is_active(hw));
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_destroy(hw) == -EBUSY);
    CHECK(idw_hsw_hw_sim_reset(hw) == 0);
    CHECK(!idw_hsw_hw_is_active(hw));
    CHECK(!idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    return 0;
}



static int test_directory_liveness(void)
{
    struct idw_device_scope scope = { 0x1234, 1 };
    struct idw_ggtt_config config;
    struct idw_ggtt *ggtt = NULL;
    struct idw_hsw_hw *hw = NULL;
    struct idw_ggtt_directory_provider provider;
    struct idw_hsw_ppgtt_directory_ops ops;
    struct idw_hsw_ppgtt_directory directory;
    struct idw_hsw_ppgtt_directory stale;
    uint32_t value = 0;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.version = IDW_GGTT_VERSION;
    config.bytes = TEST_GGTT_BYTES;
    config.page_bytes = IDW_GGTT_PAGE_BYTES;
    config.max_allocations = 8;
    CHECK(idw_ggtt_create(&config, &ggtt) == 0);
    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    CHECK(idw_ggtt_directory_init(&provider, ggtt, hw, &scope,
                                  IDW_HSW_GGTT_RECOMMENDED_ALIGN) == 0);
    idw_ggtt_directory_ops(&provider, &ops);
    memset(&directory, 0, sizeof(directory));
    directory.size = sizeof(directory);
    directory.version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    CHECK(ops.alloc_directory(ops.context, &directory) == 0);
    stale = directory;
    ops.free_directory(ops.context, &directory);
    CHECK(provider.active == 0);
    CHECK(idw_ggtt_count(ggtt) == 0);
    CHECK(ops.write_pde_gttaddr(ops.context, &stale, 0, 0) == -EINVAL);
    CHECK(ops.read_pde_gttaddr(ops.context, &stale, 0, &value) == -EINVAL);
    CHECK(ops.flush_gtt(ops.context, &stale) == -EINVAL);
    CHECK(idw_ggtt_directory_fini(&provider) == 0);
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    CHECK(idw_ggtt_destroy(ggtt) == 0);
    return 0;
}

static int test_activation_order(void)
{
    struct idw_device_scope scope = { 0x1234, 1 };
    struct idw_hsw_hw *hw = NULL;
    uint32_t engines = IDW_HSW_ENGINE_RENDER | IDW_HSW_ENGINE_BLITTER;

    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DCLV,
                                   0x11111111u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DIR_BASE,
                                   0x1000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DIR_BASE_READ,
                                   0xdead0000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                   0x40u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
              0x22222222u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE),
              0x2000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              0x80u) == 0);

    CHECK(idw_hsw_hw_sim_trace_reset(hw) == 0);
    CHECK(idw_hsw_hw_activate(hw, engines, 0x3000) == 0);
    CHECK(idw_hsw_hw_sim_trace_overflowed(hw) == 0);
    CHECK(idw_hsw_hw_sim_trace_count(hw) == 19);
    CHECK(expect_event(hw, 0, IDW_HSW_HW_SIM_EVENT_GTT_FLUSH, 0, 0, 0) == 0);
    CHECK(expect_event(hw, 1, IDW_HSW_HW_SIM_EVENT_POSTING_READ, 0,
                       IDW_HSW_REG_GFX_FLSH_CNTL_GEN6, 0) == 0);
    CHECK(expect_event(hw, 2, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_REG_PP_DCLV, 0) == 0);
    CHECK(expect_event(hw, 3, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_REG_PP_DIR_BASE, 0) == 0);
    CHECK(expect_event(hw, 4, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_REG_RING_MODE_GEN7, 0) == 0);
    CHECK(expect_event(hw, 5, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
                       0) == 0);
    CHECK(expect_event(hw, 6, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_RING_PP_DIR_BASE(
                           IDW_HSW_BLITTER_RING_BASE),
                       0) == 0);
    CHECK(expect_event(hw, 7, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
                       0) == 0);
    CHECK(expect_event(hw, 8, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_REG_PP_DCLV, IDW_HSW_PP_DIR_DCLV_2G) == 0);
    CHECK(expect_event(hw, 9, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_REG_PP_DIR_BASE, 0x3000) == 0);
    CHECK(expect_event(hw, 10, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
                       IDW_HSW_PP_DIR_DCLV_2G) == 0);
    CHECK(expect_event(hw, 11, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE),
                       0x3000) == 0);
    CHECK(expect_event(hw, 12, IDW_HSW_HW_SIM_EVENT_ENGINE,
                       IDW_HSW_ENGINE_RENDER, 0, 1) == 0);
    CHECK(expect_event(hw, 13, IDW_HSW_HW_SIM_EVENT_ENGINE,
                       IDW_HSW_ENGINE_BLITTER, 0, 1) == 0);
    CHECK(expect_event(hw, 14, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_REG_RING_MODE_GEN7, 0) == 0);
    CHECK(expect_event(hw, 15, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
                       0) == 0);
    CHECK(expect_event(hw, 16, IDW_HSW_HW_SIM_EVENT_INVALIDATE, engines,
                       0, 0) == 0);
    CHECK(expect_event(hw, 17, IDW_HSW_HW_SIM_EVENT_POSTING_READ, 0,
                       IDW_HSW_REG_INSTPM, 0) == 0);
    CHECK(expect_event(hw, 18, IDW_HSW_HW_SIM_EVENT_POSTING_READ, 0,
                       IDW_HSW_RING_INSTPM(IDW_HSW_BLITTER_RING_BASE),
                       0) == 0);

    CHECK(idw_hsw_hw_sim_trace_reset(hw) == 0);
    CHECK(idw_hsw_hw_deactivate(hw) == 0);
    CHECK(idw_hsw_hw_sim_trace_overflowed(hw) == 0);
    CHECK(idw_hsw_hw_sim_trace_count(hw) == 15);
    CHECK(expect_event(hw, 0, IDW_HSW_HW_SIM_EVENT_ENGINE,
                       IDW_HSW_ENGINE_RENDER, 0, 0) == 0);
    CHECK(expect_event(hw, 1, IDW_HSW_HW_SIM_EVENT_ENGINE,
                       IDW_HSW_ENGINE_BLITTER, 0, 0) == 0);
    CHECK(expect_event(hw, 2, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_REG_RING_MODE_GEN7, 0) == 0);
    CHECK(expect_event(hw, 3, IDW_HSW_HW_SIM_EVENT_READ, 0,
                       IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
                       0) == 0);
    CHECK(expect_event(hw, 4, IDW_HSW_HW_SIM_EVENT_INVALIDATE, engines,
                       0, 0) == 0);
    CHECK(expect_event(hw, 5, IDW_HSW_HW_SIM_EVENT_POSTING_READ, 0,
                       IDW_HSW_REG_INSTPM, 0) == 0);
    CHECK(expect_event(hw, 6, IDW_HSW_HW_SIM_EVENT_POSTING_READ, 0,
                       IDW_HSW_RING_INSTPM(IDW_HSW_BLITTER_RING_BASE),
                       0) == 0);
    CHECK(expect_event(hw, 7, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_REG_PP_DCLV, 0x11111111u) == 0);
    CHECK(expect_event(hw, 8, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_REG_PP_DIR_BASE, 0x1000u) == 0);
    CHECK(expect_event(hw, 9, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_REG_RING_MODE_GEN7, 0x40u) == 0);
    CHECK(expect_event(hw, 10, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
                       0x22222222u) == 0);
    CHECK(expect_event(hw, 11, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE),
                       0x2000u) == 0);
    CHECK(expect_event(hw, 12, IDW_HSW_HW_SIM_EVENT_WRITE, 0,
                       IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
                       0x80u) == 0);
    CHECK(expect_event(hw, 13, IDW_HSW_HW_SIM_EVENT_GTT_FLUSH, 0, 0, 0) == 0);
    CHECK(expect_event(hw, 14, IDW_HSW_HW_SIM_EVENT_POSTING_READ, 0,
                       IDW_HSW_REG_GFX_FLSH_CNTL_GEN6, 0) == 0);
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    return 0;
}


static int test_all_engine_activation(void)
{
    struct idw_device_scope scope = { 0x1234, 1 };
    struct idw_hsw_hw *hw = NULL;
    uint32_t value;

    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DCLV,
                                   0x11111111u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DIR_BASE,
                                   0x1000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                   0x40u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
              0x22222222u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE),
              0x2000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              0x80u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_VIDEO_RING_BASE),
              0x33333333u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_VIDEO_RING_BASE),
              0x3000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_VIDEO_RING_BASE),
              0x100u) == 0);

    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_ALL, 0x4000) == 0);
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == IDW_HSW_ENGINE_ALL);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                  &value) == 0);
    CHECK(value == (0x40u | IDW_HSW_RING_MODE_PPGTT_ENABLE));
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == (0x80u | IDW_HSW_RING_MODE_PPGTT_ENABLE));
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == (0x100u | IDW_HSW_RING_MODE_PPGTT_ENABLE));
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x4000u);

    CHECK(idw_hsw_hw_deactivate(hw) == 0);
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                  &value) == 0);
    CHECK(value == 0x40u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0x80u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x100u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x33333333u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_VIDEO_RING_BASE),
              &value) == 0);
    CHECK(value == 0x3000u);
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    return 0;
}

static int test_partial_activation_rollback(void)
{
    struct idw_device_scope scope = { 0x1234, 1 };
    struct idw_hsw_hw *hw = NULL;
    uint32_t value;

    CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DCLV,
                                   0x11111111u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_PP_DIR_BASE,
                                   0x1000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                   0x40u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
              0x22222222u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE),
              0x2000u) == 0);
    CHECK(idw_hsw_hw_sim_write_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              0x80u) == 0);
    CHECK(idw_hsw_hw_sim_fail_engine(hw, IDW_HSW_ENGINE_BLITTER) == 0);
    CHECK(idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER |
                                   IDW_HSW_ENGINE_BLITTER, 0x5000) == -EIO);
    CHECK(!idw_hsw_hw_is_active(hw));
    CHECK(!idw_hsw_hw_is_poisoned(hw));
    CHECK(idw_hsw_hw_sim_engine_mask(hw) == 0);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_RING_MODE_GEN7,
                                  &value) == 0);
    CHECK(value == 0x40u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_MODE_GEN7(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0x80u);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DCLV, &value) == 0);
    CHECK(value == 0x11111111u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0x22222222u);
    CHECK(idw_hsw_hw_sim_read_reg(hw, IDW_HSW_REG_PP_DIR_BASE, &value) == 0);
    CHECK(value == 0x1000u);
    CHECK(idw_hsw_hw_sim_read_reg(
              hw, IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE),
              &value) == 0);
    CHECK(value == 0x2000u);
    CHECK(idw_hsw_hw_destroy(hw) == 0);
    return 0;
}

static int test_activation_failure_matrix(void)
{
    struct idw_device_scope scope = { 0x1234, 1 };
    const uint32_t flags[] = {
        IDW_HSW_HW_SIM_FAIL_READ,
        IDW_HSW_HW_SIM_FAIL_WRITE,
        IDW_HSW_HW_SIM_FAIL_GTT_READ,
        IDW_HSW_HW_SIM_FAIL_GTT_WRITE,
        IDW_HSW_HW_SIM_FAIL_FLUSH,
        IDW_HSW_HW_SIM_FAIL_INVALIDATE,
        IDW_HSW_HW_SIM_FAIL_ENGINE,
        IDW_HSW_HW_SIM_FAIL_POSTING_READ
    };
    uint32_t i;

    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        struct idw_hsw_hw *hw = NULL;

        CHECK(idw_hsw_hw_sim_create(&hw, TEST_GGTT_BYTES, &scope) == 0);
        CHECK(idw_hsw_hw_sim_set_fail_mask(hw, flags[i]) == 0);
        (void)idw_hsw_hw_activate(hw, IDW_HSW_ENGINE_RENDER |
                                       IDW_HSW_ENGINE_BLITTER, 0x1000);
        if (idw_hsw_hw_is_active(hw)) {
            CHECK(idw_hsw_hw_destroy(hw) == -EBUSY);
            CHECK(idw_hsw_hw_sim_reset(hw) == 0);
        }
        CHECK(idw_hsw_hw_destroy(hw) == 0);
    }
    return 0;
}

int main(void)
{
    if (test_register_vectors() || test_directory_and_activation() ||
        test_directory_liveness() ||
        test_activation_order() || test_all_engine_activation() ||
        test_partial_activation_rollback() || test_activation_failures() ||
        test_activation_failure_matrix())
        return 1;
    printf("PASS: Haswell GGTT directory, hardware sequence, and rollback tests\n");
    return 0;
}
