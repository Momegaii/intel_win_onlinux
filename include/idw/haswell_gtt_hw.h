/* SPDX-License-Identifier: MIT */
#ifndef IDW_HASWELL_GTT_HW_H
#define IDW_HASWELL_GTT_HW_H

#include "contracts/base.h"
#include "contracts/page_table_memory.h"

#include <stdbool.h>

#define IDW_HSW_HW_VERSION 1u
#define IDW_HSW_PPGTT_DIRECTORY_PDES 512u
#define IDW_HSW_PPGTT_PDE_BYTES 4u
#define IDW_HSW_PPGTT_DIRECTORY_GGTT_BYTES 4096u
#define IDW_HSW_GGTT_MIN_ALIGN 4096u
#define IDW_HSW_GGTT_RECOMMENDED_ALIGN (64u * 1024u)
#define IDW_HSW_PP_DIR_DCLV_2G 0xffffffffu

/* Haswell engine MMIO bases and per-engine PPGTT register offsets. */
#define IDW_HSW_RENDER_RING_BASE 0x2000u
#define IDW_HSW_VIDEO_RING_BASE 0x12000u
#define IDW_HSW_BLITTER_RING_BASE 0x22000u

#define IDW_HSW_RING_INSTPM(base) ((base) + 0xc0u)
#define IDW_HSW_RING_PP_DIR_DCLV(base) ((base) + 0x220u)
#define IDW_HSW_RING_PP_DIR_BASE(base) ((base) + 0x228u)
#define IDW_HSW_RING_PP_DIR_BASE_READ(base) ((base) + 0x518u)
#define IDW_HSW_RING_MODE_GEN7(base) ((base) + 0x29cu)

/* Render-engine aliases retained for register-vector tests and callers. */
#define IDW_HSW_REG_PGTBL_CTL 0x2020u
#define IDW_HSW_REG_INSTPM IDW_HSW_RING_INSTPM(IDW_HSW_RENDER_RING_BASE)
#define IDW_HSW_REG_PP_DCLV IDW_HSW_RING_PP_DIR_DCLV(IDW_HSW_RENDER_RING_BASE)
#define IDW_HSW_REG_PP_DIR_BASE \
    IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_RENDER_RING_BASE)
#define IDW_HSW_REG_PP_DIR_BASE_READ \
    IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_RENDER_RING_BASE)
#define IDW_HSW_REG_RING_MODE_GEN7 \
    IDW_HSW_RING_MODE_GEN7(IDW_HSW_RENDER_RING_BASE)
#define IDW_HSW_REG_ECOCHK 0x4090u
#define IDW_HSW_REG_GAC_ECO_BITS 0x14090u
#define IDW_HSW_REG_GFX_FLSH_CNTL_GEN6 0x101008u

#define IDW_HSW_RING_MODE_PPGTT_ENABLE (1u << 9)
#define IDW_HSW_INSTPM_TLB_INVALIDATE (1u << 9)
#define IDW_HSW_INSTPM_SYNC_FLUSH (1u << 5)
#define IDW_HSW_GFX_FLSH_CNTL_GEN6_FLUSH (1u << 0)

enum idw_hsw_engine {
    IDW_HSW_ENGINE_RENDER = 1u << 0,
    IDW_HSW_ENGINE_BLITTER = 1u << 1,
    IDW_HSW_ENGINE_VIDEO = 1u << 2
};

#define IDW_HSW_ENGINE_ALL \
    (IDW_HSW_ENGINE_RENDER | IDW_HSW_ENGINE_BLITTER | IDW_HSW_ENGINE_VIDEO)

enum idw_hsw_hw_flags {
    IDW_HSW_HW_EXCLUSIVE_OWNER = 1u << 0,
    IDW_HSW_HW_SIMULATED = 1u << 1
};

struct idw_hsw_hw_config {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t ggtt_bytes;
    struct idw_device_scope scope;
};

/*
 * Low-level hardware operations. The real backend owns PCI BAR mapping,
 * forcewake, register serialization, and engine-specific register offsets.
 * `invalidate_tlb` must perform the Gen7.5 INSTPM TLB-invalidate/sync-flush
 * sequence and poll until the sync-flush bit clears. `set_engine_ppgtt` must
 * program only the named engine's RING_MODE_GEN7 PPGTT-enable bit.
 */
