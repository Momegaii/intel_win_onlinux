/* SPDX-License-Identifier: MIT */
#include "../include/idw/haswell_gtt_hw.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define IDW_HSW_SIM_REG_BYTES 0x200000u
#define IDW_HSW_SIM_REG_WORDS (IDW_HSW_SIM_REG_BYTES / 4u)
#define IDW_HSW_SIM_MAX_GGTT_BYTES (64ull * 1024u * 1024u)

struct idw_hsw_hw_saved_engine {
    uint32_t dclv;
    uint32_t dir_base;
    uint32_t ring_mode;
};

struct idw_hsw_hw {
    struct idw_hsw_hw_ops ops;
    void *context;
    struct idw_device_scope scope;
    uint64_t ggtt_bytes;
    uint32_t flags;
    uint32_t active_engines;
    struct idw_hsw_hw_saved_engine saved[3];
    uint32_t saved_mask;
    bool initialized;
    bool active;
    bool poisoned;
};

static bool engine_mask_valid(uint32_t mask)
{
    return mask && !(mask & ~IDW_HSW_ENGINE_ALL);
}

static int engine_index(uint32_t engine)
{
    switch (engine) {
    case IDW_HSW_ENGINE_RENDER:
        return 0;
    case IDW_HSW_ENGINE_BLITTER:
        return 1;
    case IDW_HSW_ENGINE_VIDEO:
        return 2;
    default:
        return -1;
    }
}

static uint32_t engine_base(uint32_t engine)
{
    switch (engine) {
    case IDW_HSW_ENGINE_RENDER:
        return IDW_HSW_RENDER_RING_BASE;
    case IDW_HSW_ENGINE_BLITTER:
        return IDW_HSW_BLITTER_RING_BASE;
    case IDW_HSW_ENGINE_VIDEO:
        return IDW_HSW_VIDEO_RING_BASE;
    default:
        return 0;
    }
}

static uint32_t engine_reg(uint32_t engine, uint32_t offset)
{
    return engine_base(engine) + offset;
}

static bool directory_valid(const struct idw_hsw_hw *hw, uint64_t directory)
{
    return !(directory & (IDW_HSW_GGTT_MIN_ALIGN - 1)) &&
           directory <= hw->ggtt_bytes &&
           hw->ggtt_bytes - directory >= IDW_HSW_PPGTT_DIRECTORY_GGTT_BYTES;
}

static void poison(struct idw_hsw_hw *hw)
{
    hw->poisoned = true;
}

static int restore_engines(struct idw_hsw_hw *hw, uint32_t mask)
{
    uint32_t engines[] = {
        IDW_HSW_ENGINE_RENDER, IDW_HSW_ENGINE_BLITTER, IDW_HSW_ENGINE_VIDEO
    };
    uint32_t i;
    int first_error = 0;
    int ret;

    for (i = 0; i < sizeof(engines) / sizeof(engines[0]); ++i) {
        int index;

        if (!(mask & engines[i]))
            continue;
        index = engine_index(engines[i]);
        ret = hw->ops.write32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_PP_DIR_DCLV(0)),
            hw->saved[index].dclv);
        if (ret && !first_error)
            first_error = ret;
        ret = hw->ops.write32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_PP_DIR_BASE(0)),
            hw->saved[index].dir_base);
        if (ret && !first_error)
            first_error = ret;
        ret = hw->ops.write32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_MODE_GEN7(0)),
            hw->saved[index].ring_mode);
        if (ret && !first_error)
            first_error = ret;
    }
    return first_error;
}

static int disable_engines(struct idw_hsw_hw *hw, uint32_t mask)
{
    uint32_t engines[] = {
        IDW_HSW_ENGINE_RENDER, IDW_HSW_ENGINE_BLITTER, IDW_HSW_ENGINE_VIDEO
    };
    uint32_t i;
    int first_error = 0;
    int ret;

    for (i = 0; i < sizeof(engines) / sizeof(engines[0]); ++i) {
        if (!(mask & engines[i]))
            continue;
        ret = hw->ops.set_engine_ppgtt(hw->context, engines[i], false);
        if (ret && !first_error)
            first_error = ret;
    }
    return first_error;
}

