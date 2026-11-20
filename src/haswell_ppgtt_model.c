/* SPDX-License-Identifier: MIT */
#include "../include/idw/haswell_ppgtt_model.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define HSW_PTE_VALID 0x1u
#define HSW_PDE_VALID 0x1u
/* Upstream Gen6/7 intel_gtt.h defines HSW_PTE_UNCACHED as zero. */
#define HSW_PTE_UNCACHED 0u
#define HSW_DMA_LIMIT (1ULL << IDW_HSW_PPGTT_DMA_ADDRESS_BITS)
#define HSW_PTE_ADDRESS_LOW_MASK 0xfffff000u
#define HSW_PTE_ADDRESS_HIGH_MASK 0x00000ff0u

struct idw_hsw_pt {
    struct idw_dma_page page;
};

struct idw_hsw_staged_table {
    uint32_t index;
    struct idw_hsw_pt *table;
};

struct idw_hsw_ppgtt_model {
    struct idw_hsw_ppgtt_page_ops page_ops;
    uint32_t directory[IDW_HSW_PPGTT_PDES];
    struct idw_hsw_pt *tables[IDW_HSW_PPGTT_PDES];
    struct idw_hsw_pt *retired[IDW_HSW_PPGTT_PDES];
    uint32_t retired_count;
    uint64_t generation;
    idw_gpuva dirty_start;
    uint64_t dirty_end;
    bool invalidation_pending;
    bool poisoned;
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

static int dma_address_valid(uint64_t address)
{
    if (address >= HSW_DMA_LIMIT ||
        (address & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1)))
        return -ERANGE;
    return 0;
}

static uint32_t encode_address(uint64_t address)
{
    return (uint32_t)address | (uint32_t)((address >> 28) & 0xff0u);
}

static uint64_t decode_address(uint32_t entry)
{
    return (uint64_t)(entry & HSW_PTE_ADDRESS_LOW_MASK) |
           ((uint64_t)(entry & HSW_PTE_ADDRESS_HIGH_MASK) << 28);
}

static bool pt_empty(const struct idw_hsw_pt *table)
{
    const uint32_t *ptes = table->page.cpu_address;
    uint32_t i;

    for (i = 0; i < IDW_HASWELL_PPGTT_PTES_PER_TABLE; ++i) {
        if (ptes[i] & HSW_PTE_VALID)
            return false;
    }
    return true;
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
    ret = model->page_ops.alloc_page(model->page_ops.context, &table->page);
    if (ret) {
        free(table);
        return ret < 0 ? ret : -EIO;
    }
    if (table->page.size < sizeof(table->page) ||
        !table->page.cpu_address ||
        table->page.bytes < IDW_HASWELL_PPGTT_PAGE_SIZE ||
        ((uintptr_t)table->page.cpu_address &
         (IDW_HASWELL_PPGTT_PAGE_SIZE - 1)) ||
        dma_address_valid(table->page.dma_address)) {
        model->page_ops.free_page(model->page_ops.context, &table->page);
        free(table);
        return -EINVAL;
    }
    if (!idw_pt_scope_equal(&model->page_ops.scope, &table->page.scope)) {
        model->page_ops.free_page(model->page_ops.context, &table->page);
        free(table);
        return -EXDEV;
    }
    if (table->page.memory_type != IDW_PT_PAGE_COHERENT &&
        table->page.memory_type != IDW_PT_PAGE_STREAMING) {
        model->page_ops.free_page(model->page_ops.context, &table->page);
        free(table);
        return -EINVAL;
    }
    if (!table->page.allocation_cookie) {
        model->page_ops.free_page(model->page_ops.context, &table->page);
        free(table);
        return -EINVAL;
    }
    memset(table->page.cpu_address, 0, IDW_HASWELL_PPGTT_PAGE_SIZE);
    *out = table;
    return 0;
}

static void free_table(struct idw_hsw_ppgtt_model *model,
                       struct idw_hsw_pt *table)
{
    if (!table)
        return;
    model->page_ops.free_page(model->page_ops.context, &table->page);
    free(table);
}

static bool table_aliases(const struct idw_hsw_ppgtt_model *model,
                          const struct idw_hsw_pt *candidate,
                          const struct idw_hsw_staged_table *staged,
                          uint32_t staged_count)
{
    uint32_t i;

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

int idw_hsw_ppgtt_model_create(
    const struct idw_hsw_ppgtt_page_ops *page_ops,
    struct idw_hsw_ppgtt_model **out)
{
    struct idw_hsw_ppgtt_model *model;

    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!page_ops || page_ops->size != sizeof(*page_ops) ||
        page_ops->version != 1 || !page_ops->alloc_page ||
        !page_ops->free_page || !page_ops->scope.device_cookie ||
        !page_ops->scope.generation)
        return -EINVAL;
    model = calloc(1, sizeof(*model));
    if (!model)
        return -ENOMEM;
    model->page_ops = *page_ops;
    *out = model;
    return 0;
}

void idw_hsw_ppgtt_model_destroy(struct idw_hsw_ppgtt_model *model)
{
    uint32_t i;

    if (!model)
        return;
    for (i = 0; i < IDW_HSW_PPGTT_PDES; ++i)
        free_table(model, model->tables[i]);
    for (i = 0; i < model->retired_count; ++i)
        free_table(model, model->retired[i]);
    free(model);
}

