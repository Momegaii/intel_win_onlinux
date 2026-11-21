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
	cookie = atomic64_inc_return(&idw_next_device_cookie);
	if ((s64)cookie <= 0) {
		return -EOVERFLOW;
	}

	memset(provider, 0, sizeof(*provider));
	provider->dev = get_device(dev);
	if (!provider->dev)
		return -ENODEV;
	provider->owner = owner;
	provider->device_cookie = cookie;
	provider->generation = generation;
	provider->next_cookie = 1;
	provider->next_mapping_cookie = 1;
	mutex_init(&provider->lock);
	INIT_LIST_HEAD(&provider->pages);
	INIT_LIST_HEAD(&provider->resources);
	provider->active = true;
	return 0;
}
EXPORT_SYMBOL_GPL(idw_dma_provider_init);

static void idw_dma_resource_release_pages(struct idw_dma_provider *provider,
					   struct idw_dma_resource *resource)
{
	u32 i;

	if (!resource || !resource->pages)
		return;
	for (i = 0; i < resource->page_count; i++) {
		if (!resource->pages[i].page)
			continue;
		dma_unmap_page(provider->dev, resource->pages[i].dma_address,
			       PAGE_SIZE, resource->direction);
		__free_pages(resource->pages[i].page, 0);
	}
	kfree(resource->pages);
	resource->pages = NULL;
}

int idw_dma_provider_fini(struct idw_dma_provider *provider)
{
	struct idw_dma_provider_page *page, *next;
	struct idw_dma_resource *resource, *resource_next;
	struct device *dev;

	if (!provider)
		return -EINVAL;

	mutex_lock(&provider->lock);
	if (!provider->active) {
		mutex_unlock(&provider->lock);
		return -EINVAL;
	}
	provider->active = false;
	dev = provider->dev;
	list_for_each_entry_safe(page, next, &provider->pages, link) {
		list_del(&page->link);
		dma_free_coherent(dev, PAGE_SIZE, page->cpu_address,
				  page->dma_address);
		kfree(page);
	}
	list_for_each_entry_safe(resource, resource_next, &provider->resources,
				 link) {
		list_del(&resource->link);
		idw_dma_resource_release_pages(provider, resource);
		memset(resource, 0, sizeof(*resource));
	}
	provider->dev = NULL;
	provider->owner = NULL;
	mutex_unlock(&provider->lock);

	put_device(dev);
	return 0;
}
EXPORT_SYMBOL_GPL(idw_dma_provider_fini);

int idw_dma_page_alloc(struct idw_dma_provider *provider,
		       struct idw_dma_page_info *info)
{
	struct idw_dma_provider_page *page;
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
	struct idw_dma_provider_page *page;
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


int idw_dma_resource_alloc(struct idw_dma_provider *provider, u64 bytes,
			   enum dma_data_direction direction,
			   struct idw_dma_resource *resource)
{
	struct idw_dma_resource_page *pages;
	u32 page_count;
	u32 i;
	u32 j;
	int ret = 0;

	if (!provider || !resource)
		return -EINVAL;
	if (!bytes || bytes > IDW_DMA_RESOURCE_MAX_BYTES)
		return -E2BIG;
	if (direction != DMA_TO_DEVICE && direction != DMA_FROM_DEVICE &&
	    direction != DMA_BIDIRECTIONAL)
		return -EINVAL;
	page_count = DIV_ROUND_UP(bytes, PAGE_SIZE);
	if (!page_count || page_count > IDW_DMA_RESOURCE_MAX_PAGES)
		return -E2BIG;
	pages = kcalloc(page_count, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	memset(resource, 0, sizeof(*resource));

	mutex_lock(&provider->lock);
	if (!provider->active ||
	    !idw_dma_device_owned(provider->dev, provider->owner)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!provider->next_cookie || !provider->next_mapping_cookie) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	for (i = 0; i < page_count; i++) {
		struct page *page;
		void *cpu_address;
		dma_addr_t dma_address;

		page = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN);
		if (!page) {
			ret = -ENOMEM;
			goto free_pages;
		}
		cpu_address = page_address(page);
		if (!cpu_address) {
			__free_pages(page, 0);
			ret = -ENOMEM;
			goto free_pages;
		}
		dma_address = dma_map_page(provider->dev, page, 0, PAGE_SIZE,
					   direction);
		if (dma_mapping_error(provider->dev, dma_address)) {
			__free_pages(page, 0);
			ret = -EIO;
			goto free_pages;
		}
		if (!IS_ALIGNED((u64)dma_address, PAGE_SIZE) ||
		    (u64)dma_address >
			IDW_DMA_RESOURCE_ADDRESS_MASK - (PAGE_SIZE - 1)) {
			dma_unmap_page(provider->dev, dma_address, PAGE_SIZE,
				       direction);
			__free_pages(page, 0);
			ret = -ERANGE;
			goto free_pages;
		}
		pages[i].page = page;
		pages[i].cpu_address = cpu_address;
		pages[i].dma_address = dma_address;
		pages[i].mapping_cookie = provider->next_mapping_cookie++;
	}