static int verify_engine_state(struct idw_hsw_hw *hw, uint32_t mask,
                               bool enabled)
{
    uint32_t engines[] = {
        IDW_HSW_ENGINE_RENDER, IDW_HSW_ENGINE_BLITTER, IDW_HSW_ENGINE_VIDEO
    };
    uint32_t i;

    for (i = 0; i < sizeof(engines) / sizeof(engines[0]); ++i) {
        uint32_t value;
        int ret;

        if (!(mask & engines[i]))
            continue;
        ret = hw->ops.read32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_MODE_GEN7(0)), &value);
        if (ret)
            return ret;
        if (!!(value & IDW_HSW_RING_MODE_PPGTT_ENABLE) != enabled)
            return -EIO;
    }
    return 0;
}

static int rollback_activation(struct idw_hsw_hw *hw, uint32_t enabled,
                               uint32_t saved_mask)
{
    int ret;
    int first_error;

    first_error = disable_engines(hw, enabled);
    ret = verify_engine_state(hw, enabled, false);
    if (ret && !first_error)
        first_error = ret;
    ret = restore_engines(hw, saved_mask);
    if (ret && !first_error)
        first_error = ret;
    ret = hw->ops.gtt_flush(hw->context);
    if (ret && !first_error)
        first_error = ret;
    if (hw->ops.posting_read) {
        uint32_t value;

        ret = hw->ops.posting_read(hw->context,
                                   IDW_HSW_REG_GFX_FLSH_CNTL_GEN6, &value);
        if (ret && !first_error)
            first_error = ret;
    }
    if (first_error) {
        poison(hw);
        hw->active = true;
        hw->active_engines = saved_mask ? saved_mask : enabled;
    } else {
        hw->saved_mask = 0;
    }
    return first_error;
}

static int snapshot_engines(struct idw_hsw_hw *hw, uint32_t mask)
{
    uint32_t engines[] = {
        IDW_HSW_ENGINE_RENDER, IDW_HSW_ENGINE_BLITTER, IDW_HSW_ENGINE_VIDEO
    };
    uint32_t i;
    int ret;

    for (i = 0; i < sizeof(engines) / sizeof(engines[0]); ++i) {
        int index;

        if (!(mask & engines[i]))
            continue;
        index = engine_index(engines[i]);
        ret = hw->ops.read32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_PP_DIR_DCLV(0)),
            &hw->saved[index].dclv);
        if (ret)
            return ret;
        ret = hw->ops.read32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_PP_DIR_BASE(0)),
            &hw->saved[index].dir_base);
        if (ret)
            return ret;
        ret = hw->ops.read32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_MODE_GEN7(0)),
            &hw->saved[index].ring_mode);
        if (ret)
            return ret;
        if (hw->saved[index].ring_mode & IDW_HSW_RING_MODE_PPGTT_ENABLE)
            return -EBUSY;
        hw->saved_mask |= engines[i];
    }
    return 0;
}

static int program_engines(struct idw_hsw_hw *hw, uint32_t mask,
                           uint64_t directory_ggtt)
{
    uint32_t engines[] = {
        IDW_HSW_ENGINE_RENDER, IDW_HSW_ENGINE_BLITTER, IDW_HSW_ENGINE_VIDEO
    };
    uint32_t i;
    int ret;

    for (i = 0; i < sizeof(engines) / sizeof(engines[0]); ++i) {
        if (!(mask & engines[i]))
            continue;
        ret = hw->ops.write32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_PP_DIR_DCLV(0)),
            IDW_HSW_PP_DIR_DCLV_2G);
        if (ret)
            return ret;
        ret = hw->ops.write32(
            hw->context,
            engine_reg(engines[i], IDW_HSW_RING_PP_DIR_BASE(0)),
            (uint32_t)directory_ggtt);
        if (ret)
            return ret;
    }
    return 0;
}