int idw_hsw_ppgtt_map(struct idw_hsw_ppgtt_model *model,
                      idw_gpuva va, uint64_t bytes,
                      const struct idw_dma_page_mapping *dma_pages,
                      uint64_t page_count)
{
    struct idw_hsw_staged_table staged[IDW_HSW_PPGTT_PDES];
    uint32_t staged_count = 0;
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
        ret = dma_address_valid(dma_pages[i].dma_address);
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
            for (j = 0; j < staged_count && staged[j].index != pde; ++j) {}
            if (j == staged_count) {
                ret = allocate_table(model, &staged[staged_count].table);
                if (ret)
                    goto rollback;
                if (table_aliases(model, staged[staged_count].table,
                                  staged,
                                  staged_count)) {
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
        if (((uint32_t *)table->page.cpu_address)[pte] & HSW_PTE_VALID) {
            ret = -EEXIST;
            goto rollback;
        }
    }

    for (i = 0; i < staged_count; ++i) {
        uint32_t index = staged[i].index;
        model->tables[index] = staged[i].table;
        model->directory[index] =
            encode_address(staged[i].table->page.dma_address) | HSW_PDE_VALID;
        staged[i].table = NULL;
    }
    for (i = 0; i < page_count; ++i) {
        uint64_t page_va = va + i * IDW_HASWELL_PPGTT_PAGE_SIZE;
        uint32_t pde = (uint32_t)(page_va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint32_t pte = (uint32_t)((page_va >> 12) & 0x3ffu);
        ((uint32_t *)model->tables[pde]->page.cpu_address)[pte] =
            encode_address(dma_pages[i].dma_address) |
            HSW_PTE_UNCACHED | HSW_PTE_VALID;
    }
    ret = mark_dirty(model, va, bytes);
    if (ret) {
        model->poisoned = true;
        return ret;
    }
    return 0;

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
    uint32_t first_pde;
    uint32_t last_pde;
    uint32_t retire_count = 0;
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
    first_pde = (uint32_t)(va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
    last_pde = (uint32_t)((va + bytes - 1) >>
                           IDW_HASWELL_PPGTT_PDE_SHIFT);

    for (i = 0; i < page_count; ++i) {
        uint64_t page_va = va + i * IDW_HASWELL_PPGTT_PAGE_SIZE;
        uint32_t pde = (uint32_t)(page_va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint32_t pte = (uint32_t)((page_va >> 12) & 0x3ffu);

        if (!model->tables[pde] ||
            !(((uint32_t *)model->tables[pde]->page.cpu_address)[pte] &
              HSW_PTE_VALID))
            return -ENOENT;
    }
    for (i = first_pde; i <= last_pde; ++i) {
        uint64_t pde_start = i * (1ULL << IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint64_t pde_end = pde_start + (1ULL << IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint64_t clear_start = va > pde_start ? va : pde_start;
        uint64_t map_end = va + bytes;
        uint64_t clear_end = map_end < pde_end ? map_end : pde_end;
        struct idw_hsw_pt *table = model->tables[i];
        uint32_t pte_start = (uint32_t)((clear_start - pde_start) >> 12);
        uint32_t pte_end = (uint32_t)((clear_end - pde_start) >> 12);
        bool empty = true;
        uint32_t scan;

        for (scan = 0; scan < IDW_HASWELL_PPGTT_PTES_PER_TABLE; ++scan) {
            bool will_clear = scan >= pte_start && scan < pte_end;
            if (!will_clear &&
                (((uint32_t *)table->page.cpu_address)[scan] & HSW_PTE_VALID)) {
                empty = false;
                break;
            }
        }
        if (empty)
            ++retire_count;
    }
    if (retire_count > IDW_HSW_PPGTT_PDES - model->retired_count)
        return -ENOSPC;

    for (i = first_pde; i <= last_pde; ++i) {
        uint64_t pde_start = i * (1ULL << IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint64_t pde_end = pde_start + (1ULL << IDW_HASWELL_PPGTT_PDE_SHIFT);
        uint64_t clear_start = va > pde_start ? va : pde_start;
        uint64_t map_end = va + bytes;
        uint64_t clear_end = map_end < pde_end ? map_end : pde_end;
        struct idw_hsw_pt *table = model->tables[i];
        uint32_t pte_start = (uint32_t)((clear_start - pde_start) >> 12);
        uint32_t pte_end = (uint32_t)((clear_end - pde_start) >> 12);
        uint32_t pte;

        for (pte = pte_start; pte < pte_end; ++pte)
            ((uint32_t *)table->page.cpu_address)[pte] = 0;
        if (pt_empty(table)) {
            model->directory[i] = 0;
            model->tables[i] = NULL;
            model->retired[model->retired_count++] = table;
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

    if (!model || !translation || translation->size != sizeof(*translation))
        return -EINVAL;
    if (model->poisoned)
        return -EIO;
    if (va >= IDW_HASWELL_PPGTT_GEN7_ADDRESS_SPACE_SIZE)
        return -ERANGE;
    pde = (uint32_t)(va >> IDW_HASWELL_PPGTT_PDE_SHIFT);
    pte = (uint32_t)((va >> 12) & 0x3ffu);
    if (!model->tables[pde] || !(model->directory[pde] & HSW_PDE_VALID))
        return -ENOENT;
    entry = ((uint32_t *)model->tables[pde]->page.cpu_address)[pte];
    if (!(entry & HSW_PTE_VALID))
        return -ENOENT;
    translation->pde_index = pde;
    translation->pte_index = pte;
    translation->pde = model->directory[pde];
    translation->pte = entry;
    translation->dma_address = decode_address(entry) |
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
    return model->invalidation_pending ? -EAGAIN : 0;
}

uint32_t idw_hsw_ppgtt_pde(const struct idw_hsw_ppgtt_model *model,
                           uint32_t pde_index)
{
    if (!model || pde_index >= IDW_HSW_PPGTT_PDES)
        return 0;
    return model->directory[pde_index];
}
