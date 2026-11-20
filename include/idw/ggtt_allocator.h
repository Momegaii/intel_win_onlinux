/* SPDX-License-Identifier: MIT */
#ifndef IDW_GGTT_ALLOCATOR_H
#define IDW_GGTT_ALLOCATOR_H

#include "contracts/base.h"
#include "contracts/page_table_memory.h"

#include <stdbool.h>

#define IDW_GGTT_VERSION 1u
#define IDW_GGTT_PAGE_BYTES 4096u
#define IDW_GGTT_RECOMMENDED_ALIGN (64u * 1024u)
#define IDW_GGTT_MAX_ALLOCATIONS 256u

enum idw_ggtt_alloc_flags {
    IDW_GGTT_ALLOC_RESIDENT = 1u << 0,
    IDW_GGTT_ALLOC_PINNED = 1u << 1,
    IDW_GGTT_ALLOC_PAGE_DIRECTORY = 1u << 2
};

#define IDW_GGTT_ALLOC_KNOWN_FLAGS \
    (IDW_GGTT_ALLOC_RESIDENT | IDW_GGTT_ALLOC_PINNED | \
     IDW_GGTT_ALLOC_PAGE_DIRECTORY)

struct idw_ggtt_config {
    uint32_t size;
    uint32_t version;
    uint64_t base_offset;
    uint64_t bytes;
    uint32_t page_bytes;
    uint32_t max_allocations;
};

struct idw_ggtt_allocation_info {
    uint32_t size;
    uint32_t version;
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

struct idw_ggtt;

/*
 * A checked first-fit GGTT interval allocator. The caller supplies the
 * aperture base/size from the owned device; this module does not assume a
 * Haswell aperture size and does not touch hardware.
 *
 * Records are kept sorted by offset. Cookies must be nonzero and unique while
 * the allocation is live. Freeing requires the exact offset, byte count, and
 * cookie; callers must not reuse a cookie while a stale free could still
 * arrive.
 */
int idw_ggtt_create(const struct idw_ggtt_config *config,
                    struct idw_ggtt **ggtt);
int idw_ggtt_destroy(struct idw_ggtt *ggtt);

int idw_ggtt_alloc(struct idw_ggtt *ggtt, uint64_t bytes,
                   uint64_t alignment, uint64_t cookie, uint32_t flags,
                   uint64_t *offset);
int idw_ggtt_alloc_fixed(struct idw_ggtt *ggtt, uint64_t offset,
                         uint64_t bytes, uint64_t alignment, uint64_t cookie,
                         uint32_t flags);
int idw_ggtt_free(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                  uint64_t cookie);

int idw_ggtt_pin(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                 uint64_t cookie);
int idw_ggtt_unpin(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                   uint64_t cookie);
int idw_ggtt_set_resident(struct idw_ggtt *ggtt, uint64_t offset,
                          uint64_t bytes, uint64_t cookie, bool resident);

int idw_ggtt_bind(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                  uint64_t cookie, uint64_t dma_address,
                  const struct idw_device_scope *scope);
int idw_ggtt_unbind(struct idw_ggtt *ggtt, uint64_t offset, uint64_t bytes,
                    uint64_t cookie);

int idw_ggtt_query(const struct idw_ggtt *ggtt, uint64_t offset,
                   uint64_t cookie,
                   struct idw_ggtt_allocation_info *info);
uint32_t idw_ggtt_count(const struct idw_ggtt *ggtt);
uint64_t idw_ggtt_free_bytes(const struct idw_ggtt *ggtt);
int idw_ggtt_validate_range(const struct idw_ggtt *ggtt, uint64_t offset,
                            uint64_t bytes);

#endif