int idw_hsw_hw_init(const struct idw_hsw_hw_config *config,
                    const struct idw_hsw_hw_ops *ops, void *context,
                    struct idw_hsw_hw **hw)
{
    struct idw_hsw_hw *created;

    if (!hw)
        return -EINVAL;
    *hw = NULL;
    if (!config || !ops || config->size != sizeof(*config) ||
        config->version != IDW_HSW_HW_VERSION ||
        (config->flags & ~(IDW_HSW_HW_EXCLUSIVE_OWNER |
                           IDW_HSW_HW_SIMULATED)) ||
        !config->scope.device_cookie || !config->scope.generation ||
        !config->ggtt_bytes || config->ggtt_bytes > UINT32_MAX ||
        (config->ggtt_bytes & (IDW_HSW_GGTT_MIN_ALIGN - 1)) ||
        ops->size != sizeof(*ops) || ops->version != IDW_HSW_HW_VERSION ||
        !ops->read32 || !ops->write32 || !ops->gtt_read32 ||
        !ops->gtt_write32 || !ops->gtt_flush || !ops->invalidate_tlb ||
        !ops->set_engine_ppgtt || !ops->posting_read)
        return -EINVAL;
    if (!(config->flags & IDW_HSW_HW_SIMULATED) &&
        !(config->flags & IDW_HSW_HW_EXCLUSIVE_OWNER))
        return -EPERM;

    created = calloc(1, sizeof(*created));
    if (!created)
        return -ENOMEM;
    created->ops = *ops;
    created->context = context;
    created->scope = config->scope;
    created->ggtt_bytes = config->ggtt_bytes;
    created->flags = config->flags;
    created->initialized = true;
    *hw = created;
    return 0;
}

int idw_hsw_hw_destroy(struct idw_hsw_hw *hw)
{
    if (!hw)
        return -EINVAL;
    if (hw->active)
        return -EBUSY;
    if (hw->ops.release)
        hw->ops.release(hw->context);
    memset(hw, 0, sizeof(*hw));
    free(hw);
    return 0;
}

int idw_hsw_hw_write_pde(struct idw_hsw_hw *hw, uint64_t directory_ggtt,
                         uint32_t index, uint32_t pde)
{
    uint64_t offset;
    int ret;

    ret = idw_hsw_hw_check(hw);
    if (ret)
        return ret;
    if (index >= IDW_HSW_PPGTT_DIRECTORY_PDES ||
        !directory_valid(hw, directory_ggtt))
        return -ERANGE;
    offset = directory_ggtt + (uint64_t)index * IDW_HSW_PPGTT_PDE_BYTES;
    ret = hw->ops.gtt_write32(hw->context, offset, pde);
    if (ret)
        poison(hw);
    return ret;
}

int idw_hsw_hw_read_pde(struct idw_hsw_hw *hw, uint64_t directory_ggtt,
                        uint32_t index, uint32_t *pde)
{
    uint64_t offset;
    int ret;

    ret = idw_hsw_hw_check(hw);
    if (ret)
        return ret;
    if (!pde)
        return -EINVAL;
    if (index >= IDW_HSW_PPGTT_DIRECTORY_PDES ||
        !directory_valid(hw, directory_ggtt))
        return -ERANGE;
    offset = directory_ggtt + (uint64_t)index * IDW_HSW_PPGTT_PDE_BYTES;
    ret = hw->ops.gtt_read32(hw->context, offset, pde);
    if (ret)
        poison(hw);
    return ret;
}

int idw_hsw_hw_flush_gtt(struct idw_hsw_hw *hw)
{
    uint32_t value;
    int ret;

    ret = idw_hsw_hw_check(hw);
    if (ret)
        return ret;
    ret = hw->ops.gtt_flush(hw->context);
    if (ret) {
        poison(hw);
        return ret;
    }
    ret = hw->ops.posting_read(hw->context,
                               IDW_HSW_REG_GFX_FLSH_CNTL_GEN6, &value);
    if (ret)
        poison(hw);
    return ret;
}

