// SPDX-License-Identifier: GPL-2.0
/*
 * QEMU EDU PCI DMA integration test for IDW's native DMA page provider.
 * This driver is intended only for the isolated QEMU guest test harness.
 */
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include "../include/idw/dma_memory.h"
#include "../include/idw/haswell_ppgtt_model.h"
#include "../include/uapi/idw_dma_test.h"
#include "idw_dma_pt_adapter.h"

#define IDW_EDU_VENDOR_ID 0x1234
#define IDW_EDU_DEVICE_ID 0x11e8
#define IDW_EDU_BAR 0
#define IDW_EDU_DMA_SRC 0x80
#define IDW_EDU_DMA_DST 0x88
#define IDW_EDU_DMA_COUNT 0x90
#define IDW_EDU_DMA_CMD 0x98
#define IDW_EDU_DMA_BUFFER 0x40000
#define IDW_EDU_DMA_RUN 0x1
#define IDW_EDU_DMA_TO_PCI 0x2
#define IDW_QEMU_DEBUG_EXIT_PORT 0xf4
#define IDW_QEMU_EXIT_FAIL 0x11
#define IDW_EDU_DMA_CYCLES 32

struct idw_edu_test {
	void __iomem *bar;
	struct idw_dma_provider provider;
	struct idw_dma_page_info teardown_page;
	struct idw_dma_pt_adapter pt_adapter;
	struct miscdevice misc;
	struct completion closed;
	atomic_t open;
	bool provider_ready;
	bool pt_adapter_ready;
	bool misc_ready;
};

static struct idw_edu_test *idw_edu_active;
static int idw_edu_test_result = -EINPROGRESS;
static int idw_edu_remove_result = -EINPROGRESS;
module_param_named(test_result, idw_edu_test_result, int, 0444);

static int idw_edu_dma_wait(struct idw_edu_test *test)
{
	u32 command;

	return readl_poll_timeout(test->bar + IDW_EDU_DMA_CMD, command,
				  !(command & IDW_EDU_DMA_RUN), 1000,
				  5 * USEC_PER_SEC);
}

static int idw_edu_dma_transfer(struct idw_edu_test *test, u64 source,
			       u64 destination, u64 bytes, u32 command)
{
	writeq(source, test->bar + IDW_EDU_DMA_SRC);
	writeq(destination, test->bar + IDW_EDU_DMA_DST);
	writeq(bytes, test->bar + IDW_EDU_DMA_COUNT);
	dma_wmb();
	iowrite32(command | IDW_EDU_DMA_RUN,
		  test->bar + IDW_EDU_DMA_CMD);
	return idw_edu_dma_wait(test);
}

static int idw_edu_check_page(const struct idw_dma_page_info *page)
{
	if (!page->cpu_address || page->bytes != PAGE_SIZE ||
	    !IS_ALIGNED((unsigned long)page->cpu_address, PAGE_SIZE) ||
	    !IS_ALIGNED((u64)page->dma_address, PAGE_SIZE) ||
	    (u64)page->dma_address >
		IDW_DMA_ADDRESS_MASK - (PAGE_SIZE - 1))
		return -ERANGE;
	return 0;
}

static void idw_edu_fill_page(struct idw_dma_page_info *page, u32 seed)
{
	u32 *words = page->cpu_address;
	unsigned int i;

	for (i = 0; i < PAGE_SIZE / sizeof(*words); i++)
		words[i] = seed ^ (0x9e3779b9U * (i + 1));
}

static int idw_edu_test_bad_descriptors(struct idw_edu_test *test,
					const struct idw_dma_page_info *page)
{
	struct idw_dma_page_info bad = *page;
	int ret;

	bad.bytes--;
	ret = idw_dma_page_free(&test->provider, &bad);
	if (ret != -EXDEV)
		return -EINVAL;

	bad = *page;
	bad.device_cookie++;
	ret = idw_dma_page_free(&test->provider, &bad);
	if (ret != -EXDEV)
		return -EINVAL;

	bad = *page;
	bad.generation++;
	ret = idw_dma_page_free(&test->provider, &bad);
	if (ret != -EXDEV)
		return -EINVAL;

	bad = *page;
	bad.dma_address = IDW_DMA_ADDRESS_MASK + 1;
	ret = idw_dma_page_free(&test->provider, &bad);
	if (ret != -EINVAL)
		return -EINVAL;

	bad = *page;
	bad.allocation_cookie = U64_MAX;
	ret = idw_dma_page_free(&test->provider, &bad);
	if (ret != -ENOENT)
		return -EINVAL;

	pr_info("IDW_QEMU_DMA: invalid scope, length, DMA address and cookie rejected\n");
	return 0;
}

