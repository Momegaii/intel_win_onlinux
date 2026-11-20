// SPDX-License-Identifier: GPL-2.0
/*
 * QEMU EDU PCI DMA integration test for IDW's native DMA page provider.
 * This driver is intended only for the isolated QEMU guest test harness.
 */
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/io.h>
#include "../include/idw/dma_memory.h"

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
	bool provider_ready;
};

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

	ret = idw_edu_test_dma_cycles(test);
	if (ret)
		goto fail_provider;

	ret = idw_dma_page_alloc(&test->provider, &test->teardown_page);
	if (ret)
		goto fail_provider;
	ret = idw_edu_check_page(&test->teardown_page);
	if (ret)
		goto fail_provider;

	pci_set_drvdata(pdev, test);
	idw_edu_remove_result = -EINPROGRESS;
	idw_edu_test_result = 0;
	pr_info("IDW_QEMU_DMA: probe passed; retained one page to verify remove cleanup\n");
	return 0;

fail_provider:
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
	int ret;

	if (!test)
		return;
	outstanding = !list_empty(&test->provider.pages);
	ret = idw_dma_provider_fini(&test->provider);
	test->provider_ready = false;
	pci_clear_master(pdev);
	pci_iounmap(pdev, test->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(test);
	idw_edu_remove_result = outstanding && !ret ? 0 : (ret ? ret : -EINVAL);
	pr_info("IDW_QEMU_DMA: remove freed outstanding provider pages=%s result=%d\n",
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
