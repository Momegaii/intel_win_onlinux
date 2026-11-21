/* SPDX-License-Identifier: MIT */
#include "../include/idw/haswell_ppgtt_model.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define HSW_PT_DMA_LIMIT (1ULL << IDW_HSW_PPGTT_PT_DMA_ADDRESS_BITS)
#define HSW_PAGE_DMA_LIMIT (1ULL << IDW_HSW_PPGTT_PAGE_DMA_ADDRESS_BITS)

struct idw_hsw_pt {
    struct idw_dma_page page;
    bool empty;
};

struct idw_hsw_staged_table {
    uint32_t index;
    struct idw_hsw_pt *table;
};

struct idw_hsw_ppgtt_model {
    struct idw_hsw_ppgtt_page_ops page_ops;
    struct idw_hsw_ppgtt_directory_ops directory_ops;
    struct idw_hsw_ppgtt_directory directory;
    bool directory_allocated;
    uint32_t shadow[IDW_HSW_PPGTT_PDES];
    struct idw_hsw_pt *tables[IDW_HSW_PPGTT_PDES];
    struct idw_hsw_pt *retired[IDW_HSW_PPGTT_PDES];
    struct idw_hsw_pt *scratch;
    uint32_t retired_count;
    uint64_t generation;
    idw_gpuva dirty_start;
    uint64_t dirty_end;
    bool invalidation_pending;
    bool poisoned;
    bool active;
};

static int range_valid(idw_gpuva va, uint64_t bytes)
{
    uint64_t address_space_size = IDW_HASWELL_PPGTT_GEN7_ADDRESS_SPACE_SIZE;

    if (!bytes || (va & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1)) ||
        (bytes & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1)))
        return -EINVAL;
    if (va >= address_space_size || bytes > address_space_size - va)
        return -ERANGE;
    return 0;
}

static int pt_dma_valid(uint64_t address)
{
    if (address >= HSW_PT_DMA_LIMIT)
        return -ERANGE;
    if (address & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1))
        return -EINVAL;
    return 0;
}

static int page_dma_valid(uint64_t address)
{
    if (address >= HSW_PAGE_DMA_LIMIT)
        return -ERANGE;
    if (address & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1))
        return -EINVAL;
    return 0;
}

static bool pt_empty(const struct idw_hsw_pt *table)
{
    const uint32_t *ptes = table->page.cpu_address;
    uint32_t i;

    for (i = 0; i < IDW_HASWELL_PPGTT_PTES_PER_TABLE; ++i) {
        if (ptes[i] & IDW_HSW_PPGTT_PTE_VALID)
            return false;
    }
    return true;
}

static int write_pde(struct idw_hsw_ppgtt_model *model, uint32_t index,
                     uint32_t value)
{
    uint32_t readback = 0;
    int ret;

    ret = model->directory_ops.write_pde_gttaddr(
        model->directory_ops.context, &model->directory, index, value);
    if (ret)
        return ret < 0 ? ret : -EIO;
    ret = model->directory_ops.read_pde_gttaddr(
        model->directory_ops.context, &model->directory, index, &readback);
    if (ret)
        return ret < 0 ? ret : -EIO;
    if (readback != value)
        return -EIO;
    model->shadow[index] = value;
    return 0;
}

static int sync_table(struct idw_hsw_ppgtt_model *model,
                      const struct idw_hsw_pt *table)
{
    int ret = model->page_ops.sync_page_table_for_device(
        model->page_ops.context, &table->page, 0,
        IDW_HASWELL_PPGTT_PAGE_SIZE);

    return ret ? (ret < 0 ? ret : -EIO) : 0;
}

static int flush_directory(struct idw_hsw_ppgtt_model *model)
{
    int ret = model->directory_ops.flush_gtt(model->directory_ops.context,
                                             &model->directory);

    return ret ? (ret < 0 ? ret : -EIO) : 0;
}

static int initialize_directory(struct idw_hsw_ppgtt_model *model)
{
    uint32_t i;
    int ret;

    for (i = 0; i < IDW_HSW_PPGTT_PDES; ++i) {
        ret = write_pde(model, i, 0);
        if (ret)
            return ret;
    }
    return flush_directory(model);
}

static int allocate_table(struct idw_hsw_ppgtt_model *model,
                          struct idw_hsw_pt **out)
{
    struct idw_hsw_pt *table;
    int ret;