static int idw_edu_test_dma_cycles(struct idw_edu_test *test)
{
	struct idw_dma_page_info source = {};
	struct idw_dma_page_info destination = {};
	unsigned int cycle;
	int ret;

	for (cycle = 0; cycle < IDW_EDU_DMA_CYCLES; cycle++) {
		ret = idw_dma_page_alloc(&test->provider, &source);
		if (ret)
			goto out;
		ret = idw_dma_page_alloc(&test->provider, &destination);
		if (ret)
			goto free_source;
		ret = idw_edu_check_page(&source);
		if (ret)
			goto free_both;
		ret = idw_edu_check_page(&destination);
		if (ret)
			goto free_both;

		idw_edu_fill_page(&source, 0x1d0f0000U ^ cycle);
		memset(destination.cpu_address, 0, PAGE_SIZE);
		if (!cycle)
			pr_info("IDW_QEMU_DMA: round-trip source=%pad destination=%pad bytes=%lu first=%08x\n",
				&source.dma_address, &destination.dma_address,
				(unsigned long)PAGE_SIZE,
				((u32 *)source.cpu_address)[0]);
		ret = idw_edu_dma_transfer(test, source.dma_address,
					   IDW_EDU_DMA_BUFFER, PAGE_SIZE, 0);
		if (ret)
			goto free_both;
		ret = idw_edu_dma_transfer(test, IDW_EDU_DMA_BUFFER,
					   destination.dma_address, PAGE_SIZE,
					   IDW_EDU_DMA_TO_PCI);
		if (ret)
			goto free_both;
		dma_rmb();
		if (memcmp(source.cpu_address, destination.cpu_address,
			   PAGE_SIZE)) {
			ret = -EIO;
			goto free_both;
		}

		if (cycle == 0) {
			ret = idw_edu_test_bad_descriptors(test, &source);
			if (ret)
				goto free_both;
		}
		ret = idw_dma_page_free(&test->provider, &source);
		if (ret)
			goto free_destination;
		ret = idw_dma_page_free(&test->provider, &destination);
		memset(&source, 0, sizeof(source));
		memset(&destination, 0, sizeof(destination));
		if (ret)
			goto out;
		continue;

free_both:
		idw_dma_page_free(&test->provider, &destination);
free_source:
		idw_dma_page_free(&test->provider, &source);
		memset(&source, 0, sizeof(source));
		memset(&destination, 0, sizeof(destination));
		goto out;
free_destination:
		idw_dma_page_free(&test->provider, &destination);
		memset(&destination, 0, sizeof(destination));
		goto out;
	}

	pr_info("IDW_QEMU_DMA: %u device DMA read/write cycles verified\n",
		IDW_EDU_DMA_CYCLES);
	return 0;

out:
	if (source.cpu_address)
		idw_dma_page_free(&test->provider, &source);
	if (destination.cpu_address)
		idw_dma_page_free(&test->provider, &destination);
	return ret;
}

static int idw_edu_test_pt_bad_descriptors(
	struct idw_edu_test *test, const struct idw_dma_page *table,
	const struct idw_pt_memory_ops *memory)
{
	struct idw_dma_page bad;
	void *context = idw_dma_pt_adapter_memory_context(&test->pt_adapter);
	int ret;

	bad = *table;
	bad.dma_address = 1ULL << 40;
	ret = idw_dma_pt_adapter_validate_page(&test->pt_adapter, &bad);
	if (ret != -ERANGE)
		return -EINVAL;

	bad = *table;
	bad.dma_address += 1;
	ret = idw_dma_pt_adapter_validate_page(&test->pt_adapter, &bad);
	if (ret != -EINVAL)
		return -EINVAL;

	bad = *table;
	bad.scope.generation++;
	ret = idw_dma_pt_adapter_validate_page(&test->pt_adapter, &bad);
	if (ret != -EXDEV)
		return -EINVAL;

	bad = *table;
	bad.scope.device_cookie++;
	ret = idw_dma_pt_adapter_validate_page(&test->pt_adapter, &bad);
	if (ret != -EXDEV)
		return -EINVAL;

	bad = *table;
	bad.bytes = 0;
	ret = idw_dma_pt_adapter_validate_page(&test->pt_adapter, &bad);
	if (ret != -EINVAL)
		return -EINVAL;

	bad = *table;
	bad.allocation_cookie = 0;
	ret = idw_dma_pt_adapter_validate_page(&test->pt_adapter, &bad);
	if (ret != -EINVAL)
		return -EINVAL;

	bad = *table;
	bad.memory_type = 0;
	ret = idw_dma_pt_adapter_validate_page(&test->pt_adapter, &bad);
	if (ret != -EINVAL)
		return -EINVAL;

	ret = memory->sync_page_table_for_device(context, table,
						 IDW_PT_PAGE_BYTES, 1);
	if (ret != -EINVAL)
		return -EINVAL;
	ret = memory->map_allocation(context, 1, 0, IDW_PT_PAGE_BYTES,
				     NULL, 0, NULL);
	if (ret != -EOPNOTSUPP)
		return -EINVAL;
	memory->unmap_allocation(context, 1, NULL, 0);

	pr_info("IDW_QEMU_DMA: page-table malformed descriptors and unsupported mapping rejected\n");
	return 0;
}