	resource->cookie = provider->next_cookie++;
	resource->bytes = (u64)page_count * PAGE_SIZE;
	resource->device_cookie = provider->device_cookie;
	resource->generation = provider->generation;
	resource->page_count = page_count;
	resource->direction = direction;
	resource->pages = pages;
	list_add_tail(&resource->link, &provider->resources);
	mutex_unlock(&provider->lock);
	return 0;

free_pages:
	for (j = 0; j < i; j++) {
		dma_unmap_page(provider->dev, pages[j].dma_address, PAGE_SIZE,
			       direction);
		__free_pages(pages[j].page, 0);
	}
out_unlock:
	mutex_unlock(&provider->lock);
	kfree(pages);
	return ret;
}
EXPORT_SYMBOL_GPL(idw_dma_resource_alloc);

int idw_dma_resource_segments(const struct idw_dma_resource *resource,
			      u64 offset, u64 bytes,
			      struct idw_dma_segment *segments,
			      u32 segment_capacity, u32 *segment_count)
{
	u32 first;
	u32 count;
	u32 i;

	if (!resource || !segments || !segment_count || !bytes ||
	    !resource->pages || !resource->page_count)
		return -EINVAL;
	if ((offset & (PAGE_SIZE - 1)) || (bytes & (PAGE_SIZE - 1)))
		return -EINVAL;
	if (offset > resource->bytes || bytes > resource->bytes - offset)
		return -ERANGE;
	first = offset / PAGE_SIZE;
	count = bytes / PAGE_SIZE;
	if (!count)
		return -EINVAL;
	if (segment_capacity < count) {
		*segment_count = count;
		return -ENOSPC;
	}
	for (i = 0; i < count; i++) {
		segments[i].dma_address = resource->pages[first + i].dma_address;
		segments[i].bytes = PAGE_SIZE;
		segments[i].scope.device_cookie = resource->device_cookie;
		segments[i].scope.generation = resource->generation;
		segments[i].mapping_cookie =
			resource->pages[first + i].mapping_cookie;
	}
	*segment_count = count;
	return 0;
}
EXPORT_SYMBOL_GPL(idw_dma_resource_segments);

int idw_dma_resource_sync(struct idw_dma_provider *provider,
			  const struct idw_dma_resource *resource,
			  u64 offset, u64 bytes,
			  enum dma_data_direction direction)
{
	u64 remaining;
	u64 page_offset;
	u32 index;
	int ret = 0;

	if (!provider || !resource || !resource->cookie || !resource->pages)
		return -EINVAL;
	if (direction != DMA_TO_DEVICE && direction != DMA_FROM_DEVICE &&
	    direction != DMA_BIDIRECTIONAL)
		return -EINVAL;
	if (offset > resource->bytes || bytes > resource->bytes - offset)
		return -ERANGE;
	if (!bytes)
		return 0;

	mutex_lock(&provider->lock);
	if (!provider->active) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (resource->device_cookie != provider->device_cookie ||
	    resource->generation != provider->generation) {
		ret = -EXDEV;
		goto out_unlock;
	}

	index = offset / PAGE_SIZE;
	page_offset = offset % PAGE_SIZE;
	remaining = bytes;
	while (remaining) {
		u64 chunk = PAGE_SIZE - page_offset;
		dma_addr_t dma_address;

		if (chunk > remaining)
			chunk = remaining;
		dma_address = resource->pages[index].dma_address + page_offset;
		if (direction == DMA_TO_DEVICE ||
		    direction == DMA_BIDIRECTIONAL)
			dma_sync_single_for_device(provider->dev, dma_address,
						   (size_t)chunk,
						   resource->direction);
		if (direction == DMA_FROM_DEVICE ||
		    direction == DMA_BIDIRECTIONAL)
			dma_sync_single_for_cpu(provider->dev, dma_address,
						(size_t)chunk,
						resource->direction);
		remaining -= chunk;
		++index;
		page_offset = 0;
	}

out_unlock:
	mutex_unlock(&provider->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(idw_dma_resource_sync);

int idw_dma_resource_free(struct idw_dma_provider *provider,
			  struct idw_dma_resource *resource)
{
	struct idw_dma_resource *entry;
	bool found = false;
	int ret = 0;

	if (!provider || !resource)
		return -EINVAL;

	mutex_lock(&provider->lock);
	if (!provider->active) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!resource->cookie ||
	    resource->device_cookie != provider->device_cookie ||
	    resource->generation != provider->generation) {
		ret = -EXDEV;
		goto out_unlock;
	}

	list_for_each_entry(entry, &provider->resources, link) {
		if (entry->cookie == resource->cookie) {
			found = true;
			break;
		}
	}
	if (!found) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (entry != resource) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (entry->mappings) {
		ret = -EBUSY;
		goto out_unlock;
	}

	idw_dma_resource_release_pages(provider, entry);
	list_del(&entry->link);
	memset(entry, 0, sizeof(*entry));

out_unlock:
	mutex_unlock(&provider->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(idw_dma_resource_free);