int idw_hsw_hw_invalidate(struct idw_hsw_hw *hw, uint32_t engine_mask)
{
    uint32_t engines[] = {
        IDW_HSW_ENGINE_RENDER, IDW_HSW_ENGINE_BLITTER, IDW_HSW_ENGINE_VIDEO
    };
    uint32_t i;
    int ret;

    ret = idw_hsw_hw_check(hw);
    if (ret)
        return ret;
    if (!engine_mask_valid(engine_mask))
        return -EINVAL;
    ret = hw->ops.invalidate_tlb(hw->context, engine_mask);
    if (ret) {
        poison(hw);
        return ret;
    }
    for (i = 0; i < sizeof(engines) / sizeof(engines[0]); ++i) {
        uint32_t value;

        if (!(engine_mask & engines[i]))
            continue;
        ret = hw->ops.posting_read(
            hw->context, engine_reg(engines[i], IDW_HSW_RING_INSTPM(0)),
            &value);
        if (ret) {
            poison(hw);
            return ret;
        }
    }
    return 0;
}

int idw_hsw_hw_activate(struct idw_hsw_hw *hw, uint32_t engine_mask,
                        uint64_t directory_ggtt)
{
    uint32_t engines[] = {
        IDW_HSW_ENGINE_RENDER, IDW_HSW_ENGINE_BLITTER, IDW_HSW_ENGINE_VIDEO
    };
    uint32_t enabled = 0;
    uint32_t i;
    int ret;

    ret = idw_hsw_hw_check(hw);
    if (ret)
        return ret;
    if (hw->active)
        return -EBUSY;
    if (!engine_mask_valid(engine_mask) ||
        !directory_valid(hw, directory_ggtt))
        return -EINVAL;
    ret = idw_hsw_hw_flush_gtt(hw);
    if (ret)
        return ret;
    hw->saved_mask = 0;
    ret = snapshot_engines(hw, engine_mask);
    if (ret) {
        hw->saved_mask = 0;
        if (ret != -EBUSY)
            poison(hw);
        return ret;
    }
    ret = program_engines(hw, engine_mask, directory_ggtt);
    if (ret) {
        if (rollback_activation(hw, 0, hw->saved_mask))
            return -EIO;
        return ret;
    }
    for (i = 0; i < sizeof(engines) / sizeof(engines[0]); ++i) {
        if (!(engine_mask & engines[i]))
            continue;
        ret = hw->ops.set_engine_ppgtt(hw->context, engines[i], true);
        if (ret) {
            if (rollback_activation(hw, enabled | engines[i],
                                    hw->saved_mask))
                return -EIO;
            return ret;
        }
        enabled |= engines[i];
    }
    ret = verify_engine_state(hw, engine_mask, true);
    if (ret) {
        rollback_activation(hw, enabled, hw->saved_mask);
        return ret;
    }
    ret = idw_hsw_hw_invalidate(hw, engine_mask);
    if (ret) {
        rollback_activation(hw, enabled, hw->saved_mask);
        return ret;
    }
    hw->active = true;
    hw->active_engines = engine_mask;
    return 0;
}

int idw_hsw_hw_deactivate(struct idw_hsw_hw *hw)
{
    uint32_t active_engines;
    uint32_t saved_mask;
    uint32_t value;
    int first_error;
    int ret;

    ret = idw_hsw_hw_check(hw);
    if (ret)
        return ret;
    if (!hw->active)
        return -EINVAL;
    active_engines = hw->active_engines;
    saved_mask = hw->saved_mask;
    first_error = disable_engines(hw, active_engines);
    ret = verify_engine_state(hw, active_engines, false);
    if (ret && !first_error)
        first_error = ret;
    ret = idw_hsw_hw_invalidate(hw, active_engines);
    if (ret && !first_error)
        first_error = ret;
    ret = restore_engines(hw, saved_mask);
    if (ret && !first_error)
        first_error = ret;
    ret = hw->ops.gtt_flush(hw->context);
    if (ret && !first_error)
        first_error = ret;
    ret = hw->ops.posting_read(hw->context,
                               IDW_HSW_REG_GFX_FLSH_CNTL_GEN6, &value);
    if (ret && !first_error)
        first_error = ret;
    if (first_error) {
        poison(hw);
        return first_error;
    }
    hw->active = false;
    hw->active_engines = 0;
    hw->saved_mask = 0;
    return 0;
}