static int idw_edu_test_pt_storage(struct idw_edu_test *test)
{
	struct idw_dma_page table = {};
	struct idw_dma_page destination = {};
	struct idw_pt_memory_ops memory;
	struct idw_hsw_ppgtt_page_ops page_ops;
	struct idw_pt_backend_desc backend;
	u32 *ptes;
	u32 first;
	u32 last;
	unsigned int i;
	int ret;

	ret = idw_dma_pt_adapter_init(&test->pt_adapter, &test->provider);
	if (ret)
		return ret;
	test->pt_adapter_ready = true;

	ret = idw_dma_pt_adapter_backend(&test->pt_adapter, &backend);
	if (ret)
		return ret;
	ret = idw_pt_backend_validate(&backend,
				      IDW_PT_CAP_DMA_PAGE_TABLE |
				      IDW_PT_CAP_SYNC_FOR_DEVICE);
	if (ret)
		return ret;
	if (backend.kind != IDW_PT_BACKEND_NATIVE_DMA ||
	    !(backend.flags & IDW_PT_BACKEND_PAGE_TABLE_ONLY) ||
	    (backend.capabilities & IDW_PT_CAP_DMA_RESOURCE_MAPPING))
		return -EINVAL;

	ret = idw_dma_pt_adapter_page_ops(&test->pt_adapter, &page_ops);
	if (ret)
		return ret;
	table.size = sizeof(table);
	ret = page_ops.alloc_page_table_page(page_ops.context, &table);
	if (ret)
		return ret;

	ptes = table.cpu_address;
	for (i = 0; i < IDW_HASWELL_PPGTT_PTES_PER_TABLE; i++) {
		u64 dma = 0x2b00000000ULL +
			  (u64)i * IDW_HASWELL_PPGTT_PAGE_SIZE;
		u32 entry;

		ret = idw_hsw_ppgtt_encode_pte(dma,
					       IDW_HSW_PPGTT_PTE_CACHE_UC,
					       &entry);
		if (ret)
			goto free_table;
		ptes[i] = entry;
	}
	first = ptes[0];
	last = ptes[IDW_HASWELL_PPGTT_PTES_PER_TABLE - 1];
	if (first != 0x000002b1u || last != 0x003ff2b1u) {
		ret = -EIO;
		goto free_table;
	}
	{
		u32 cache_entry;

		ret = idw_hsw_ppgtt_encode_pte(0, 0x8, &cache_entry);
		if (ret || cache_entry != 0x00000801u) {
			ret = -EIO;
			goto free_table;
		}
	}

	idw_dma_pt_adapter_memory_ops(&test->pt_adapter, &memory);
	ret = memory.sync_page_table_for_device(
		idw_dma_pt_adapter_memory_context(&test->pt_adapter),
		&table, 0, IDW_PT_PAGE_BYTES);
	if (ret)
		goto free_table;

	destination.size = sizeof(destination);
	ret = page_ops.alloc_page_table_page(page_ops.context, &destination);
	if (ret)
		goto free_table;

	ret = idw_edu_dma_transfer(test, table.dma_address,
				   IDW_EDU_DMA_BUFFER, IDW_PT_PAGE_BYTES, 0);
	if (ret)
		goto free_destination;
	ret = idw_edu_dma_transfer(test, IDW_EDU_DMA_BUFFER,
				   destination.dma_address, IDW_PT_PAGE_BYTES,
				   IDW_EDU_DMA_TO_PCI);
	if (ret)
		goto free_destination;
	ret = idw_dma_pt_adapter_sync_for_cpu(&test->pt_adapter,
					      &destination, 0,
					      IDW_PT_PAGE_BYTES);
	if (ret)
		goto free_destination;
	if (memcmp(table.cpu_address, destination.cpu_address,
		   IDW_PT_PAGE_BYTES)) {
		ret = -EIO;
		goto free_destination;
	}
	if (((u32 *)destination.cpu_address)[0] != first ||
	    ((u32 *)destination.cpu_address)
		[IDW_HASWELL_PPGTT_PTES_PER_TABLE - 1] != last) {
		ret = -EIO;
		goto free_destination;
	}
	pr_info("IDW_QEMU_DMA: page-table DMA layout verified bytes=%u first=%08x last=%08x\n",
		IDW_PT_PAGE_BYTES, first, last);

	ret = idw_edu_test_pt_bad_descriptors(test, &table, &memory);
	if (ret)
		goto free_destination;

	ret = idw_dma_pt_adapter_release_page(&test->pt_adapter, &destination);
	if (ret)
		goto free_destination;
	memset(&destination, 0, sizeof(destination));
	ret = idw_dma_pt_adapter_release_page(&test->pt_adapter, &table);
	if (ret)
		return ret;
	memset(&table, 0, sizeof(table));
	if (idw_dma_pt_adapter_outstanding(&test->pt_adapter))
		return -EBUSY;
	pr_info("IDW_QEMU_DMA: page-table adapter cleanup verified\n");
	return 0;

free_destination:
	if (destination.cpu_address)
		(void)idw_dma_pt_adapter_release_page(&test->pt_adapter,
						      &destination);
free_table:
	if (table.cpu_address)
		(void)idw_dma_pt_adapter_release_page(&test->pt_adapter,
						      &table);
	return ret;
}


