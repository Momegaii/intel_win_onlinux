/* SPDX-License-Identifier: MIT */
#include "../include/idw/ggtt_allocator.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define IDW_GGTT_DMA_LIMIT (1ULL << 40)

struct idw_ggtt_record {
    uint64_t offset;
    uint64_t bytes;
    uint64_t alignment;
    uint64_t cookie;
    uint64_t dma_address;
    uint32_t flags;
    uint32_t pin_count;
    uint32_t resident;
    uint32_t bound;
    uint32_t reserved;
    struct idw_device_scope scope;
};

struct idw_ggtt {
    uint64_t base_offset;
    uint64_t bytes;
    uint32_t page_bytes;
    uint32_t capacity;
    uint32_t count;
    struct idw_ggtt_record *records;
};

static bool is_power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

static int align_up(uint64_t value, uint64_t alignment, uint64_t *result)
{
    uint64_t mask;

    if (!is_power_of_two(alignment))
        return -EINVAL;
    mask = alignment - 1;
    if (value > UINT64_MAX - mask)
        return -EOVERFLOW;
    *result = (value + mask) & ~mask;
    return 0;
}

static bool range_overlaps(uint64_t first, uint64_t first_bytes,
                           uint64_t second, uint64_t second_bytes)
{
    return first < second + second_bytes && second < first + first_bytes;
}

static int validate_geometry(const struct idw_ggtt *ggtt, uint64_t offset,
                             uint64_t bytes, uint64_t alignment)
{
    uint64_t end;

    if (!ggtt || !bytes || !alignment ||
        !is_power_of_two(alignment) || alignment < ggtt->page_bytes ||
        bytes & (ggtt->page_bytes - 1) ||
        offset & (ggtt->page_bytes - 1) ||
        offset < ggtt->base_offset ||
        offset > ggtt->base_offset + ggtt->bytes)
        return -EINVAL;
    if (bytes > ggtt->bytes || offset - ggtt->base_offset > ggtt->bytes - bytes)
        return -ERANGE;
    end = offset + bytes;
    if (end < offset)
        return -EOVERFLOW;
    return 0;
}

static struct idw_ggtt_record *find_record(struct idw_ggtt *ggtt,
                                           uint64_t offset, uint64_t cookie)
{
    uint32_t i;

    for (i = 0; i < ggtt->count; ++i) {
        if (ggtt->records[i].offset == offset &&
            ggtt->records[i].cookie == cookie)
            return &ggtt->records[i];
    }
    return NULL;
}

static const struct idw_ggtt_record *find_record_const(
    const struct idw_ggtt *ggtt, uint64_t offset, uint64_t cookie)
{
    uint32_t i;

    for (i = 0; i < ggtt->count; ++i) {
        if (ggtt->records[i].offset == offset &&
            ggtt->records[i].cookie == cookie)
            return &ggtt->records[i];
    }
    return NULL;
}

static bool cookie_in_use(const struct idw_ggtt *ggtt, uint64_t cookie)
{
    uint32_t i;

    for (i = 0; i < ggtt->count; ++i) {
        if (ggtt->records[i].cookie == cookie)
            return true;
    }
    return false;
}

static int insert_record(struct idw_ggtt *ggtt,
                         const struct idw_ggtt_record *record)
{
    uint32_t index = 0;
    uint32_t i;

    if (ggtt->count == ggtt->capacity)
        return -ENOSPC;
    while (index < ggtt->count &&
           ggtt->records[index].offset < record->offset)
        ++index;
    for (i = 0; i < ggtt->count; ++i) {
        if (range_overlaps(record->offset, record->bytes,
                           ggtt->records[i].offset,
                           ggtt->records[i].bytes))
            return -EEXIST;
    }
    if (index < ggtt->count)
        memmove(&ggtt->records[index + 1], &ggtt->records[index],
                (ggtt->count - index) * sizeof(ggtt->records[0]));
    ggtt->records[index] = *record;
    ++ggtt->count;
    return 0;
}

static void remove_record(struct idw_ggtt *ggtt, uint32_t index)
{
    if (index + 1 < ggtt->count)
        memmove(&ggtt->records[index], &ggtt->records[index + 1],
                (ggtt->count - index - 1) * sizeof(ggtt->records[0]));
    --ggtt->count;
}

int idw_ggtt_create(const struct idw_ggtt_config *config,
                    struct idw_ggtt **ggtt)
{
    struct idw_ggtt *created;

    if (!ggtt)
        return -EINVAL;
    *ggtt = NULL;
    if (!config || config->size != sizeof(*config) ||
        config->version != IDW_GGTT_VERSION ||
        !config->bytes || !config->max_allocations ||
        config->max_allocations > IDW_GGTT_MAX_ALLOCATIONS ||
        config->page_bytes < IDW_GGTT_PAGE_BYTES ||
        !is_power_of_two(config->page_bytes) ||
        (config->base_offset & (config->page_bytes - 1)) ||
        (config->bytes & (config->page_bytes - 1)) ||
        config->base_offset > UINT64_MAX - config->bytes)
        return -EINVAL;