int idw_hsw_hw_get_scope(const struct idw_hsw_hw *hw,
                         struct idw_device_scope *scope)
{
    if (!hw || !scope)
        return -EINVAL;
    *scope = hw->scope;
    return 0;
}

int idw_hsw_hw_check(const struct idw_hsw_hw *hw)
{
    if (!hw || !hw->initialized)
        return -EINVAL;
    return hw->poisoned ? -EIO : 0;
}

int idw_hsw_hw_is_active(const struct idw_hsw_hw *hw)
{
    return hw && hw->active;
}

int idw_hsw_hw_is_poisoned(const struct idw_hsw_hw *hw)
{
    return hw && hw->poisoned;
}

struct idw_hsw_hw_sim {
    uint32_t *registers;
    uint32_t *gtt;
    uint64_t ggtt_bytes;
    uint32_t fail_mask;
    uint32_t fail_once;
    uint32_t fail_engine;
    uint32_t engines;
    uint32_t trace_count;
    uint32_t trace_overflow;
    struct idw_hsw_hw_sim_event trace[IDW_HSW_HW_SIM_TRACE_MAX];
    struct idw_hsw_hw_sim_stats stats;
};

static void sim_trace(struct idw_hsw_hw_sim *sim, uint32_t type,
                      uint32_t engine, uint64_t offset, uint32_t value)
{
    struct idw_hsw_hw_sim_event *event;

    if (sim->trace_count == IDW_HSW_HW_SIM_TRACE_MAX) {
        sim->trace_overflow = 1;
        return;
    }
    event = &sim->trace[sim->trace_count++];
    event->type = type;
    event->engine = engine;
    event->offset = offset;
    event->value = value;
    event->reserved = 0;
}

static bool sim_fail(struct idw_hsw_hw_sim *sim, uint32_t flag)
{
    if (sim->fail_mask & flag)
        return true;
    if (sim->fail_once & flag) {
        sim->fail_once &= ~flag;
        return true;
    }
    return false;
}

static int sim_read32(void *context, uint32_t offset, uint32_t *value)
{
    struct idw_hsw_hw_sim *sim = context;

    if (!value || (offset & 3u) || offset > IDW_HSW_SIM_REG_BYTES - 4u)
        return -EINVAL;
    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_READ, 0, offset, 0);
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_READ))
        return -EIO;
    *value = sim->registers[offset / 4u];
    ++sim->stats.reads;
    return 0;
}

static uint32_t sim_pp_dir_base_read_alias(uint32_t offset)
{
    if (offset == IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_RENDER_RING_BASE))
        return IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_RENDER_RING_BASE);
    if (offset == IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_BLITTER_RING_BASE))
        return IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_BLITTER_RING_BASE);
    if (offset == IDW_HSW_RING_PP_DIR_BASE(IDW_HSW_VIDEO_RING_BASE))
        return IDW_HSW_RING_PP_DIR_BASE_READ(IDW_HSW_VIDEO_RING_BASE);
    return offset;
}

static void sim_store_reg(struct idw_hsw_hw_sim *sim, uint32_t offset,
                          uint32_t value)
{
    sim->registers[offset / 4u] = value;
    offset = sim_pp_dir_base_read_alias(offset);
    sim->registers[offset / 4u] = value;
}