static int idw_edu_check_resource_page(
        const struct idw_dma_resource *resource, u32 index)
{
        if (!resource || index >= resource->page_count ||
            !resource->pages || !resource->pages[index].page ||
            !resource->pages[index].cpu_address ||
            !resource->pages[index].mapping_cookie)
                return -EINVAL;
        if (!IS_ALIGNED((unsigned long)resource->pages[index].cpu_address,
                        PAGE_SIZE) ||
            !IS_ALIGNED((u64)resource->pages[index].dma_address, PAGE_SIZE))
                return -EINVAL;
        if ((u64)resource->pages[index].dma_address >
            IDW_DMA_RESOURCE_ADDRESS_MASK - (PAGE_SIZE - 1))
                return -ERANGE;
        return 0;
}

static int idw_edu_test_resource_backing(struct idw_edu_test *test)
{
        struct idw_dma_resource source = {};
        struct idw_dma_resource destination = {};
        struct idw_dma_resource bad = {};
        struct idw_dma_segment segments[4] = {};
        struct idw_dma_page table = {};
        struct idw_pt_memory_ops memory;
        struct idw_hsw_ppgtt_page_ops page_ops;
        u32 count = 0;
        u32 pte;
        u32 i;
        int ret;

        ret = idw_dma_resource_alloc(&test->provider, 3 * PAGE_SIZE,
                                     DMA_BIDIRECTIONAL, &source);
        if (ret)
                return ret;
        ret = idw_dma_resource_alloc(&test->provider, 3 * PAGE_SIZE,
                                     DMA_BIDIRECTIONAL, &destination);
        if (ret)
                goto free_both;

        for (i = 0; i < 3; i++) {
                ret = idw_edu_check_resource_page(&source, i);
                if (ret)
                        goto free_both;
                ret = idw_edu_check_resource_page(&destination, i);
                if (ret)
                        goto free_both;
        }

        idw_edu_fill_page(&(struct idw_dma_page_info) {
                .cpu_address = source.pages[0].cpu_address,
                .bytes = PAGE_SIZE,
        }, 0x3c000000U);
        memset(destination.pages[0].cpu_address, 0, PAGE_SIZE);
        ret = idw_dma_resource_sync(&test->provider, &source, 0, PAGE_SIZE,
                                    DMA_TO_DEVICE);
        if (ret)
                goto free_both;
        ret = idw_dma_resource_sync(&test->provider, &destination, 0,
                                    PAGE_SIZE, DMA_TO_DEVICE);
        if (ret)
                goto free_both;
        ret = idw_edu_dma_transfer(test, source.pages[0].dma_address,
                                   IDW_EDU_DMA_BUFFER, PAGE_SIZE, 0);
        if (ret)
                goto free_both;
        ret = idw_edu_dma_transfer(test, IDW_EDU_DMA_BUFFER,
                                   destination.pages[0].dma_address, PAGE_SIZE,
                                   IDW_EDU_DMA_TO_PCI);
        if (ret)
                goto free_both;
        ret = idw_dma_resource_sync(&test->provider, &destination, 0,
                                    PAGE_SIZE, DMA_FROM_DEVICE);
        if (ret)
                goto free_both;
        if (memcmp(source.pages[0].cpu_address,
                   destination.pages[0].cpu_address, PAGE_SIZE)) {
                ret = -EIO;
                goto free_both;
        }

        ret = idw_dma_pt_adapter_page_ops(&test->pt_adapter, &page_ops);
        if (ret)
                goto free_both;
        table.size = sizeof(table);
        ret = page_ops.alloc_page_table_page(page_ops.context, &table);
        if (ret)
                goto free_both;
        for (i = 0; i < 3; i++) {
                ret = idw_hsw_ppgtt_encode_pte(source.pages[i].dma_address,
                                               IDW_HSW_PPGTT_PTE_CACHE_UC,
                                               &pte);
                if (ret)
                        goto free_table;
                ((u32 *)table.cpu_address)[i] = pte;
        }
        idw_dma_pt_adapter_memory_ops(&test->pt_adapter, &memory);
        ret = memory.sync_page_table_for_device(
                idw_dma_pt_adapter_memory_context(&test->pt_adapter),
                &table, 0, IDW_PT_PAGE_BYTES);
        if (ret)
                goto free_table;
        for (i = 0; i < 3; i++) {
                ret = idw_hsw_ppgtt_encode_pte(source.pages[i].dma_address,
                                               IDW_HSW_PPGTT_PTE_CACHE_UC,
                                               &pte);
                if (ret || ((u32 *)table.cpu_address)[i] != pte) {
                        ret = -EIO;
                        goto free_table;
                }
        }

        ret = idw_dma_resource_segments(&source, 0, 3 * PAGE_SIZE,
                                        segments, 4, &count);
        if (ret || count != 3)
                goto free_table;
        for (i = 0; i < 3; i++) {
                if (segments[i].dma_address != source.pages[i].dma_address ||
                    segments[i].bytes != PAGE_SIZE ||
                    segments[i].mapping_cookie !=
                        source.pages[i].mapping_cookie) {
                        ret = -EIO;
                        goto free_table;
                }
        }

        ret = idw_dma_resource_alloc(&test->provider, 0,
                                     DMA_BIDIRECTIONAL, &bad);
        if (ret != -E2BIG)
                goto bad_descriptor;
        ret = idw_dma_resource_alloc(&test->provider,
                                     IDW_DMA_RESOURCE_MAX_BYTES + PAGE_SIZE,
                                     DMA_BIDIRECTIONAL, &bad);
        if (ret != -E2BIG)
                goto bad_descriptor;
        ret = idw_dma_resource_segments(&source, 1, PAGE_SIZE, segments, 4,
                                        &count);
        if (ret != -EINVAL)
                goto bad_descriptor;
        ret = idw_dma_resource_segments(&source, 0, 4 * PAGE_SIZE, segments,
                                        4, &count);
        if (ret != -ERANGE)
                goto bad_descriptor;
        ret = idw_dma_resource_segments(&source, 0, 3 * PAGE_SIZE, segments,
                                        2, &count);
        if (ret != -ENOSPC || count != 3)
                goto bad_descriptor;
        bad = source;
        bad.device_cookie++;
        ret = idw_dma_resource_free(&test->provider, &bad);
        if (ret != -EXDEV)
                goto bad_descriptor;

        ret = idw_dma_pt_adapter_release_page(&test->pt_adapter, &table);
        if (ret)
                goto free_both;
        memset(&table, 0, sizeof(table));
        ret = idw_dma_resource_free(&test->provider, &source);
        if (ret)
                goto free_both;
        ret = idw_dma_resource_free(&test->provider, &destination);
        if (ret)
                goto out;
        if (!list_empty(&test->provider.resources))
                return -EBUSY;
        pr_info("IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified\n");
        return 0;

bad_descriptor:
        ret = -EINVAL;
free_table:
        if (table.cpu_address)
                (void)idw_dma_pt_adapter_release_page(&test->pt_adapter,
                                                      &table);
free_both:
        if (destination.pages)
                (void)idw_dma_resource_free(&test->provider, &destination);
        if (source.pages)
                (void)idw_dma_resource_free(&test->provider, &source);
out:
        return ret;
}


