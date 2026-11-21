/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDW_DMA_MEMORY_H
#define IDW_DMA_MEMORY_H

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include "contracts/page_table_memory.h"

#define IDW_DMA_ADDRESS_BITS 40
#define IDW_DMA_ADDRESS_MASK DMA_BIT_MASK(IDW_DMA_ADDRESS_BITS)

/*
 * Haswell Gen7.5 resource pages are addressed by PPGTT PTEs limited to 39
 * bits. Page-directory/page-table storage may use the full 40-bit DMA mask.
 * Resource backing is therefore checked against a stricter mask.
 */
#define IDW_DMA_RESOURCE_ADDRESS_BITS 39
#define IDW_DMA_RESOURCE_ADDRESS_MASK DMA_BIT_MASK(IDW_DMA_RESOURCE_ADDRESS_BITS)
#define IDW_DMA_RESOURCE_MAX_BYTES (64ULL * 1024 * 1024)
#define IDW_DMA_RESOURCE_MAX_PAGES (IDW_DMA_RESOURCE_MAX_BYTES / PAGE_SIZE)

struct idw_dma_provider;

struct idw_dma_provider_page {
	struct list_head link;
	struct idw_dma_provider *provider;
	void *cpu_address;
	dma_addr_t dma_address;
	u64 cookie;
	atomic_t mappings;
};

struct idw_dma_page_info {
	void *cpu_address;
	dma_addr_t dma_address;
	u64 bytes;
	u64 device_cookie;
	u64 generation;
	u64 allocation_cookie;
};

struct idw_dma_resource_page {
	struct page *page;
	void *cpu_address;
	dma_addr_t dma_address;
	u64 mapping_cookie;
	bool unmapped;
	bool freed;
};

/*
 * Resource backing uses individual pages plus per-page DMA mappings. It does
 * not require one physically contiguous coherent allocation.
 */
struct idw_dma_resource {
	struct list_head link;
	struct idw_dma_provider *provider;
	u64 cookie;
	u64 bytes;
	u64 device_cookie;
	u64 generation;
	u32 page_count;
	atomic_t mappings;
	enum dma_data_direction direction;
	struct idw_dma_resource_page *pages;
	bool dma_unmapped;
	bool pages_released;
};

/*
 * The provider storage must be zero-initialized. The owning PCI driver's
 * probe configures both DMA masks before init; init runs from probe and fini
 * from remove after hardware quiescence. The PCI core keeps the bound driver
 * module alive through remove; the driver serializes provider use against
 * teardown. The provider intentionally does not pin the owner module because
 * a self-reference would prevent the driver's remove callback from running.
 * Fini refuses to release anything while a resource, page, or VMA reference
 * remains; the caller must retain or quarantine the provider in that case.
 */
struct idw_dma_provider {
	struct device *dev;
	struct module *owner;
	struct mutex lock;
	struct list_head pages;
	struct list_head resources;
	u64 device_cookie;
	u64 generation;
	u64 next_cookie;
	u64 next_mapping_cookie;
	u64 resource_pages;
	u64 mapped_pages;
	u64 page_table_pages;
	u64 quarantined_pages;
	atomic_t vma_refs;
	bool active;
	bool removal_blocked;
	s32 fail_alloc_at;
	s32 fail_map_at;
	bool fail_unmap;
	bool fail_free;
	bool fail_free_pt;
};

struct idw_dma_provider_stats {
	u64 resources;
	u64 resource_pages;
	u64 mapped_pages;
	u64 page_table_pages;
	u64 vma_refs;
	u64 quarantined_pages;
	bool active;
	bool removal_blocked;
};

int idw_dma_provider_init(struct idw_dma_provider *provider,
			  struct device *dev, struct module *owner,
			  u64 generation);
int idw_dma_provider_fini(struct idw_dma_provider *provider);
int idw_dma_provider_stats(struct idw_dma_provider *provider,
			   struct idw_dma_provider_stats *stats);
int idw_dma_page_alloc(struct idw_dma_provider *provider,
		       struct idw_dma_page_info *info);
int idw_dma_page_free(struct idw_dma_provider *provider,
		      const struct idw_dma_page_info *info);

int idw_dma_resource_alloc(struct idw_dma_provider *provider, u64 bytes,
			   enum dma_data_direction direction,
			   struct idw_dma_resource *resource);
int idw_dma_resource_segments(const struct idw_dma_resource *resource,
			      u64 offset, u64 bytes,
			      struct idw_dma_segment *segments,
			      u32 segment_capacity, u32 *segment_count);
int idw_dma_resource_sync(struct idw_dma_provider *provider,
			  const struct idw_dma_resource *resource,
			  u64 offset, u64 bytes,
			  enum dma_data_direction direction);
int idw_dma_resource_free(struct idw_dma_provider *provider,
			  struct idw_dma_resource *resource);

#endif