    table = calloc(1, sizeof(*table));
    if (!table)
        return -ENOMEM;
    table->page.size = sizeof(table->page);
    ret = model->page_ops.alloc_page_table_page(model->page_ops.context,
                                                &table->page);
    if (ret) {
        free(table);
        return ret < 0 ? ret : -EIO;
    }
    if (table->page.size < sizeof(table->page) ||
        !table->page.cpu_address ||
        table->page.bytes < IDW_HASWELL_PPGTT_PAGE_SIZE ||
        ((uintptr_t)table->page.cpu_address &
         (IDW_HASWELL_PPGTT_PAGE_SIZE - 1))) {
        model->page_ops.free_page_table_page(model->page_ops.context,
                                             &table->page);
        free(table);
        return -EINVAL;
    }
    ret = pt_dma_valid(table->page.dma_address);
    if (ret) {
        model->page_ops.free_page_table_page(model->page_ops.context,
                                             &table->page);
        free(table);
        return ret;
    }
    if (!idw_pt_scope_equal(&model->page_ops.scope, &table->page.scope)) {
        model->page_ops.free_page_table_page(model->page_ops.context,
                                             &table->page);
        free(table);
        return -EXDEV;
    }
    if (table->page.memory_type != IDW_PT_PAGE_COHERENT &&
        table->page.memory_type != IDW_PT_PAGE_STREAMING) {
        model->page_ops.free_page_table_page(model->page_ops.context,
                                             &table->page);
        free(table);
        return -EINVAL;
    }
    if (!table->page.allocation_cookie) {
        model->page_ops.free_page_table_page(model->page_ops.context,
                                             &table->page);
        free(table);
        return -EINVAL;
    }
    memset(table->page.cpu_address, 0, IDW_HASWELL_PPGTT_PAGE_SIZE);
    ret = sync_table(model, table);
    if (ret) {
        model->page_ops.free_page_table_page(model->page_ops.context,
                                             &table->page);
        free(table);
        return ret;
    }
    table->empty = true;
    *out = table;
    return 0;
}

static void free_table(struct idw_hsw_ppgtt_model *model,
                       struct idw_hsw_pt *table)
{
    if (!table)
        return;
    model->page_ops.free_page_table_page(model->page_ops.context,
                                         &table->page);
    free(table);
}

static bool table_aliases(const struct idw_hsw_ppgtt_model *model,
                          const struct idw_hsw_pt *candidate,
                          const struct idw_hsw_staged_table *staged,
                          uint32_t staged_count)
{
    uint32_t i;

    if (model->scratch &&
        (model->scratch->page.cpu_address == candidate->page.cpu_address ||
         model->scratch->page.dma_address == candidate->page.dma_address ||
         model->scratch->page.allocation_cookie ==
             candidate->page.allocation_cookie))
        return true;
    for (i = 0; i < IDW_HSW_PPGTT_PDES; ++i) {
        const struct idw_hsw_pt *table = model->tables[i];
        if (table && (table->page.cpu_address == candidate->page.cpu_address ||
                      table->page.dma_address == candidate->page.dma_address ||
                      table->page.allocation_cookie ==
                          candidate->page.allocation_cookie))
            return true;
    }
    for (i = 0; i < model->retired_count; ++i) {
        const struct idw_hsw_pt *table = model->retired[i];
        if (table->page.cpu_address == candidate->page.cpu_address ||
            table->page.dma_address == candidate->page.dma_address ||
            table->page.allocation_cookie == candidate->page.allocation_cookie)
            return true;
    }
    for (i = 0; i < staged_count; ++i) {
        const struct idw_hsw_pt *table = staged[i].table;
        if (table && (table->page.cpu_address == candidate->page.cpu_address ||
                      table->page.dma_address == candidate->page.dma_address ||
                      table->page.allocation_cookie ==
                          candidate->page.allocation_cookie))
            return true;
    }
    return false;
}

static int mark_dirty(struct idw_hsw_ppgtt_model *model,
                      idw_gpuva start, uint64_t bytes)
{
    uint64_t end = start + bytes;

    if (model->generation == UINT64_MAX)
        return -EOVERFLOW;
    ++model->generation;
    if (!model->invalidation_pending) {
        model->dirty_start = start;
        model->dirty_end = end;
        model->invalidation_pending = true;
    } else {
        if (start < model->dirty_start)
            model->dirty_start = start;
        if (end > model->dirty_end)
            model->dirty_end = end;
    }
    return 0;
}