static struct idw_dma_resource *
idw_edu_find_resource(struct idw_edu_test *test, u64 cookie)
{
        struct idw_dma_resource *resource;

        list_for_each_entry(resource, &test->provider.resources, link) {
                if (resource->cookie == cookie)
                        return resource;
        }
        return NULL;
}

static struct idw_dma_provider_page *
idw_edu_find_page(struct idw_edu_test *test, u64 cookie)
{
        struct idw_dma_provider_page *page;

        list_for_each_entry(page, &test->provider.pages, link) {
                if (page->cookie == cookie)
                        return page;
        }
        return NULL;
}

static enum dma_data_direction idw_edu_test_direction(u32 direction)
{
        switch (direction) {
        case IDW_DMA_TEST_BIDIRECTIONAL:
                return DMA_BIDIRECTIONAL;
        case IDW_DMA_TEST_TO_DEVICE:
                return DMA_TO_DEVICE;
        case IDW_DMA_TEST_FROM_DEVICE:
                return DMA_FROM_DEVICE;
        default:
                return DMA_NONE;
        }
}

static void idw_edu_resource_vm_close(struct vm_area_struct *vma)
{
        struct idw_dma_resource *resource = vma->vm_private_data;

        if (resource && resource->mappings)
                --resource->mappings;
}

static const struct vm_operations_struct idw_edu_resource_vm_ops = {
        .close = idw_edu_resource_vm_close,
};

static int idw_edu_open(struct inode *inode, struct file *file)
{
        struct idw_edu_test *test = READ_ONCE(idw_edu_active);

        if (!test)
                return -ENODEV;
        if (atomic_cmpxchg(&test->open, 0, 1))
                return -EBUSY;
        reinit_completion(&test->closed);
        file->private_data = test;
        return 0;
}

static int idw_edu_release(struct inode *inode, struct file *file)
{
        struct idw_edu_test *test = file->private_data;

        if (!test)
                return 0;
        atomic_set(&test->open, 0);
        complete(&test->closed);
        return 0;
}