static int sim_write32(void *context, uint32_t offset, uint32_t value)
{
    struct idw_hsw_hw_sim *sim = context;

    if ((offset & 3u) || offset > IDW_HSW_SIM_REG_BYTES - 4u)
        return -EINVAL;
    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_WRITE, 0, offset, value);
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_WRITE))
        return -EIO;
    sim_store_reg(sim, offset, value);
    ++sim->stats.writes;
    return 0;
}

static int sim_gtt_read32(void *context, uint64_t offset, uint32_t *value)
{
    struct idw_hsw_hw_sim *sim = context;

    if (!value || (offset & 3u) || offset > sim->ggtt_bytes - 4u)
        return -EINVAL;
    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_GTT_READ, 0, offset, 0);
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_GTT_READ))
        return -EIO;
    *value = sim->gtt[offset / 4u];
    ++sim->stats.gtt_reads;
    return 0;
}

static int sim_gtt_write32(void *context, uint64_t offset, uint32_t value)
{
    struct idw_hsw_hw_sim *sim = context;

    if ((offset & 3u) || offset > sim->ggtt_bytes - 4u)
        return -EINVAL;
    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_GTT_WRITE, 0, offset, value);
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_GTT_WRITE))
        return -EIO;
    sim->gtt[offset / 4u] = value;
    ++sim->stats.gtt_writes;
    return 0;
}

static int sim_gtt_flush(void *context)
{
    struct idw_hsw_hw_sim *sim = context;

    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_GTT_FLUSH, 0, 0, 0);
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_FLUSH))
        return -EIO;
    ++sim->stats.flushes;
    return 0;
}

static int sim_invalidate_tlb(void *context, uint32_t engine_mask)
{
    struct idw_hsw_hw_sim *sim = context;

    if (!engine_mask_valid(engine_mask))
        return -EINVAL;
    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_INVALIDATE, engine_mask, 0, 0);
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_INVALIDATE))
        return -EIO;
    ++sim->stats.invalidations;
    return 0;
}

static int sim_set_engine_ppgtt(void *context, uint32_t engine, bool enable)
{
    struct idw_hsw_hw_sim *sim = context;

    if (!engine || (engine & (engine - 1)) ||
        (engine & ~IDW_HSW_ENGINE_ALL))
        return -EINVAL;
    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_ENGINE, engine, 0,
              enable ? 1u : 0u);
    if (sim->fail_engine == engine) {
        sim->fail_engine = 0;
        return -EIO;
    }
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_ENGINE))
        return -EIO;
    {
        uint32_t offset = engine_reg(engine, IDW_HSW_RING_MODE_GEN7(0));
        uint32_t value = sim->registers[offset / 4u];

        if (enable)
            value |= IDW_HSW_RING_MODE_PPGTT_ENABLE;
        else
            value &= ~IDW_HSW_RING_MODE_PPGTT_ENABLE;
        sim_store_reg(sim, offset, value);
    }
    if (enable)
        sim->engines |= engine;
    else
        sim->engines &= ~engine;
    ++sim->stats.engine_changes;
    return 0;
}

static int sim_posting_read(void *context, uint32_t offset, uint32_t *value)
{
    struct idw_hsw_hw_sim *sim = context;

    if (!value || (offset & 3u) || offset > IDW_HSW_SIM_REG_BYTES - 4u)
        return -EINVAL;
    sim_trace(sim, IDW_HSW_HW_SIM_EVENT_POSTING_READ, 0, offset, 0);
    if (sim_fail(sim, IDW_HSW_HW_SIM_FAIL_POSTING_READ))
        return -EIO;
    *value = sim->registers[offset / 4u];
    ++sim->stats.posting_reads;
    return 0;
}

static void sim_release(void *context)
{
    struct idw_hsw_hw_sim *sim = context;

    if (!sim)
        return;
    free(sim->registers);
    free(sim->gtt);
    free(sim);
}