static int restore_pdes(struct idw_hsw_ppgtt_model *model,
                        const uint32_t *indexes, const uint32_t *values,
                        uint32_t count)
{
    uint32_t i;
    int ret;

    for (i = 0; i < count; ++i) {
        ret = write_pde(model, indexes[i], values[i]);
        if (ret)
            return ret;
    }
    return flush_directory(model);
}

int idw_hsw_ppgtt_model_create(
    const struct idw_hsw_ppgtt_page_ops *page_ops,
    const struct idw_hsw_ppgtt_directory_ops *directory_ops,
    struct idw_hsw_ppgtt_model **out)
{
    struct idw_hsw_ppgtt_model *model;
    int ret;

    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!page_ops || page_ops->size != sizeof(*page_ops) ||
        page_ops->version != 1 || !page_ops->alloc_page_table_page ||
        !page_ops->free_page_table_page ||
        !page_ops->sync_page_table_for_device ||
        !page_ops->scope.device_cookie ||
        !page_ops->scope.generation)
        return -EINVAL;
    if (!directory_ops || directory_ops->size != sizeof(*directory_ops) ||
        directory_ops->version != IDW_HSW_PPGTT_DIRECTORY_VERSION ||
        !directory_ops->alloc_directory ||
        !directory_ops->free_directory ||
        !directory_ops->write_pde_gttaddr ||
        !directory_ops->read_pde_gttaddr || !directory_ops->flush_gtt ||
        !directory_ops->scope.device_cookie ||
        !directory_ops->scope.generation)
        return -EINVAL;
    if (!idw_pt_scope_equal(&page_ops->scope, &directory_ops->scope))
        return -EXDEV;
    model = calloc(1, sizeof(*model));
    if (!model)
        return -ENOMEM;
    model->page_ops = *page_ops;
    model->directory_ops = *directory_ops;
    model->directory.size = sizeof(model->directory);
    model->directory.version = IDW_HSW_PPGTT_DIRECTORY_VERSION;
    ret = model->directory_ops.alloc_directory(model->directory_ops.context,
                                               &model->directory);
    if (ret) {
        free(model);
        return ret < 0 ? ret : -EIO;
    }
    model->directory_allocated = true;
    if (model->directory.size < sizeof(model->directory) ||
        model->directory.version != IDW_HSW_PPGTT_DIRECTORY_VERSION ||
        model->directory.bytes < IDW_HSW_PPGTT_DIRECTORY_BYTES ||
        (model->directory.ggtt_offset &
         (IDW_HSW_PPGTT_DIRECTORY_ALIGN - 1)) ||
        model->directory.ggtt_offset >
            UINT64_MAX - model->directory.bytes ||
        !model->directory.allocation_cookie ||
        !idw_pt_scope_equal(&model->page_ops.scope,
                            &model->directory.scope)) {
        model->directory_ops.free_directory(model->directory_ops.context,
                                            &model->directory);
        free(model);
        return -EINVAL;
    }
    ret = initialize_directory(model);
    if (ret) {
        model->directory_ops.free_directory(model->directory_ops.context,
                                            &model->directory);
        free(model);
        return ret;
    }
    *out = model;
    return 0;
}

int idw_hsw_ppgtt_model_destroy_checked(struct idw_hsw_ppgtt_model *model,
                                        bool hardware_quiesced)
{
    uint32_t i;

    if (!model)
        return -EINVAL;
    if (!hardware_quiesced &&
        (model->active || model->invalidation_pending))
        return -EBUSY;
    for (i = 0; i < IDW_HSW_PPGTT_PDES; ++i)
        free_table(model, model->tables[i]);
    for (i = 0; i < model->retired_count; ++i)
        free_table(model, model->retired[i]);
    free_table(model, model->scratch);
    if (model->directory_allocated)
        model->directory_ops.free_directory(model->directory_ops.context,
                                            &model->directory);
    free(model);
    return 0;
}

void idw_hsw_ppgtt_model_destroy(struct idw_hsw_ppgtt_model *model)
{
    (void)idw_hsw_ppgtt_model_destroy_checked(model, false);
}