static int idw_edu_mmap(struct file *file, struct vm_area_struct *vma)
{
        struct idw_edu_test *test = file->private_data;
        unsigned long pgoff = vma->vm_pgoff;
        u64 cookie;
        u32 i;

        if (!test || !test->provider_ready)
                return -ENODEV;

        if (pgoff >= IDW_DMA_TEST_PT_PGOFF_BASE) {
                struct idw_dma_provider_page *page;

                cookie = pgoff - IDW_DMA_TEST_PT_PGOFF_BASE;
                page = idw_edu_find_page(test, cookie);
                if (!page)
                        return -ENOENT;
                if (vma->vm_end - vma->vm_start != PAGE_SIZE)
                        return -EINVAL;
                vma->vm_pgoff = 0;
                return dma_mmap_coherent(test->provider.dev, vma,
                                         page->cpu_address,
                                         page->dma_address, PAGE_SIZE);
        }

        cookie = pgoff;
        {
                struct idw_dma_resource *resource =
                        idw_edu_find_resource(test, cookie);

                if (!resource)
                        return -ENOENT;
                if (vma->vm_end - vma->vm_start != resource->bytes)
                        return -EINVAL;
                if (resource->mappings)
                        return -EBUSY;
                vma->vm_pgoff = 0;
                vma->vm_ops = &idw_edu_resource_vm_ops;
                vma->vm_private_data = resource;
                vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
                for (i = 0; i < resource->page_count; i++) {
                        if (vm_insert_page(vma,
                                           vma->vm_start + (unsigned long)i *
                                                   PAGE_SIZE,
                                           resource->pages[i].page))
                                return -EFAULT;
                }
                resource->mappings = 1;
        }
        return 0;
}

static long idw_edu_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg)
{
        struct idw_edu_test *test = file->private_data;
        void __user *user = (void __user *)arg;

        if (!test || !test->provider_ready)
                return -ENODEV;

        switch (cmd) {
        case IDW_DMA_TEST_IOCTL_ALLOC: {
                struct idw_dma_test_alloc request;
                struct idw_dma_resource *resource;
                enum dma_data_direction direction;
                int ret;

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request))
                        return -EINVAL;
                direction = idw_edu_test_direction(request.direction);
                if (direction == DMA_NONE)
                        return -EINVAL;
                resource = kzalloc(sizeof(*resource), GFP_KERNEL);
                if (!resource)
                        return -ENOMEM;
                ret = idw_dma_resource_alloc(&test->provider, request.bytes,
                                             direction, resource);
                if (ret) {
                        kfree(resource);
                        return ret;
                }
                request.cookie = resource->cookie;
                request.device_cookie = resource->device_cookie;
                request.generation = resource->generation;
                request.page_count = resource->page_count;
                if (copy_to_user(user, &request, sizeof(request))) {
                        (void)idw_dma_resource_free(&test->provider, resource);
                        kfree(resource);
                        return -EFAULT;
                }
                return 0;
        }
        case IDW_DMA_TEST_IOCTL_FREE: {
                struct idw_dma_test_free request;
                struct idw_dma_resource *resource;
                int ret;

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request))
                        return -EINVAL;
                resource = idw_edu_find_resource(test, request.cookie);
                if (!resource)
                        return -ENOENT;
                ret = idw_dma_resource_free(&test->provider, resource);
                if (!ret)
                        kfree(resource);
                return ret;
        }
        case IDW_DMA_TEST_IOCTL_SEGMENTS: {
                struct idw_dma_test_segments request;
                struct idw_dma_test_segment *user_segments;
                struct idw_dma_segment *segments;
                struct idw_dma_resource *resource;
                u32 count = 0;
                u32 i;
                int ret;

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request) ||
                    request.capacity > IDW_DMA_TEST_MAX_SEGMENTS)
                        return -EINVAL;
                resource = idw_edu_find_resource(test, request.cookie);
                if (!resource)
                        return -ENOENT;
                user_segments = kcalloc(IDW_DMA_TEST_MAX_SEGMENTS,
                                        sizeof(*user_segments), GFP_KERNEL);
                segments = kcalloc(IDW_DMA_TEST_MAX_SEGMENTS,
                                   sizeof(*segments), GFP_KERNEL);
                if (!user_segments || !segments) {
                        kfree(user_segments);
                        kfree(segments);
                        return -ENOMEM;
                }
                ret = idw_dma_resource_segments(resource, request.offset,
                                                request.bytes, segments,
                                                request.capacity, &count);
                if (ret && ret != -ENOSPC) {
                        kfree(user_segments);
                        kfree(segments);
                        return ret;
                }
                if (count > request.capacity) {
                        request.count = count;
                        if (copy_to_user(user, &request, sizeof(request)))
                                ret = -EFAULT;
                        else
                                ret = -ENOSPC;
                        kfree(user_segments);
                        kfree(segments);
                        return ret;
                }
                for (i = 0; i < count; i++) {
                        user_segments[i].dma_address =
                                segments[i].dma_address;
                        user_segments[i].bytes = segments[i].bytes;
                        user_segments[i].device_cookie =
                                segments[i].scope.device_cookie;
                        user_segments[i].generation =
                                segments[i].scope.generation;
                        user_segments[i].mapping_cookie =
                                segments[i].mapping_cookie;
                }
                if (count && copy_to_user((void __user *)(uintptr_t)
                                                  request.segments,
                                          user_segments,
                                          count * sizeof(*user_segments)))
                        ret = -EFAULT;
                else {
                        request.count = count;
                        if (copy_to_user(user, &request, sizeof(request)))
                                ret = -EFAULT;
                }
                kfree(user_segments);
                kfree(segments);
                return ret;
        }
        case IDW_DMA_TEST_IOCTL_ALLOC_PT: {
                struct idw_dma_test_pt request;
                struct idw_dma_page_info info = {};
                int ret;

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request))
                        return -EINVAL;
                ret = idw_dma_page_alloc(&test->provider, &info);
                if (ret)
                        return ret;
                request.cookie = info.allocation_cookie;
                request.dma_address = info.dma_address;
                request.device_cookie = info.device_cookie;
                request.generation = info.generation;
                request.bytes = info.bytes;
                if (copy_to_user(user, &request, sizeof(request))) {
                        (void)idw_dma_page_free(&test->provider, &info);
                        return -EFAULT;
                }
                return 0;
        }
        case IDW_DMA_TEST_IOCTL_FREE_PT: {
                struct idw_dma_test_pt request;
                struct idw_dma_provider_page *page;
                struct idw_dma_page_info info = {};

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request))
                        return -EINVAL;
                page = idw_edu_find_page(test, request.cookie);
                if (!page)
                        return -ENOENT;
                info.cpu_address = page->cpu_address;
                info.dma_address = page->dma_address;
                info.bytes = PAGE_SIZE;
                info.device_cookie = test->provider.device_cookie;
                info.generation = test->provider.generation;
                info.allocation_cookie = page->cookie;
                return idw_dma_page_free(&test->provider, &info);
        }
        case IDW_DMA_TEST_IOCTL_SYNC: {
                struct idw_dma_test_sync request;
                struct idw_dma_resource *resource;
                enum dma_data_direction direction;

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request))
                        return -EINVAL;
                direction = idw_edu_test_direction(request.direction);
                if (direction == DMA_NONE)
                        return -EINVAL;
                resource = idw_edu_find_resource(test, request.cookie);
                if (!resource)
                        return -ENOENT;
                return idw_dma_resource_sync(&test->provider, resource,
                                             request.offset, request.bytes,
                                             direction);
        }
        case IDW_DMA_TEST_IOCTL_SYNC_PT: {
                struct idw_dma_test_pt request;
                struct idw_dma_provider_page *page;

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request))
                        return -EINVAL;
                page = idw_edu_find_page(test, request.cookie);
                if (!page)
                        return -ENOENT;
                dma_sync_single_for_device(test->provider.dev,
                                           page->dma_address, PAGE_SIZE,
                                           DMA_BIDIRECTIONAL);
                return 0;
        }
        default:
                return -ENOTTY;
        }
}

