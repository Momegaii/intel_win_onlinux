// SPDX-License-Identifier: GPL-2.0
/*
 * Device-scoped DMA page provider for an IDW-owned PCI device.
 * This module does not bind hardware or bypass the current driver.
 */
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "../include/idw/dma_memory.h"

static atomic64_t idw_next_device_cookie = ATOMIC64_INIT(0);

static bool idw_dma_device_owned(struct device *dev, struct module *owner)
{
	return dev && dev_is_pci(dev) && dev->driver &&
	       dev->driver->owner == owner;
}

int idw_dma_provider_init(struct idw_dma_provider *provider,
			  struct device *dev, struct module *owner,
			  u64 generation)
{
	u64 cookie;

	if (!provider || !dev || !generation)
		return -EINVAL;
	if (provider->active)
		return -EBUSY;
	if (!idw_dma_device_owned(dev, owner))
		return -EPERM;
	if (dma_get_mask(dev) < IDW_DMA_ADDRESS_MASK ||
	    dev->coherent_dma_mask < IDW_DMA_ADDRESS_MASK)
		return -EIO;
	if (owner && !try_module_get(owner))
		return -ENODEV;

	cookie = atomic64_inc_return(&idw_next_device_cookie);
	if ((s64)cookie <= 0) {
		if (owner)
			module_put(owner);
		return -EOVERFLOW;
	}

	memset(provider, 0, sizeof(*provider));
	provider->dev = get_device(dev);
	if (!provider->dev) {
		if (owner)
			module_put(owner);
		return -ENODEV;
	}
	provider->owner = owner;
	provider->device_cookie = cookie;
	provider->generation = generation;
	provider->next_cookie = 1;
	mutex_init(&provider->lock);
	INIT_LIST_HEAD(&provider->pages);
	provider->active = true;
	return 0;
}
EXPORT_SYMBOL_GPL(idw_dma_provider_init);

int idw_dma_provider_fini(struct idw_dma_provider *provider)
{
	struct idw_dma_page *page, *next;
	struct device *dev;
	struct module *owner;

	if (!provider)
		return -EINVAL;

	mutex_lock(&provider->lock);
	if (!provider->active) {
		mutex_unlock(&provider->lock);
		return -EINVAL;
	}
	provider->active = false;
	dev = provider->dev;
	owner = provider->owner;
	list_for_each_entry_safe(page, next, &provider->pages, link) {
		list_del(&page->link);
		dma_free_coherent(dev, PAGE_SIZE, page->cpu_address,
				  page->dma_address);
		kfree(page);
	}
	provider->dev = NULL;
	provider->owner = NULL;
	mutex_unlock(&provider->lock);

	put_device(dev);
	if (owner)
		module_put(owner);
	return 0;
}
EXPORT_SYMBOL_GPL(idw_dma_provider_fini);

int idw_dma_page_alloc(struct idw_dma_provider *provider,
		       struct idw_dma_page_info *info)
{
	struct idw_dma_page *page;
	void *cpu_address;
	dma_addr_t dma_address;
	int ret = 0;

	if (!provider || !info)
		return -EINVAL;
	page = kzalloc(sizeof(*page), GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	mutex_lock(&provider->lock);
	if (!provider->active || !idw_dma_device_owned(provider->dev,
							provider->owner)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!provider->next_cookie) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	cpu_address = dma_alloc_coherent(provider->dev, PAGE_SIZE,
					 &dma_address, GFP_KERNEL | __GFP_ZERO);
	if (!cpu_address) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	if (!IS_ALIGNED((unsigned long)cpu_address, PAGE_SIZE) ||
	    !IS_ALIGNED((u64)dma_address, PAGE_SIZE) ||
	    (u64)dma_address > IDW_DMA_ADDRESS_MASK - (PAGE_SIZE - 1)) {
		dma_free_coherent(provider->dev, PAGE_SIZE, cpu_address,
				  dma_address);
		ret = -ERANGE;
		goto out_unlock;
	}

	INIT_LIST_HEAD(&page->link);
	page->cpu_address = cpu_address;
	page->dma_address = dma_address;
	page->cookie = provider->next_cookie++;
	list_add_tail(&page->link, &provider->pages);
	info->cpu_address = cpu_address;
	info->dma_address = dma_address;
	info->bytes = PAGE_SIZE;
	info->device_cookie = provider->device_cookie;
	info->generation = provider->generation;
	info->allocation_cookie = page->cookie;
	mutex_unlock(&provider->lock);
	return 0;

out_unlock:
	mutex_unlock(&provider->lock);
	kfree(page);
	return ret;
}
EXPORT_SYMBOL_GPL(idw_dma_page_alloc);

int idw_dma_page_free(struct idw_dma_provider *provider,
		      const struct idw_dma_page_info *info)
{
	struct idw_dma_page *page;
	int ret = -ENOENT;

	if (!provider || !info)
		return -EINVAL;

	mutex_lock(&provider->lock);
	if (!provider->active) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (info->bytes != PAGE_SIZE ||
	    info->device_cookie != provider->device_cookie ||
	    info->generation != provider->generation) {
		ret = -EXDEV;
		goto out_unlock;
	}

	list_for_each_entry(page, &provider->pages, link) {
		if (page->cookie != info->allocation_cookie)
			continue;
		if (page->cpu_address != info->cpu_address ||
		    page->dma_address != info->dma_address) {
			ret = -EINVAL;
			goto out_unlock;
		}
		list_del(&page->link);
		dma_free_coherent(provider->dev, PAGE_SIZE, page->cpu_address,
				  page->dma_address);
		kfree(page);
		ret = 0;
		break;
	}

out_unlock:
	mutex_unlock(&provider->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(idw_dma_page_free);
