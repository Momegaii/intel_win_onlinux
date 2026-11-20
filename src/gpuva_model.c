/* SPDX-License-Identifier: MIT */
#include "../include/idw/gpuva_model.h"

#include <errno.h>
#include <stdlib.h>

#define GPUVA_PAGE_SIZE 4096ULL

struct idw_gpuva_range {
    uint64_t reservation_start;
    uint64_t reservation_end;
    idw_gpuva va;
    uint64_t bytes;
    struct idw_gpuva_range *next;
};

static int add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return -EOVERFLOW;
    *out = a + b;
    return 0;
}

static int align_up_u64(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t remainder;
    uint64_t padding;

    if (!alignment)
        return -EINVAL;
    remainder = value % alignment;
    padding = remainder ? alignment - remainder : 0;
    return add_u64(value, padding, out);
}

int idw_gpuva_space_init(struct idw_gpuva_space *space,
                         uint64_t min_va, uint64_t max_va)
{
    if (!space || min_va >= max_va ||
        (min_va & (GPUVA_PAGE_SIZE - 1)) ||
        (max_va & (GPUVA_PAGE_SIZE - 1)))
        return -EINVAL;
    space->min_va = min_va;
    space->max_va = max_va;
    space->ranges = NULL;
    return 0;
}

void idw_gpuva_space_fini(struct idw_gpuva_space *space)
{
    struct idw_gpuva_range *range;

    if (!space)
        return;
    range = space->ranges;
    while (range) {
        struct idw_gpuva_range *next = range->next;
        free(range);
        range = next;
    }
    space->ranges = NULL;
    space->min_va = 0;
    space->max_va = 0;
}

static int reserve_range(struct idw_gpuva_space *space, idw_gpuva va,
                         uint64_t bytes, uint64_t guard_bytes)
{
    struct idw_gpuva_range **link;
    struct idw_gpuva_range *range;
    uint64_t reservation_start;
    uint64_t reservation_end;

    if (!space || !space->max_va || !bytes ||
        (va & (GPUVA_PAGE_SIZE - 1)) ||
        (bytes & (GPUVA_PAGE_SIZE - 1)) ||
        (guard_bytes & (GPUVA_PAGE_SIZE - 1)))
        return -EINVAL;
    if (va < space->min_va || va >= space->max_va)
        return -ERANGE;
    if (va < guard_bytes)
        return -EOVERFLOW;
    reservation_start = va - guard_bytes;
    if (add_u64(va, bytes, &reservation_end) ||
        add_u64(reservation_end, guard_bytes, &reservation_end))
        return -EOVERFLOW;
    if (reservation_start < space->min_va ||
        reservation_end > space->max_va)
        return -ERANGE;

    link = &space->ranges;
    while (*link && (*link)->reservation_start < reservation_start) {
        if ((*link)->reservation_end > reservation_start)
            return -EEXIST;
        link = &(*link)->next;
    }
    if (*link && reservation_end > (*link)->reservation_start)
        return -EEXIST;

    range = calloc(1, sizeof(*range));
    if (!range)
        return -ENOMEM;
    range->reservation_start = reservation_start;
    range->reservation_end = reservation_end;
    range->va = va;
    range->bytes = bytes;
    range->next = *link;
    *link = range;
    return 0;
}

int idw_gpuva_reserve(struct idw_gpuva_space *space, idw_gpuva va,
                      uint64_t bytes, uint64_t guard_bytes)
{
    return reserve_range(space, va, bytes, guard_bytes);
}

int idw_gpuva_alloc(struct idw_gpuva_space *space, uint64_t bytes,
                    uint64_t alignment, uint64_t guard_bytes,
                    idw_gpuva *va)
{
    uint64_t candidate;
    uint64_t first_usable;
    uint64_t reservation_start;
    struct idw_gpuva_range *range;
    int ret;

    if (!space || !space->max_va || space->min_va >= space->max_va ||
        !va || !bytes ||
        (bytes & (GPUVA_PAGE_SIZE - 1)) ||
        (guard_bytes & (GPUVA_PAGE_SIZE - 1)) ||
        alignment < GPUVA_PAGE_SIZE ||
        (alignment & (alignment - 1)))
        return -EINVAL;

    ret = add_u64(space->min_va, guard_bytes, &first_usable);
    if (ret)
        return ret;
    ret = align_up_u64(first_usable, alignment, &candidate);
    if (ret)
        return ret;
    for (range = space->ranges; ; range = range ? range->next : NULL) {
        uint64_t end;

        if (candidate < guard_bytes)
            return -EOVERFLOW;
        reservation_start = candidate - guard_bytes;
        if (range && reservation_start < range->reservation_start) {
            if (add_u64(candidate, bytes, &end) ||
                add_u64(end, guard_bytes, &end))
                return -EOVERFLOW;
            if (end <= range->reservation_start)
                break;
        }
        if (!range) {
            if (add_u64(candidate, bytes, &end) ||
                add_u64(end, guard_bytes, &end))
                return -EOVERFLOW;
            if (end > space->max_va)
                return -ENOSPC;
            break;
        }
        ret = add_u64(range->reservation_end, guard_bytes, &first_usable);
        if (ret)
            return ret;
        ret = align_up_u64(first_usable, alignment, &candidate);
        if (ret)
            return ret;
    }

    ret = reserve_range(space, candidate, bytes, guard_bytes);
    if (!ret)
        *va = candidate;
    return ret;
}

int idw_gpuva_free(struct idw_gpuva_space *space, idw_gpuva va)
{
    struct idw_gpuva_range **link;

    if (!space || !space->max_va)
        return -EINVAL;
    link = &space->ranges;
    while (*link && (*link)->va != va)
        link = &(*link)->next;
    if (!*link)
        return -ENOENT;
    {
        struct idw_gpuva_range *range = *link;
        *link = range->next;
        free(range);
    }
    return 0;
}