static const struct file_operations idw_edu_fops = {
        .owner = THIS_MODULE,
        .open = idw_edu_open,
        .release = idw_edu_release,
        .unlocked_ioctl = idw_edu_ioctl,
        .mmap = idw_edu_mmap,
        .llseek = noop_llseek,
};


static int idw_edu_probe(struct pci_dev *pdev,
			 const struct pci_device_id *id)
{
	struct idw_edu_test *test;
	struct idw_dma_provider rejected = {};
	struct idw_dma_provider low_mask = {};
	int ret;

	(void)id;
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto fail_state;
	ret = pci_request_regions(pdev, "idw-edu-dma-test");
	if (ret)
		goto fail_disable;
	test->bar = pci_iomap(pdev, IDW_EDU_BAR, 0);
	if (!test->bar) {
		ret = -ENOMEM;
		goto fail_regions;
	}

	ret = idw_dma_provider_init(&rejected, &pdev->dev, NULL, 1);
	if (ret != -EPERM) {
		ret = -EINVAL;
		goto fail_iomap;
	}
	pr_info("IDW_QEMU_DMA: rejected provider owner mismatch\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto fail_iomap;
	ret = idw_dma_provider_init(&low_mask, &pdev->dev, THIS_MODULE, 2);
	if (ret != -EIO) {
		if (!ret)
			idw_dma_provider_fini(&low_mask);
		ret = -EINVAL;
		goto fail_iomap;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, IDW_DMA_ADDRESS_MASK);
	if (ret)
		goto fail_iomap;
	pr_info("IDW_QEMU_DMA: rejected 32-bit mask; accepted configured 40-bit mask\n");

	pci_set_master(pdev);
	ret = idw_dma_provider_init(&test->provider, &pdev->dev,
				    THIS_MODULE, 1);
	if (ret)
		goto fail_master;
	test->provider_ready = true;

	ret = idw_edu_test_pt_storage(test);
	if (ret)
		goto fail_provider;

	ret = idw_edu_test_dma_cycles(test);
	if (ret)
		goto fail_provider;

	ret = idw_edu_test_resource_backing(test);
	if (ret)
		goto fail_provider;

	ret = idw_dma_page_alloc(&test->provider, &test->teardown_page);
	if (ret)
		goto fail_provider;
	ret = idw_edu_check_page(&test->teardown_page);
	if (ret)
		goto fail_provider;

	test->misc.minor = MISC_DYNAMIC_MINOR;
	test->misc.name = "idw-edu-dma";
	test->misc.fops = &idw_edu_fops;
	test->misc.parent = &pdev->dev;
	test->misc.mode = 0600;
	init_completion(&test->closed);
	atomic_set(&test->open, 0);
	ret = misc_register(&test->misc);
	if (ret)
		goto fail_provider;
	test->misc_ready = true;
	WRITE_ONCE(idw_edu_active, test);

	pci_set_drvdata(pdev, test);
	idw_edu_remove_result = -EINPROGRESS;
	idw_edu_test_result = 0;
	pr_info("IDW_QEMU_DMA: probe passed; retained one page to verify remove cleanup\n");
	return 0;

fail_provider:
	if (test->misc_ready) {
		misc_deregister(&test->misc);
		test->misc_ready = false;
	}
	if (test->pt_adapter_ready) {
		int adapter_ret = idw_dma_pt_adapter_fini(&test->pt_adapter);

		if (adapter_ret)
			pr_warn("IDW_QEMU_DMA: page-table adapter cleanup returned %d\n",
				adapter_ret);
		test->pt_adapter_ready = false;
	}
	if (test->provider_ready) {
		idw_dma_provider_fini(&test->provider);
		test->provider_ready = false;
	}
fail_master:
	pci_clear_master(pdev);
fail_iomap:
	if (test->bar)
		pci_iounmap(pdev, test->bar);
fail_regions:
	pci_release_regions(pdev);
fail_disable:
	pci_disable_device(pdev);
fail_state:
	kfree(test);
	idw_edu_test_result = ret;
	pr_err("IDW_QEMU_DMA: probe test failed: %d\n", ret);
	outb(IDW_QEMU_EXIT_FAIL, IDW_QEMU_DEBUG_EXIT_PORT);
	return ret;
}

static void idw_edu_remove(struct pci_dev *pdev)
{
	struct idw_edu_test *test = pci_get_drvdata(pdev);
	bool outstanding;
	int adapter_ret = 0;
	int ret;

	if (!test)
		return;
	if (test->misc_ready) {
		misc_deregister(&test->misc);
		WRITE_ONCE(idw_edu_active, NULL);
		if (atomic_read(&test->open))
			wait_for_completion(&test->closed);
		test->misc_ready = false;
	}
	if (test->pt_adapter_ready) {
		adapter_ret = idw_dma_pt_adapter_fini(&test->pt_adapter);
		test->pt_adapter_ready = false;
	}
	outstanding = !list_empty(&test->provider.pages) ||
		      !list_empty(&test->provider.resources);
	ret = idw_dma_provider_fini(&test->provider);
	test->provider_ready = false;
	pci_clear_master(pdev);
	pci_iounmap(pdev, test->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(test);
	if (adapter_ret)
		idw_edu_remove_result = adapter_ret;
	else
		idw_edu_remove_result =
			outstanding && !ret ? 0 : (ret ? ret : -EINVAL);
	pr_info("IDW_QEMU_DMA: remove freed outstanding provider allocations=%s result=%d\n",
		outstanding ? "yes" : "no", idw_edu_remove_result);
}

static const struct pci_device_id idw_edu_ids[] = {
	{ PCI_DEVICE(IDW_EDU_VENDOR_ID, IDW_EDU_DEVICE_ID) },
	{}
};
MODULE_DEVICE_TABLE(pci, idw_edu_ids);

static struct pci_driver idw_edu_driver = {
	.name = "idw-edu-dma-test",
	.id_table = idw_edu_ids,
	.probe = idw_edu_probe,
	.remove = idw_edu_remove,
};

static int __init idw_edu_init(void)
{
	int ret;

	idw_edu_test_result = -EINPROGRESS;
	idw_edu_remove_result = -EINPROGRESS;
	ret = pci_register_driver(&idw_edu_driver);
	if (ret)
		return ret;
	if (idw_edu_test_result == -EINPROGRESS) {
		pci_unregister_driver(&idw_edu_driver);
		return -ENODEV;
	}
	return 0;
}

static void __exit idw_edu_exit(void)
{
	pci_unregister_driver(&idw_edu_driver);
	if (idw_edu_test_result == 0 && idw_edu_remove_result == 0) {
		pr_info("IDW_QEMU_DMA: driver removal completed successfully\n");
	} else {
		pr_err("IDW_QEMU_DMA: FAIL: probe_result=%d remove_result=%d\n",
		       idw_edu_test_result, idw_edu_remove_result);
		outb(IDW_QEMU_EXIT_FAIL, IDW_QEMU_DEBUG_EXIT_PORT);
	}
}

module_init(idw_edu_init);
module_exit(idw_edu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IDW DMA provider test using QEMU EDU PCI DMA");