static const struct idw_hsw_hw_ops sim_ops = {
    .size = sizeof(sim_ops),
    .version = IDW_HSW_HW_VERSION,
    .read32 = sim_read32,
    .write32 = sim_write32,
    .gtt_read32 = sim_gtt_read32,
    .gtt_write32 = sim_gtt_write32,
    .gtt_flush = sim_gtt_flush,
    .invalidate_tlb = sim_invalidate_tlb,
    .set_engine_ppgtt = sim_set_engine_ppgtt,
    .posting_read = sim_posting_read,
    .release = sim_release
};

int idw_hsw_hw_sim_create(struct idw_hsw_hw **hw, uint64_t ggtt_bytes,
                          const struct idw_device_scope *scope)
{
    struct idw_hsw_hw_sim *sim;
    struct idw_hsw_hw_config config;
    int ret;

    if (!hw)
        return -EINVAL;
    *hw = NULL;
    if (!scope || !ggtt_bytes ||
        ggtt_bytes > IDW_HSW_SIM_MAX_GGTT_BYTES ||
        (ggtt_bytes & (IDW_HSW_GGTT_MIN_ALIGN - 1)))
        return -EINVAL;
    sim = calloc(1, sizeof(*sim));
    if (!sim)
        return -ENOMEM;
    sim->registers = calloc(IDW_HSW_SIM_REG_WORDS,
                            sizeof(sim->registers[0]));
    sim->gtt = calloc((size_t)(ggtt_bytes / 4u), sizeof(sim->gtt[0]));
    if (!sim->registers || !sim->gtt) {
        sim_release(sim);
        return -ENOMEM;
    }
    sim->ggtt_bytes = ggtt_bytes;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.version = IDW_HSW_HW_VERSION;
    config.flags = IDW_HSW_HW_SIMULATED;
    config.ggtt_bytes = ggtt_bytes;
    config.scope = *scope;
    ret = idw_hsw_hw_init(&config, &sim_ops, sim, hw);
    if (ret)
        sim_release(sim);
    return ret;
}

static struct idw_hsw_hw_sim *sim_from_hw(struct idw_hsw_hw *hw)
{
    if (!hw || !(hw->flags & IDW_HSW_HW_SIMULATED) || !hw->context)
        return NULL;
    return hw->context;
}

int idw_hsw_hw_sim_set_fail_mask(struct idw_hsw_hw *hw, uint32_t mask)
{
    struct idw_hsw_hw_sim *sim = sim_from_hw(hw);

    if (!sim || (mask & ~0xffu))
        return -EINVAL;
    sim->fail_mask = mask;
    return 0;
}

int idw_hsw_hw_sim_fail_next(struct idw_hsw_hw *hw, uint32_t mask)
{
    struct idw_hsw_hw_sim *sim = sim_from_hw(hw);

    if (!sim || !mask || (mask & ~0xffu))
        return -EINVAL;
    sim->fail_once |= mask;
    return 0;
}

int idw_hsw_hw_sim_get_stats(const struct idw_hsw_hw *hw,
                             struct idw_hsw_hw_sim_stats *stats)
{
    struct idw_hsw_hw_sim *sim;

    if (!hw || !stats || !(hw->flags & IDW_HSW_HW_SIMULATED))
        return -EINVAL;
    sim = hw->context;
    if (!sim)
        return -EINVAL;
    *stats = sim->stats;
    return 0;
}

int idw_hsw_hw_sim_read_reg(const struct idw_hsw_hw *hw, uint32_t offset,
                            uint32_t *value)
{
    struct idw_hsw_hw_sim *sim;

    if (!hw || !value || (offset & 3u) ||
        offset > IDW_HSW_SIM_REG_BYTES - 4u ||
        !(hw->flags & IDW_HSW_HW_SIMULATED))
        return -EINVAL;
    sim = hw->context;
    if (!sim)
        return -EINVAL;
    *value = sim->registers[offset / 4u];
    return 0;
}

