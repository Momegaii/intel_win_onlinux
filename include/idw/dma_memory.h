/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDW_DMA_MEMORY_H
#define IDW_DMA_MEMORY_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define IDW_DMA_ADDRESS_BITS 40
#define IDW_DMA_ADDRESS_MASK DMA_BIT_MASK(IDW_DMA_ADDRESS_BITS)

struct idw_dma_provider_page {
	struct list_head link;
	void *cpu_address;
	dma_addr_t dma_address;
	u64 cookie;
};

struct idw_dma_page_info {
	void *cpu_address;
	dma_addr_t dma_address;
	u64 bytes;
	u64 device_cookie;
	u64 generation;
	u64 allocation_cookie;
};

/*
 * The provider storage must be zero-initialized. The owning PCI driver's
 * probe configures both DMA masks before init; init runs from probe and fini
 * from remove after hardware quiescence. The PCI core keeps the bound driver
 * module alive through remove; the driver serializes provider use against
 * teardown. The provider intentionally does not pin the owner module because
 * a self-reference would prevent the driver's remove callback from running.
 * Fini releases any remaining page allocations.
 */
struct idw_dma_provider {
	struct device *dev;
	struct module *owner;
	struct mutex lock;
	struct list_head pages;
	u64 device_cookie;
	u64 generation;
	u64 next_cookie;
	bool active;
};

int idw_dma_provider_init(struct idw_dma_provider *provider,
			  struct device *dev, struct module *owner,
			  u64 generation);
int idw_dma_provider_fini(struct idw_dma_provider *provider);
int idw_dma_page_alloc(struct idw_dma_provider *provider,
		       struct idw_dma_page_info *info);
int idw_dma_page_free(struct idw_dma_provider *provider,
		      const struct idw_dma_page_info *info);

#endif