struct idw_hsw_hw_ops {
    uint32_t size;
    uint32_t version;
    int (*read32)(void *context, uint32_t offset, uint32_t *value);
    int (*write32)(void *context, uint32_t offset, uint32_t value);
    int (*gtt_read32)(void *context, uint64_t ggtt_offset, uint32_t *value);
    int (*gtt_write32)(void *context, uint64_t ggtt_offset, uint32_t value);
    int (*gtt_flush)(void *context);
    int (*invalidate_tlb)(void *context, uint32_t engine_mask);
    int (*set_engine_ppgtt)(void *context, uint32_t engine, bool enable);
    int (*posting_read)(void *context, uint32_t offset, uint32_t *value);
    void (*release)(void *context);
};

struct idw_hsw_hw;

int idw_hsw_hw_init(const struct idw_hsw_hw_config *config,
                    const struct idw_hsw_hw_ops *ops, void *context,
                    struct idw_hsw_hw **hw);
int idw_hsw_hw_destroy(struct idw_hsw_hw *hw);

int idw_hsw_hw_write_pde(struct idw_hsw_hw *hw, uint64_t directory_ggtt,
                         uint32_t index, uint32_t pde);
int idw_hsw_hw_read_pde(struct idw_hsw_hw *hw, uint64_t directory_ggtt,
                        uint32_t index, uint32_t *pde);
int idw_hsw_hw_flush_gtt(struct idw_hsw_hw *hw);
int idw_hsw_hw_invalidate(struct idw_hsw_hw *hw, uint32_t engine_mask);
int idw_hsw_hw_activate(struct idw_hsw_hw *hw, uint32_t engine_mask,
                        uint64_t directory_ggtt);
int idw_hsw_hw_deactivate(struct idw_hsw_hw *hw);
int idw_hsw_hw_get_scope(const struct idw_hsw_hw *hw,
                         struct idw_device_scope *scope);
int idw_hsw_hw_check(const struct idw_hsw_hw *hw);
int idw_hsw_hw_is_active(const struct idw_hsw_hw *hw);
int idw_hsw_hw_is_poisoned(const struct idw_hsw_hw *hw);

/* Simulated backend: software state only, never a hardware operation. */
struct idw_hsw_hw_sim_stats {
    uint32_t reads;
    uint32_t writes;
    uint32_t gtt_reads;
    uint32_t gtt_writes;
    uint32_t flushes;
    uint32_t invalidations;
    uint32_t engine_changes;
    uint32_t posting_reads;
};

enum idw_hsw_hw_sim_fail {
    IDW_HSW_HW_SIM_FAIL_READ = 1u << 0,
    IDW_HSW_HW_SIM_FAIL_WRITE = 1u << 1,
    IDW_HSW_HW_SIM_FAIL_GTT_READ = 1u << 2,
    IDW_HSW_HW_SIM_FAIL_GTT_WRITE = 1u << 3,
    IDW_HSW_HW_SIM_FAIL_FLUSH = 1u << 4,
    IDW_HSW_HW_SIM_FAIL_INVALIDATE = 1u << 5,
    IDW_HSW_HW_SIM_FAIL_ENGINE = 1u << 6,
    IDW_HSW_HW_SIM_FAIL_POSTING_READ = 1u << 7
};

int idw_hsw_hw_sim_create(struct idw_hsw_hw **hw, uint64_t ggtt_bytes,
                          const struct idw_device_scope *scope);
int idw_hsw_hw_sim_set_fail_mask(struct idw_hsw_hw *hw, uint32_t mask);
int idw_hsw_hw_sim_fail_next(struct idw_hsw_hw *hw, uint32_t mask);
int idw_hsw_hw_sim_get_stats(const struct idw_hsw_hw *hw,
                             struct idw_hsw_hw_sim_stats *stats);
int idw_hsw_hw_sim_read_reg(const struct idw_hsw_hw *hw, uint32_t offset,
                            uint32_t *value);
int idw_hsw_hw_sim_write_reg(struct idw_hsw_hw *hw, uint32_t offset,
                             uint32_t value);
int idw_hsw_hw_sim_read_gtt(const struct idw_hsw_hw *hw, uint64_t offset,
                            uint32_t *value);
int idw_hsw_hw_sim_reset(struct idw_hsw_hw *hw);
uint32_t idw_hsw_hw_sim_engine_mask(const struct idw_hsw_hw *hw);

#endif