    created = calloc(1, sizeof(*created));
    if (!created)
        return -ENOMEM;
    created->records = calloc(config->max_allocations,
                              sizeof(created->records[0]));
    if (!created->records) {
        free(created);
        return -ENOMEM;
    }
    created->base_offset = config->base_offset;
    created->bytes = config->bytes;
    created->page_bytes = config->page_bytes;
    created->capacity = config->max_allocations;
    *ggtt = created;
    return 0;
}

int idw_ggtt_destroy(struct idw_ggtt *ggtt)
{
    if (!ggtt)
        return -EINVAL;
    if (ggtt->count)
        return -EBUSY;
    free(ggtt->records);
    memset(ggtt, 0, sizeof(*ggtt));
    free(ggtt);
    return 0;
}

int idw_ggtt_alloc(struct idw_ggtt *ggtt, uint64_t bytes,
                   uint64_t alignment, uint64_t cookie, uint32_t flags,
                   uint64_t *offset)
{
    struct idw_ggtt_record record;
    uint64_t cursor;
    uint64_t candidate;
    uint64_t end;
    uint32_t i;
    int ret;

    if (!ggtt || !offset || !cookie || !bytes ||
        (flags & ~IDW_GGTT_ALLOC_KNOWN_FLAGS))
        return -EINVAL;
    *offset = 0;
    if ((flags & IDW_GGTT_ALLOC_PAGE_DIRECTORY) &&
        bytes != ggtt->page_bytes)
        return -EINVAL;
    if (!is_power_of_two(alignment) || alignment < ggtt->page_bytes ||
        bytes & (ggtt->page_bytes - 1) || bytes > ggtt->bytes)
        return -EINVAL;
    if (cookie_in_use(ggtt, cookie))
        return -EEXIST;

    cursor = ggtt->base_offset;
    end = ggtt->base_offset + ggtt->bytes;
    for (i = 0; i <= ggtt->count; ++i) {
        uint64_t limit = i == ggtt->count ? end : ggtt->records[i].offset;

        ret = align_up(cursor, alignment, &candidate);
        if (ret)
            return ret;
        if (candidate <= limit && bytes <= limit - candidate) {
            memset(&record, 0, sizeof(record));
            record.offset = candidate;
            record.bytes = bytes;
            record.alignment = alignment;
            record.cookie = cookie;
            record.flags = flags;
            record.pin_count = (flags & IDW_GGTT_ALLOC_PINNED) ? 1u : 0u;
            record.resident = (flags & (IDW_GGTT_ALLOC_RESIDENT |
                                              IDW_GGTT_ALLOC_PINNED)) ? 1u : 0u;
            ret = insert_record(ggtt, &record);
            if (ret)
                return ret;
            *offset = candidate;
            return 0;
        }
        if (i == ggtt->count)
            break;
        if (ggtt->records[i].offset + ggtt->records[i].bytes > cursor)
            cursor = ggtt->records[i].offset + ggtt->records[i].bytes;
    }
    return -ENOSPC;
}

int idw_ggtt_alloc_fixed(struct idw_ggtt *ggtt, uint64_t offset,
                         uint64_t bytes, uint64_t alignment, uint64_t cookie,
                         uint32_t flags)
{
    struct idw_ggtt_record record;
    int ret;

    if (!ggtt || !cookie || (flags & ~IDW_GGTT_ALLOC_KNOWN_FLAGS))
        return -EINVAL;
    if ((flags & IDW_GGTT_ALLOC_PAGE_DIRECTORY) &&
        bytes != ggtt->page_bytes)
        return -EINVAL;
    ret = validate_geometry(ggtt, offset, bytes, alignment);
    if (ret)
        return ret;
    if (offset & (alignment - 1))
        return -EINVAL;
    if (cookie_in_use(ggtt, cookie))
        return -EEXIST;

    memset(&record, 0, sizeof(record));
    record.offset = offset;
    record.bytes = bytes;
    record.alignment = alignment;
    record.cookie = cookie;
    record.flags = flags;
    record.pin_count = (flags & IDW_GGTT_ALLOC_PINNED) ? 1u : 0u;
    record.resident = (flags & (IDW_GGTT_ALLOC_RESIDENT |
                                              IDW_GGTT_ALLOC_PINNED)) ? 1u : 0u;
    return insert_record(ggtt, &record);
}