int idw_hsw_ppgtt_map(struct idw_hsw_ppgtt_model *model,
                      idw_gpuva va, uint64_t bytes,
                      const struct idw_dma_page_mapping *dma_pages,
                      uint64_t page_count)
{
    struct idw_hsw_staged_table staged[IDW_HSW_PPGTT_PDES];
    bool sync_needed[IDW_HSW_PPGTT_PDES] = { false };
    uint32_t staged_count = 0;
    uint32_t written_count = 0;
    uint32_t written_index[IDW_HSW_PPGTT_PDES];
    uint32_t written_old[IDW_HSW_PPGTT_PDES];
    uint64_t expected_pages;
    uint64_t i;
    int ret;

    if (!model || !dma_pages)
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    ret = range_valid(va, bytes);
    if (ret)
        return ret;
    expected_pages = bytes / IDW_HASWELL_PPGTT_PAGE_SIZE;
    if (page_count != expected_pages)
        return -EINVAL;
    for (i = 0; i < page_count; ++i) {
        if (dma_pages[i].bytes != IDW_HASWELL_PPGTT_PAGE_SIZE ||
            !dma_pages[i].mapping_cookie)
            return -EINVAL;
        if (!idw_pt_scope_equal(&model->page_ops.scope,
                                &dma_pages[i].scope))
            return -EXDEV;
        ret = page_dma_valid(dma_pages[i].dma_address);
        if (ret)
            return ret;
    }
    if (model->generation == UINT64_MAX)
        return -EOVERFLOW;

    for (i = 0; i < page_count; ++i) {
        uint64_t page_va = va + i * IDW_HASWELL_PPGTT_PAGE_SIZE;
        uint32_t pde = (uint32_t)(page_va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint32_t pte = (uint32_t)((page_va >> 12) & 0x3ffu);
        struct idw_hsw_pt *table = model->tables[pde];
        uint32_t j;

        if (!table) {
            if (model->active) {
                ret = -EBUSY;
                goto rollback;
            }
            for (j = 0; j < staged_count && staged[j].index != pde; ++j) {}
            if (j == staged_count) {
                ret = allocate_table(model, &staged[staged_count].table);
                if (ret)
                    goto rollback;
                if (table_aliases(model, staged[staged_count].table,
                                  staged, staged_count)) {
                    free_table(model, staged[staged_count].table);
                    staged[staged_count].table = NULL;
                    ret = -EEXIST;
                    goto rollback;
                }
                staged[staged_count].index = pde;
                ++staged_count;
            }
            table = staged[j].table;
        }
        if (((uint32_t *)table->page.cpu_address)[pte] &
            IDW_HSW_PPGTT_PTE_VALID) {
            ret = -EEXIST;
            goto rollback;
        }
    }

    for (i = 0; i < staged_count; ++i) {
        uint32_t index = staged[i].index;
        uint32_t pde_entry;

        ret = idw_hsw_ppgtt_encode_pde(staged[i].table->page.dma_address,
                                       &pde_entry);
        if (ret)
            goto rollback_published;
        written_index[written_count] = index;
        written_old[written_count] = model->shadow[index];
        ++written_count;
        ret = write_pde(model, index, pde_entry);
        if (ret)
            goto rollback_published;
    }
    if (written_count) {
        ret = flush_directory(model);
        if (ret)
            goto rollback_published;
    }

    for (i = 0; i < staged_count; ++i) {
        uint32_t index = staged[i].index;
        model->tables[index] = staged[i].table;
        staged[i].table = NULL;
    }
    for (i = 0; i < page_count; ++i) {
        uint64_t page_va = va + i * IDW_HASWELL_PPGTT_PAGE_SIZE;
        uint32_t pde = (uint32_t)(page_va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint32_t pte = (uint32_t)((page_va >> 12) & 0x3ffu);
        uint32_t pte_entry;

        ret = idw_hsw_ppgtt_encode_pte(dma_pages[i].dma_address,
                                       IDW_HSW_PPGTT_PTE_CACHE_UC,
                                       &pte_entry);
        if (ret)
            return ret;
        ((uint32_t *)model->tables[pde]->page.cpu_address)[pte] = pte_entry;
        model->tables[pde]->empty = false;
        sync_needed[pde] = true;
    }
    for (i = 0; i < IDW_HSW_PPGTT_PDES; ++i) {
        if (!sync_needed[i])
            continue;
        ret = sync_table(model, model->tables[i]);
        if (ret) {
            model->poisoned = true;
            return ret;
        }
    }
    ret = mark_dirty(model, va, bytes);
    if (ret) {
        model->poisoned = true;
        return ret;
    }
    return 0;

rollback_published:
    if (written_count &&
        restore_pdes(model, written_index, written_old, written_count))
        model->poisoned = true;
rollback:
    while (staged_count)
        free_table(model, staged[--staged_count].table);
    return ret;
}

int idw_hsw_ppgtt_unmap(struct idw_hsw_ppgtt_model *model,
                        idw_gpuva va, uint64_t bytes)
{
    uint64_t page_count;
    uint64_t i;
    int ret;

    if (!model)
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    ret = range_valid(va, bytes);
    if (ret)
        return ret;
    if (model->generation == UINT64_MAX)
        return -EOVERFLOW;
    page_count = bytes / IDW_HASWELL_PPGTT_PAGE_SIZE;
    {
        bool sync_needed[IDW_HSW_PPGTT_PDES] = { false };

        for (i = 0; i < page_count; ++i) {
            uint64_t page_va = va + i * IDW_HASWELL_PPGTT_PAGE_SIZE;
            uint32_t pde = (uint32_t)(page_va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
            uint32_t pte = (uint32_t)((page_va >> 12) & 0x3ffu);

            if (!model->tables[pde] ||
                !(((uint32_t *)model->tables[pde]->page.cpu_address)[pte] &
                  IDW_HSW_PPGTT_PTE_VALID))
                return -ENOENT;
        }
        for (i = 0; i < page_count; ++i) {
            uint64_t page_va = va + i * IDW_HASWELL_PPGTT_PAGE_SIZE;
            uint32_t pde = (uint32_t)(page_va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
            uint32_t pte = (uint32_t)((page_va >> 12) & 0x3ffu);

            ((uint32_t *)model->tables[pde]->page.cpu_address)[pte] = 0;
            model->tables[pde]->empty = pt_empty(model->tables[pde]);
            sync_needed[pde] = true;
        }
        for (i = 0; i < IDW_HSW_PPGTT_PDES; ++i) {
            if (!sync_needed[i])
                continue;
            ret = sync_table(model, model->tables[i]);
            if (ret) {
                model->poisoned = true;
                return ret;
            }
        }
    }
    return mark_dirty(model, va, bytes);
}

int idw_hsw_ppgtt_translate(const struct idw_hsw_ppgtt_model *model,
                            idw_gpuva va,
                            struct idw_hsw_ppgtt_translation *translation)
{
    uint32_t pde;
    uint32_t pte;
    uint32_t entry;
    int ret;

    if (!model || !translation || translation->size != sizeof(*translation))
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    if (va >= IDW_HASWELL_PPGTT_GEN7_ADDRESS_SPACE_SIZE)
        return -ERANGE;
    pde = (uint32_t)(va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
    pte = (uint32_t)((va >> 12) & 0x3ffu);
    ret = idw_hsw_ppgtt_validate_pde_entry(model->shadow[pde]);
    if (ret)
        return -EIO;
    if (!(model->shadow[pde] & IDW_HSW_PPGTT_PDE_VALID) ||
        !model->tables[pde])
        return -ENOENT;
    if (idw_hsw_ppgtt_decode_pde(model->shadow[pde]) !=
        model->tables[pde]->page.dma_address)
        return -EIO;
    entry = ((uint32_t *)model->tables[pde]->page.cpu_address)[pte];
    ret = idw_hsw_ppgtt_validate_pte_entry(entry);
    if (ret)
        return -EIO;
    if (!(entry & IDW_HSW_PPGTT_PTE_VALID))
        return -ENOENT;
    translation->pde_index = pde;
    translation->pte_index = pte;
    translation->pde = model->shadow[pde];
    translation->pte = entry;
    translation->dma_address = idw_hsw_ppgtt_decode_pte(entry) |
                               (va & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1));
    return 0;
}

int idw_hsw_ppgtt_pending(
    const struct idw_hsw_ppgtt_model *model,
    struct idw_hsw_ppgtt_pending_invalidation *pending)
{
    if (!model || !pending || pending->size != sizeof(*pending))
        return -EINVAL;
    pending->pending = model->invalidation_pending;
    pending->generation = model->generation;
    pending->start = model->invalidation_pending ? model->dirty_start : 0;
    pending->bytes = model->invalidation_pending ?
                     model->dirty_end - model->dirty_start : 0;
    return 0;
}

int idw_hsw_ppgtt_invalidation_complete(
    struct idw_hsw_ppgtt_model *model, uint64_t generation)
{
    uint32_t i;

    if (!model)
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    if (!model->invalidation_pending || generation != model->generation)
        return -ESTALE;
    model->invalidation_pending = false;
    model->dirty_start = 0;
    model->dirty_end = 0;
    for (i = 0; i < model->retired_count; ++i)
        free_table(model, model->retired[i]);
    model->retired_count = 0;
    return 0;
}

int idw_hsw_ppgtt_invalidation_failed(
    struct idw_hsw_ppgtt_model *model, uint64_t generation)
{
    if (!model)
        return -EINVAL;
    if (!model->invalidation_pending || generation != model->generation)
        return -ESTALE;
    model->poisoned = true;
    return -EIO;
}

int idw_hsw_ppgtt_check_submit(const struct idw_hsw_ppgtt_model *model)
{
    if (!model)
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    if (!model->active)
        return -ENODEV;
    return model->invalidation_pending ? -EAGAIN : 0;
}

int idw_hsw_ppgtt_set_active(struct idw_hsw_ppgtt_model *model, bool active)
{
    if (!model)
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    if (active && model->invalidation_pending)
        return -EAGAIN;
    model->active = active;
    return 0;
}

int idw_hsw_ppgtt_is_active(const struct idw_hsw_ppgtt_model *model)
{
    return model && model->active;
}

int idw_hsw_ppgtt_reclaim_empty(struct idw_hsw_ppgtt_model *model)
{
    uint32_t indexes[IDW_HSW_PPGTT_PDES];
    uint32_t old_values[IDW_HSW_PPGTT_PDES];
    struct idw_hsw_pt *tables[IDW_HSW_PPGTT_PDES];
    uint32_t count = 0;
    uint32_t first = IDW_HSW_PPGTT_PDES;
    uint32_t last = 0;
    uint32_t scratch_pde;
    uint32_t i;
    int ret;

    if (!model)
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    if (model->active)
        return -EBUSY;
    if (model->invalidation_pending)
        return -EAGAIN;
    if (model->generation == UINT64_MAX)
        return -EOVERFLOW;

    for (i = 0; i < IDW_HSW_PPGTT_PDES; ++i) {
        if (!model->tables[i] || !model->tables[i]->empty)
            continue;
        if (count == IDW_HSW_PPGTT_PDES - model->retired_count)
            return -ENOSPC;
        indexes[count] = i;
        old_values[count] = model->shadow[i];
        tables[count] = model->tables[i];
        ++count;
        if (i < first)
            first = i;
        last = i;
    }
    if (!count)
        return 0;

    if (!model->scratch) {
        ret = allocate_table(model, &model->scratch);
        if (ret)
            return ret;
    }
    ret = idw_hsw_ppgtt_encode_pde(model->scratch->page.dma_address,
                                   &scratch_pde);
    if (ret)
        return ret;

    for (i = 0; i < count; ++i) {
        ret = write_pde(model, indexes[i], scratch_pde);
        if (ret)
            goto restore;
    }
    ret = flush_directory(model);
    if (ret)
        goto restore;

    for (i = 0; i < count; ++i) {
        model->tables[indexes[i]] = NULL;
        model->retired[model->retired_count++] = tables[i];
    }
    ret = mark_dirty(model, (idw_gpuva)first << IDW_HASWELL_PPGTT_PDE_SHIFT,
                     (uint64_t)(last - first + 1) <<
                     IDW_HASWELL_PPGTT_PDE_SHIFT);
    if (ret) {
        model->poisoned = true;
        return ret;
    }
    return 0;

restore:
    if (restore_pdes(model, indexes, old_values, count))
        model->poisoned = true;
    return ret;
}

uint32_t idw_hsw_ppgtt_pde(const struct idw_hsw_ppgtt_model *model,
                           uint32_t pde_index)
{
    if (!model || pde_index >= IDW_HSW_PPGTT_PDES)
        return 0;
    return model->shadow[pde_index];
}

int idw_hsw_ppgtt_hardware_pde(const struct idw_hsw_ppgtt_model *model,
                               uint32_t pde_index, uint32_t *pde)
{
    int ret;

    if (!model || !pde || pde_index >= IDW_HSW_PPGTT_PDES)
        return -EINVAL;
    ret = model->directory_ops.read_pde_gttaddr(
        model->directory_ops.context, &model->directory, pde_index, pde);
    return ret ? (ret < 0 ? ret : -EIO) : 0;
}

uint64_t idw_hsw_ppgtt_directory_ggtt_offset(
    const struct idw_hsw_ppgtt_model *model)
{
    return model ? model->directory.ggtt_offset : 0;
}