int idw_hsw_hw_sim_write_reg(struct idw_hsw_hw *hw, uint32_t offset,
                             uint32_t value)
{
    struct idw_hsw_hw_sim *sim = sim_from_hw(hw);

    if (!sim || (offset & 3u) || offset > IDW_HSW_SIM_REG_BYTES - 4u)
        return -EINVAL;
    sim_store_reg(sim, offset, value);
    return 0;
}

int idw_hsw_hw_sim_reset(struct idw_hsw_hw *hw)
{
    struct idw_hsw_hw_sim *sim = sim_from_hw(hw);

    if (!sim)
        return -EINVAL;
    memset(sim->registers, 0, IDW_HSW_SIM_REG_BYTES);
    memset(sim->gtt, 0, (size_t)sim->ggtt_bytes);
    sim->fail_mask = 0;
    sim->fail_once = 0;
    sim->fail_engine = 0;
    sim->engines = 0;
    sim->trace_count = 0;
    sim->trace_overflow = 0;
    memset(sim->trace, 0, sizeof(sim->trace));
    memset(&sim->stats, 0, sizeof(sim->stats));
    hw->active = false;
    hw->active_engines = 0;
    hw->saved_mask = 0;
    hw->poisoned = false;
    return 0;
}

int idw_hsw_hw_sim_read_gtt(const struct idw_hsw_hw *hw, uint64_t offset,
                            uint32_t *value)
{
    struct idw_hsw_hw_sim *sim;

    if (!hw || !value || (offset & 3u) ||
        !(hw->flags & IDW_HSW_HW_SIMULATED))
        return -EINVAL;
    sim = hw->context;
    if (!sim || offset > sim->ggtt_bytes - 4u)
        return -EINVAL;
    *value = sim->gtt[offset / 4u];
    return 0;
}

uint32_t idw_hsw_hw_sim_engine_mask(const struct idw_hsw_hw *hw)
{
    struct idw_hsw_hw_sim *sim;

    if (!hw || !(hw->flags & IDW_HSW_HW_SIMULATED))
        return 0;
    sim = hw->context;
    return sim ? sim->engines : 0;
}

int idw_hsw_hw_sim_fail_engine(struct idw_hsw_hw *hw, uint32_t engine)
{
    struct idw_hsw_hw_sim *sim = sim_from_hw(hw);

    if (!sim || !engine || (engine & (engine - 1)) ||
        (engine & ~IDW_HSW_ENGINE_ALL))
        return -EINVAL;
    sim->fail_engine = engine;
    return 0;
}

int idw_hsw_hw_sim_trace_reset(struct idw_hsw_hw *hw)
{
    struct idw_hsw_hw_sim *sim = sim_from_hw(hw);

    if (!sim)
        return -EINVAL;
    sim->trace_count = 0;
    sim->trace_overflow = 0;
    memset(sim->trace, 0, sizeof(sim->trace));
    return 0;
}

uint32_t idw_hsw_hw_sim_trace_count(const struct idw_hsw_hw *hw)
{
    struct idw_hsw_hw_sim *sim;

    if (!hw || !(hw->flags & IDW_HSW_HW_SIMULATED))
        return 0;
    sim = hw->context;
    return sim ? sim->trace_count : 0;
}

int idw_hsw_hw_sim_trace_get(const struct idw_hsw_hw *hw, uint32_t index,
                             struct idw_hsw_hw_sim_event *event)
{
    struct idw_hsw_hw_sim *sim;

    if (!hw || !event || !(hw->flags & IDW_HSW_HW_SIMULATED))
        return -EINVAL;
    sim = hw->context;
    if (!sim || index >= sim->trace_count)
        return -EINVAL;
    *event = sim->trace[index];
    return 0;
}

int idw_hsw_hw_sim_trace_overflowed(const struct idw_hsw_hw *hw)
{
    struct idw_hsw_hw_sim *sim;

    if (!hw || !(hw->flags & IDW_HSW_HW_SIMULATED))
        return -EINVAL;
    sim = hw->context;
    if (!sim)
        return -EINVAL;
    return sim->trace_overflow ? 1 : 0;
}