int idw_ggtt_free(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                  uint64_t cookie)
{
    struct idw_ggtt_record *record;
    uint32_t i;

    if (!ggtt || !cookie)
        return -EINVAL;
    for (i = 0; i < ggtt->count; ++i) {
        if (ggtt->records[i].offset == offset &&
            ggtt->records[i].cookie == cookie)
            break;
    }
    if (i == ggtt->count)
        return -ENOENT;
    record = &ggtt->records[i];
    if (record->bytes != bytes)
        return -EINVAL;
    if (record->pin_count)
        return -EBUSY;
    if (record->bound)
        return -EBUSY;
    remove_record(ggtt, i);
    return 0;
}

int idw_ggtt_pin(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                 uint64_t cookie)
{
    struct idw_ggtt_record *record;

    if (!ggtt || !cookie)
        return -EINVAL;
    record = find_record(ggtt, offset, cookie);
    if (!record || record->bytes != bytes)
        return -ENOENT;
    if (record->pin_count == UINT32_MAX)
        return -EOVERFLOW;
    ++record->pin_count;
    record->resident = 1;
    return 0;
}

int idw_ggtt_unpin(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                   uint64_t cookie)
{
    struct idw_ggtt_record *record;

    if (!ggtt || !cookie)
        return -EINVAL;
    record = find_record(ggtt, offset, cookie);
    if (!record || record->bytes != bytes)
        return -ENOENT;
    if (!record->pin_count)
        return -EINVAL;
    --record->pin_count;
    return 0;
}

int idw_ggtt_set_resident(struct idw_ggtt *ggtt, uint64_t offset,
                          uint64_t bytes, uint64_t cookie, bool resident)
{
    struct idw_ggtt_record *record;

    if (!ggtt || !cookie)
        return -EINVAL;
    record = find_record(ggtt, offset, cookie);
    if (!record || record->bytes != bytes)
        return -ENOENT;
    if (!resident && (record->pin_count || record->bound))
        return -EBUSY;
    record->resident = resident ? 1u : 0u;
    return 0;
}

int idw_ggtt_bind(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                  uint64_t cookie, uint64_t dma_address,
                  const struct idw_device_scope *scope)
{
    struct idw_ggtt_record *record;

    if (!ggtt || !cookie || !scope || !scope->device_cookie ||
        !scope->generation || (dma_address & (ggtt->page_bytes - 1)) ||
        dma_address >= IDW_GGTT_DMA_LIMIT ||
        bytes > IDW_GGTT_DMA_LIMIT - dma_address)
        return -EINVAL;
    record = find_record(ggtt, offset, cookie);
    if (!record || record->bytes != bytes)
        return -ENOENT;
    if (record->bound)
        return -EEXIST;
    record->dma_address = dma_address;
    record->scope = *scope;
    record->bound = 1;
    record->resident = 1;
    return 0;
}

int idw_ggtt_unbind(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                    uint64_t cookie)
{
    struct idw_ggtt_record *record;

    if (!ggtt || !cookie)
        return -EINVAL;
    record = find_record(ggtt, offset, cookie);
    if (!record || record->bytes != bytes)
        return -ENOENT;
    if (!record->bound)
        return -ENOENT;
    if (record->pin_count)
        return -EBUSY;
    record->dma_address = 0;
    record->bound = 0;
    memset(&record->scope, 0, sizeof(record->scope));
    return 0;
}

int idw_ggtt_query(const struct idw_ggtt *ggtt, uint64_t offset,
                   uint64_t cookie,
                   struct idw_ggtt_allocation_info *info)
{
    const struct idw_ggtt_record *record;

    if (!ggtt || !info || info->size != sizeof(*info) || !cookie)
        return -EINVAL;
    record = find_record_const(ggtt, offset, cookie);
    if (!record)
        return -ENOENT;
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->version = IDW_GGTT_VERSION;
    info->offset = record->offset;
    info->bytes = record->bytes;
    info->alignment = record->alignment;
    info->cookie = record->cookie;
    info->dma_address = record->dma_address;
    info->flags = record->flags;
    info->pin_count = record->pin_count;
    info->resident = record->resident;
    info->bound = record->bound;
    info->scope = record->scope;
    return 0;
}

uint32_t idw_ggtt_count(const struct idw_ggtt *ggtt)
{
    return ggtt ? ggtt->count : 0;
}

uint64_t idw_ggtt_free_bytes(const struct idw_ggtt *ggtt)
{
    uint64_t used = 0;
    uint32_t i;

    if (!ggtt)
        return 0;
    for (i = 0; i < ggtt->count; ++i)
        used += ggtt->records[i].bytes;
    return used > ggtt->bytes ? 0 : ggtt->bytes - used;
}

int idw_ggtt_validate_range(const struct idw_ggtt *ggtt, uint64_t offset,
                            uint64_t bytes)
{
    uint64_t end;

    if (!ggtt || !bytes || offset < ggtt->base_offset ||
        bytes > ggtt->bytes || offset - ggtt->base_offset > ggtt->bytes - bytes)
        return -ERANGE;
    end = offset + bytes;
    return end < offset ? -EOVERFLOW : 0;
}
