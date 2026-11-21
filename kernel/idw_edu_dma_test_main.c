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
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
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

enum idw_edu_owner_state {
	IDW_EDU_OWNER_INITIALIZING,
	IDW_EDU_OWNER_ACTIVE,
	IDW_EDU_OWNER_CLEANUP_PENDING,
	IDW_EDU_OWNER_QUARANTINED,
	IDW_EDU_OWNER_RECOVERING,
	IDW_EDU_OWNER_RELEASED,
};

struct idw_edu_test {
	struct device *dev;
	struct pci_dev *pdev;
	void __iomem *bar;
	struct idw_dma_provider provider;
	struct idw_dma_pt_adapter pt_adapter;
	struct miscdevice misc;
	struct completion closed;
	struct mutex ops_lock;
	struct mutex lifecycle_lock;
	atomic_t open;
	struct kref refs;
	struct list_head registry_link;
	u64 owner_id;
	enum idw_edu_owner_state state;
	bool registered;
	bool module_ref;
	bool provider_ready;
	bool pt_adapter_ready;
	bool misc_ready;
	bool pci_enabled;
	bool pci_regions;
	bool pci_master;
	bool bar_mapped;
	bool adapter_cleanup_done;
	bool provider_cleanup_done;
	bool pci_cleanup_done;
	struct idw_dma_page_info probe_test_page;
	bool probe_test_page_valid;
	int cleanup_error;
};

#define IDW_EDU_MAX_OWNERS 8

static struct idw_edu_test *idw_edu_active;
static int idw_edu_test_result = -EINPROGRESS;
static int idw_edu_remove_result = -EINPROGRESS;
static LIST_HEAD(idw_edu_owners);
static DEFINE_MUTEX(idw_edu_registry_lock);
static unsigned int idw_edu_owner_count;
static unsigned int idw_edu_owner_state_snapshot;
static atomic64_t idw_edu_next_owner_id = ATOMIC64_INIT(0);
static struct workqueue_struct *idw_edu_cleanup_wq;
static struct delayed_work idw_edu_cleanup_work;
static bool idw_edu_probe_failure_test;

module_param_named(test_result, idw_edu_test_result, int, 0444);
module_param_named(probe_failure_test, idw_edu_probe_failure_test, bool, 0444);
module_param_named(owner_count, idw_edu_owner_count, uint, 0444);
module_param_named(owner_state, idw_edu_owner_state_snapshot, uint, 0444);

static int idw_edu_provider_fini_checked(struct idw_edu_test *test)
{
        int ret;

        if (!test || !test->provider_ready)
                return 0;
        ret = idw_dma_provider_fini(&test->provider);
        if (!ret)
                test->provider_ready = false;
        return ret;
}

static int idw_edu_owner_cleanup(struct idw_edu_test *test);
static void idw_edu_registry_retry(void);
static void idw_edu_schedule_cleanup(void);

static const char *idw_edu_owner_state_name(enum idw_edu_owner_state state)
{
        switch (state) {
        case IDW_EDU_OWNER_INITIALIZING:
                return "INITIALIZING";
        case IDW_EDU_OWNER_ACTIVE:
                return "ACTIVE";
        case IDW_EDU_OWNER_CLEANUP_PENDING:
                return "CLEANUP_PENDING";
        case IDW_EDU_OWNER_QUARANTINED:
                return "QUARANTINED";
        case IDW_EDU_OWNER_RECOVERING:
                return "RECOVERING";
        case IDW_EDU_OWNER_RELEASED:
                return "RELEASED";
        default:
                return "UNKNOWN";
        }
}

static void idw_edu_update_observability_locked(void)
{
        struct idw_edu_test *first;

        idw_edu_owner_state_snapshot = 0;
        if (list_empty(&idw_edu_owners))
                return;
        first = list_first_entry(&idw_edu_owners, struct idw_edu_test,
                                 registry_link);
        idw_edu_owner_state_snapshot = first->state;
}

static void idw_edu_owner_release(struct kref *ref)
{
        struct idw_edu_test *test = container_of(ref, struct idw_edu_test,
                                                 refs);

        if (test->module_ref) {
                test->module_ref = false;
                module_put(THIS_MODULE);
        }
        kfree(test);
}

static void idw_edu_owner_get(struct idw_edu_test *test)
{
        kref_get(&test->refs);
}

static void idw_edu_owner_put(struct idw_edu_test *test)
{
        if (test)
                kref_put(&test->refs, idw_edu_owner_release);
}

static void idw_edu_pci_cleanup(struct idw_edu_test *test)
{
        if (!test || !test->pdev)
                return;
        pr_info("IDW_QEMU_DMA: owner %llu PCI cleanup begin enabled=%d regions=%d master=%d bar=%d\n",
                (unsigned long long)test->owner_id, test->pci_enabled,
                test->pci_regions, test->pci_master, test->bar_mapped);
        if (test->pci_master) {
                pci_clear_master(test->pdev);
                test->pci_master = false;
        }
        if (test->bar_mapped) {
                pci_iounmap(test->pdev, test->bar);
                test->bar = NULL;
                test->bar_mapped = false;
        }
        if (test->pci_regions) {
                pci_release_regions(test->pdev);
                test->pci_regions = false;
        }
        if (test->pci_enabled) {
                pci_disable_device(test->pdev);
                test->pci_enabled = false;
        }
        pci_set_drvdata(test->pdev, NULL);
        test->pci_cleanup_done = true;
        pr_info("IDW_QEMU_DMA: owner %llu PCI cleanup complete enabled=%d regions=%d master=%d bar=%d\n",
                (unsigned long long)test->owner_id, test->pci_enabled,
                test->pci_regions, test->pci_master, test->bar_mapped);
}

static bool idw_edu_registry_capacity(void)
{
        bool available;

        mutex_lock(&idw_edu_registry_lock);
        available = idw_edu_owner_count < IDW_EDU_MAX_OWNERS;
        mutex_unlock(&idw_edu_registry_lock);
        return available;
}

static int idw_edu_registry_add(struct idw_edu_test *test)
{
        if (!test)
                return -EINVAL;
        if (test->registered)
                return 0;
        if (!try_module_get(THIS_MODULE))
                return -ENODEV;

        mutex_lock(&idw_edu_registry_lock);
        if (idw_edu_owner_count >= IDW_EDU_MAX_OWNERS) {
                mutex_unlock(&idw_edu_registry_lock);
                module_put(THIS_MODULE);
                return -ENOSPC;
        }
        idw_edu_owner_get(test);
        test->registered = true;
        test->module_ref = true;
        test->owner_id = atomic64_inc_return(&idw_edu_next_owner_id);
        test->state = IDW_EDU_OWNER_QUARANTINED;
        list_add_tail(&test->registry_link, &idw_edu_owners);
        idw_edu_owner_count++;
        idw_edu_update_observability_locked();
        mutex_unlock(&idw_edu_registry_lock);
        idw_edu_schedule_cleanup();
        return 0;
}

static void idw_edu_registry_remove(struct idw_edu_test *test)
{
        bool found = false;

        if (!test)
                return;
        mutex_lock(&idw_edu_registry_lock);
        if (test->registered) {
                list_del(&test->registry_link);
                test->registered = false;
                if (idw_edu_owner_count)
                        idw_edu_owner_count--;
                idw_edu_update_observability_locked();
                found = true;
        }
        mutex_unlock(&idw_edu_registry_lock);
        if (found)
                idw_edu_owner_put(test);
}

static bool idw_edu_registry_contains(struct idw_edu_test *test)
{
        struct idw_edu_test *entry;
        bool found = false;

        mutex_lock(&idw_edu_registry_lock);
        list_for_each_entry(entry, &idw_edu_owners, registry_link) {
                if (entry == test) {
                        found = true;
                        break;
                }
        }
        mutex_unlock(&idw_edu_registry_lock);
        return found;
}

static int idw_edu_owner_cleanup(struct idw_edu_test *test)
{
        int ret;

        if (!test)
                return -EINVAL;

        mutex_lock(&test->lifecycle_lock);
        if (test->state == IDW_EDU_OWNER_RELEASED) {
                mutex_unlock(&test->lifecycle_lock);
                idw_edu_registry_remove(test);
                return 0;
        }
        if (test->state == IDW_EDU_OWNER_QUARANTINED) {
                test->state = IDW_EDU_OWNER_RECOVERING;
                pr_info("IDW_QEMU_DMA: owner %llu recovering from quarantine\n",
                        (unsigned long long)test->owner_id);
        }
        test->state = IDW_EDU_OWNER_CLEANUP_PENDING;
        test->cleanup_error = 0;

        if (test->misc_ready) {
                misc_deregister(&test->misc);
                WRITE_ONCE(idw_edu_active, NULL);
                if (atomic_read(&test->open))
                        wait_for_completion(&test->closed);
                test->misc_ready = false;
        }

        if (test->pt_adapter_ready && !test->adapter_cleanup_done) {
                ret = idw_dma_pt_adapter_fini(&test->pt_adapter);
                if (ret) {
                        test->cleanup_error = ret;
                        test->state = IDW_EDU_OWNER_QUARANTINED;
                        pr_err("IDW_QEMU_DMA: owner %llu adapter cleanup blocked: %d\n",
                               (unsigned long long)test->owner_id, ret);
                        mutex_unlock(&test->lifecycle_lock);
                        idw_edu_schedule_cleanup();
                        return ret;
                }
                test->pt_adapter_ready = false;
                test->adapter_cleanup_done = true;
        }

        if (test->provider_ready && !test->provider_cleanup_done) {
                ret = idw_edu_provider_fini_checked(test);
                if (ret) {
                        test->cleanup_error = ret;
                        test->state = IDW_EDU_OWNER_QUARANTINED;
                        pr_err("IDW_QEMU_DMA: owner %llu provider cleanup blocked: %d pci_enabled=%d regions=%d master=%d bar=%d\n",
                               (unsigned long long)test->owner_id, ret,
                               test->pci_enabled, test->pci_regions,
                               test->pci_master, test->bar_mapped);
                        mutex_unlock(&test->lifecycle_lock);
                        idw_edu_schedule_cleanup();
                        return ret;
                }
                test->provider_cleanup_done = true;
        }

        idw_edu_pci_cleanup(test);
        test->state = IDW_EDU_OWNER_RELEASED;
        pr_info("IDW_QEMU_DMA: owner %llu released state=RELEASED\n",
                (unsigned long long)test->owner_id);
        mutex_unlock(&test->lifecycle_lock);
        idw_edu_registry_remove(test);
        return 0;
}

static void idw_edu_registry_retry(void)
{
        struct idw_edu_test *snapshot[IDW_EDU_MAX_OWNERS];
        struct idw_edu_test *entry;
        unsigned int count = 0;
        unsigned int i;

        mutex_lock(&idw_edu_registry_lock);
        list_for_each_entry(entry, &idw_edu_owners, registry_link) {
                if (count >= IDW_EDU_MAX_OWNERS)
                        break;
                if (entry->state != IDW_EDU_OWNER_QUARANTINED &&
                    entry->state != IDW_EDU_OWNER_CLEANUP_PENDING)
                        continue;
                idw_edu_owner_get(entry);
                snapshot[count++] = entry;
        }
        mutex_unlock(&idw_edu_registry_lock);

        for (i = 0; i < count; i++) {
                if (snapshot[i]->probe_test_page_valid) {
                        int page_ret = idw_dma_page_free(&snapshot[i]->provider,
                                                         &snapshot[i]->probe_test_page);

                        if (!page_ret) {
                                snapshot[i]->probe_test_page_valid = false;
                                memset(&snapshot[i]->probe_test_page, 0,
                                       sizeof(snapshot[i]->probe_test_page));
                                pr_info("IDW_QEMU_DMA: owner %llu released probe-failure test page\n",
                                        (unsigned long long)snapshot[i]->owner_id);
                        } else {
                                pr_err("IDW_QEMU_DMA: owner %llu test page release failed: %d\n",
                                       (unsigned long long)snapshot[i]->owner_id,
                                       page_ret);
                        }
                }
                idw_edu_owner_cleanup(snapshot[i]);
                idw_edu_owner_put(snapshot[i]);
        }
}

static void idw_edu_cleanup_work_fn(struct work_struct *work)
{
        bool pending;

        if (!try_module_get(THIS_MODULE))
                return;
        idw_edu_registry_retry();
        mutex_lock(&idw_edu_registry_lock);
        pending = idw_edu_owner_count != 0;
        mutex_unlock(&idw_edu_registry_lock);
        if (pending && idw_edu_cleanup_wq)
                queue_delayed_work(idw_edu_cleanup_wq, &idw_edu_cleanup_work,
                                   HZ);
        module_put(THIS_MODULE);
}

static void idw_edu_schedule_cleanup(void)
{
        if (idw_edu_cleanup_wq)
                queue_delayed_work(idw_edu_cleanup_wq, &idw_edu_cleanup_work,
                                   HZ);
}

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
        struct idw_dma_resource directional = {};
        struct idw_dma_resource bad = {};
        struct idw_dma_provider_stats stats = {};
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

        ret = idw_dma_resource_alloc(&test->provider, PAGE_SIZE,
                                     DMA_TO_DEVICE, &directional);
        if (ret)
                goto bad_descriptor;
        ret = idw_dma_resource_sync(&test->provider, &directional, 0, PAGE_SIZE,
                                    DMA_FROM_DEVICE);
        if (ret != -EINVAL)
                goto free_directional;
        ret = idw_dma_resource_sync(&test->provider, &directional, 0, PAGE_SIZE,
                                    DMA_TO_DEVICE);
        if (ret)
                goto free_directional;
        ret = idw_dma_resource_free(&test->provider, &directional);
        if (ret)
                goto bad_descriptor;
        memset(&directional, 0, sizeof(directional));

        for (i = 0; i < 3; i++) {
                test->provider.fail_alloc_at = (s32)i;
                ret = idw_dma_resource_alloc(&test->provider, 3 * PAGE_SIZE,
                                             DMA_BIDIRECTIONAL, &bad);
                test->provider.fail_alloc_at = -1;
                if (ret != -ENOMEM)
                        goto bad_descriptor;
                memset(&bad, 0, sizeof(bad));

                test->provider.fail_map_at = (s32)i;
                ret = idw_dma_resource_alloc(&test->provider, 3 * PAGE_SIZE,
                                             DMA_BIDIRECTIONAL, &bad);
                test->provider.fail_map_at = -1;
                if (ret != -EIO)
                        goto bad_descriptor;
                memset(&bad, 0, sizeof(bad));
        }
        ret = idw_dma_provider_stats(&test->provider, &stats);
        if (ret || stats.resources != 2 || stats.resource_pages != 6 ||
            stats.mapped_pages != 6 || stats.quarantined_pages)
                goto bad_descriptor;
        pr_info("IDW_QEMU_DMA: backing allocation/mapping fault injection verified\n");

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
free_directional:
        test->provider.fail_alloc_at = -1;
        test->provider.fail_map_at = -1;
        if (directional.pages) {
                int release = idw_dma_resource_free(&test->provider,
                                                    &directional);
                if (!ret)
                        ret = release;
        }
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


static int idw_edu_test_provider_removal(struct idw_edu_test *test)
{
	struct idw_edu_test local = {};
	struct idw_dma_resource resource = {};
	struct idw_dma_page_info page = {};
	int ret;

	local.dev = test->dev;
	ret = idw_dma_provider_init(&local.provider, local.dev, THIS_MODULE, 1);
	if (ret)
		return ret;
	local.provider_ready = true;
	ret = idw_dma_resource_alloc(&local.provider, PAGE_SIZE,
				     DMA_BIDIRECTIONAL, &resource);
	if (ret)
		goto fini;
	atomic_inc(&local.provider.vma_refs);
	ret = idw_edu_provider_fini_checked(&local);
	if (ret != -EBUSY || !local.provider_ready ||
		    !local.provider.removal_blocked) {
		ret = -EIO;
		goto free_resource;
	}
	atomic_dec(&local.provider.vma_refs);
	ret = idw_dma_resource_free(&local.provider, &resource);
	if (ret)
		goto fini;
	ret = idw_edu_provider_fini_checked(&local);
	if (ret)
		return ret;

	memset(&local, 0, sizeof(local));
	local.dev = test->dev;
	ret = idw_dma_provider_init(&local.provider, local.dev, THIS_MODULE, 2);
	if (ret)
		return ret;
	local.provider_ready = true;
	ret = idw_dma_page_alloc(&local.provider, &page);
	if (ret)
		goto fini;
	ret = idw_edu_provider_fini_checked(&local);
	if (ret != -EBUSY || !local.provider_ready ||
		    !local.provider.removal_blocked) {
		ret = -EIO;
		goto free_page;
	}
	ret = idw_dma_page_free(&local.provider, &page);
	if (ret)
		goto fini;
	ret = idw_edu_provider_fini_checked(&local);
	if (ret)
		return ret;
	pr_info("IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release\n");
	pr_info("IDW_QEMU_DMA: provider finalization refusal retained owner and recovered after release\n");
	return 0;

free_page:
	(void)idw_dma_page_free(&local.provider, &page);
	goto fini;
free_resource:
	(void)idw_dma_resource_free(&local.provider, &resource);
fini:
	(void)idw_edu_provider_fini_checked(&local);
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

static bool idw_edu_vma_get(struct idw_dma_provider *provider)
{
        if (!provider || !try_module_get(THIS_MODULE))
                return false;
        atomic_inc(&provider->vma_refs);
        return true;
}

static void idw_edu_vma_put(struct idw_dma_provider *provider)
{
        if (!provider)
                return;
        if (atomic_dec_if_positive(&provider->vma_refs) < 0)
                return;
        module_put(THIS_MODULE);
}

static void idw_edu_resource_vm_open(struct vm_area_struct *vma)
{
        struct idw_dma_resource *resource = vma->vm_private_data;

        if (!resource)
                return;
        if (!idw_edu_vma_get(resource->provider)) {
                vma->vm_private_data = NULL;
                return;
        }
        atomic_inc(&resource->mappings);
}

static void idw_edu_resource_vm_close(struct vm_area_struct *vma)
{
        struct idw_dma_resource *resource = vma->vm_private_data;

        if (!resource)
                return;
        (void)atomic_dec_if_positive(&resource->mappings);
        idw_edu_vma_put(resource->provider);
}

static const struct vm_operations_struct idw_edu_resource_vm_ops = {
        .open = idw_edu_resource_vm_open,
        .close = idw_edu_resource_vm_close,
};

static void idw_edu_pt_vm_open(struct vm_area_struct *vma)
{
        struct idw_dma_provider_page *page = vma->vm_private_data;

        if (!page)
                return;
        if (!idw_edu_vma_get(page->provider)) {
                vma->vm_private_data = NULL;
                return;
        }
        atomic_inc(&page->mappings);
}

static void idw_edu_pt_vm_close(struct vm_area_struct *vma)
{
        struct idw_dma_provider_page *page = vma->vm_private_data;

        if (!page)
                return;
        (void)atomic_dec_if_positive(&page->mappings);
        idw_edu_vma_put(page->provider);
}

static const struct vm_operations_struct idw_edu_pt_vm_ops = {
        .open = idw_edu_pt_vm_open,
        .close = idw_edu_pt_vm_close,
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
        int ret = 0;

        if (!test || !test->provider_ready)
                return -ENODEV;

        mutex_lock(&test->ops_lock);
        if (pgoff >= IDW_DMA_TEST_PT_PGOFF_BASE) {
                struct idw_dma_provider_page *page;

                cookie = pgoff - IDW_DMA_TEST_PT_PGOFF_BASE;
                page = idw_edu_find_page(test, cookie);
                if (!page) {
                        ret = -ENOENT;
                        goto out_unlock;
                }
                if (vma->vm_end - vma->vm_start != PAGE_SIZE) {
                        ret = -EINVAL;
                        goto out_unlock;
                }
                vma->vm_pgoff = 0;
                ret = dma_mmap_coherent(test->provider.dev, vma,
                                        page->cpu_address,
                                        page->dma_address, PAGE_SIZE);
                if (ret)
                        goto out_unlock;
                vma->vm_ops = &idw_edu_pt_vm_ops;
                vma->vm_private_data = page;
                vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
                if (!idw_edu_vma_get(page->provider)) {
                        vma->vm_private_data = NULL;
                        ret = -ENODEV;
                        goto out_unlock;
                }
                atomic_inc(&page->mappings);
                goto out_unlock;
        }

        cookie = pgoff;
        {
                struct idw_dma_resource *resource =
                        idw_edu_find_resource(test, cookie);

                if (!resource) {
                        ret = -ENOENT;
                        goto out_unlock;
                }
                if (vma->vm_end - vma->vm_start != resource->bytes) {
                        ret = -EINVAL;
                        goto out_unlock;
                }
                vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
                for (i = 0; i < resource->page_count; i++) {
                        if (vm_insert_page(vma,
                                           vma->vm_start + (unsigned long)i *
                                                   PAGE_SIZE,
                                           resource->pages[i].page)) {
                                ret = -EFAULT;
                                goto out_unlock;
                        }
                }
                vma->vm_pgoff = 0;
                vma->vm_ops = &idw_edu_resource_vm_ops;
                vma->vm_private_data = resource;
                vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
                if (!idw_edu_vma_get(resource->provider)) {
                        vma->vm_private_data = NULL;
                        ret = -ENODEV;
                        goto out_unlock;
                }
                atomic_inc(&resource->mappings);
        }

out_unlock:
        mutex_unlock(&test->ops_lock);
        return ret;
}

static long idw_edu_ioctl_locked(struct file *file, unsigned int cmd,
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
        case IDW_DMA_TEST_IOCTL_STATS: {
                struct idw_dma_test_stats output = {};
                struct idw_dma_provider_stats stats;
                int ret;

                ret = idw_dma_provider_stats(&test->provider, &stats);
                if (ret)
                        return ret;
                output.size = sizeof(output);
                output.resources = stats.resources;
                output.resource_pages = stats.resource_pages;
                output.mapped_pages = stats.mapped_pages;
                output.page_table_pages = stats.page_table_pages;
                output.vma_refs = stats.vma_refs;
                output.quarantined_pages = stats.quarantined_pages;
                output.active = stats.active;
                output.removal_blocked = stats.removal_blocked;
                if (copy_to_user(user, &output, sizeof(output)))
                        return -EFAULT;
                return 0;
        }
        case IDW_DMA_TEST_IOCTL_FAULT: {
                struct idw_dma_test_fault request;

                if (copy_from_user(&request, user, sizeof(request)))
                        return -EFAULT;
                if (request.size != sizeof(request))
                        return -EINVAL;
                if (request.alloc_fail_at < -1 || request.map_fail_at < -1 ||
                    (request.flags & ~(IDW_DMA_TEST_FAULT_UNMAP |
                                       IDW_DMA_TEST_FAULT_FREE |
                                       IDW_DMA_TEST_FAULT_FREE_PT)))
                        return -EINVAL;
                mutex_lock(&test->provider.lock);
                test->provider.fail_alloc_at = request.alloc_fail_at;
                test->provider.fail_map_at = request.map_fail_at;
                test->provider.fail_unmap =
                        request.flags & IDW_DMA_TEST_FAULT_UNMAP;
                test->provider.fail_free =
                        request.flags & IDW_DMA_TEST_FAULT_FREE;
                test->provider.fail_free_pt =
                        request.flags & IDW_DMA_TEST_FAULT_FREE_PT;
                mutex_unlock(&test->provider.lock);
                return 0;
        }
        default:
                return -ENOTTY;
        }
}

static long idw_edu_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg)
{
        struct idw_edu_test *test = file->private_data;
        long ret;

        if (!test || !test->provider_ready)
                return -ENODEV;
        mutex_lock(&test->ops_lock);
        ret = idw_edu_ioctl_locked(file, cmd, arg);
        mutex_unlock(&test->ops_lock);
        return ret;
}

static const struct file_operations idw_edu_fops = {
        .owner = THIS_MODULE,
        .open = idw_edu_open,
        .release = idw_edu_release,
        .unlocked_ioctl = idw_edu_ioctl,
        .mmap = idw_edu_mmap,
        .llseek = noop_llseek,
};


static int idw_edu_test_owner_lifecycle(struct idw_edu_test *test)
{
        struct idw_edu_test *owner;
        struct idw_dma_resource *resource;
        struct idw_hsw_ppgtt_page_ops page_ops;
        struct idw_dma_page page = {};
        int ret;

        owner = kzalloc(sizeof(*owner), GFP_KERNEL);
        if (!owner)
                return -ENOMEM;
        resource = kzalloc(sizeof(*resource), GFP_KERNEL);
        if (!resource) {
                kfree(owner);
                return -ENOMEM;
        }

        owner->dev = test->dev;
        kref_init(&owner->refs);
        mutex_init(&owner->lifecycle_lock);
        owner->state = IDW_EDU_OWNER_INITIALIZING;

        ret = idw_dma_provider_init(&owner->provider, owner->dev, THIS_MODULE, 3);
        if (ret)
                goto free_owner;
        owner->provider_ready = true;

        ret = idw_edu_registry_add(owner);
        if (ret)
                goto free_provider;

        ret = idw_dma_pt_adapter_init(&owner->pt_adapter, &owner->provider);
        if (ret)
                goto free_provider;
        owner->pt_adapter_ready = true;
        ret = idw_dma_pt_adapter_page_ops(&owner->pt_adapter, &page_ops);
        if (ret)
                goto free_adapter;
        page.size = sizeof(page);
        ret = page_ops.alloc_page_table_page(page_ops.context, &page);
        if (ret)
                goto free_adapter;

        ret = idw_dma_resource_alloc(&owner->provider, PAGE_SIZE,
                                     DMA_BIDIRECTIONAL, resource);
        if (ret)
                goto free_page;

        ret = idw_edu_owner_cleanup(owner);
        if (ret != -EBUSY || owner->state != IDW_EDU_OWNER_QUARANTINED ||
            !owner->registered) {
                ret = -EIO;
                goto free_resource;
        }
        if (!idw_edu_registry_contains(owner) || idw_edu_owner_count != 1) {
                ret = -EIO;
                goto free_resource;
        }
        pr_info("IDW_QEMU_DMA: owner %llu quarantined adapter refs state=%s count=%u\n",
                (unsigned long long)owner->owner_id,
                idw_edu_owner_state_name(owner->state), idw_edu_owner_count);

        ret = idw_dma_pt_adapter_release_page(&owner->pt_adapter, &page);
        if (ret)
                goto free_resource;
        memset(&page, 0, sizeof(page));

        ret = idw_edu_owner_cleanup(owner);
        if (ret != -EBUSY || owner->state != IDW_EDU_OWNER_QUARANTINED ||
            !owner->registered || !owner->adapter_cleanup_done) {
                ret = -EIO;
                goto free_resource;
        }
        pr_info("IDW_QEMU_DMA: owner %llu adapter recovered, provider still blocked state=%s count=%u\n",
                (unsigned long long)owner->owner_id,
                idw_edu_owner_state_name(owner->state), idw_edu_owner_count);

        ret = idw_dma_resource_free(&owner->provider, resource);
        if (ret)
                goto free_resource;

        ret = idw_edu_owner_cleanup(owner);
        if (ret || owner->state != IDW_EDU_OWNER_RELEASED ||
            owner->registered || idw_edu_owner_count != 0 ||
            owner->provider.active) {
                ret = -EIO;
                goto free_resource;
        }
        pr_info("IDW_QEMU_DMA: owner %llu released after all dependencies drained\n",
                (unsigned long long)owner->owner_id);

        kfree(resource);
        idw_edu_owner_put(owner);
        return 0;

free_resource:
        if (resource)
                (void)idw_dma_resource_free(&owner->provider, resource);
free_page:
        if (page.cpu_address)
                (void)idw_dma_pt_adapter_release_page(&owner->pt_adapter, &page);
free_adapter:
        if (owner->pt_adapter_ready)
                (void)idw_dma_pt_adapter_fini(&owner->pt_adapter);
free_provider:
        if (owner->provider_ready) {
                int cleanup_ret = idw_dma_provider_fini(&owner->provider);

                if (cleanup_ret) {
                        pr_err("IDW_QEMU_DMA: lifecycle test provider cleanup blocked: %d\n",
                               cleanup_ret);
                        if (owner->registered)
                                idw_edu_owner_put(owner);
                        else
                                pr_err("IDW_QEMU_DMA: lifecycle test owner is unregistered\n");
                        return ret;
                }
                owner->provider_ready = false;
        }
        if (owner->registered)
                idw_edu_registry_remove(owner);
        kfree(resource);
free_owner:
        idw_edu_owner_put(owner);
        return ret;
}

static int idw_edu_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
        struct idw_edu_test *test;
        struct idw_dma_provider rejected = {};
        struct idw_dma_provider low_mask = {};
        int ret;
        bool owner_retained = false;

        (void)id;
        if (!idw_edu_registry_capacity())
                return -EBUSY;
        test = kzalloc(sizeof(*test), GFP_KERNEL);
        if (!test)
                return -ENOMEM;
        test->dev = &pdev->dev;
        test->pdev = pdev;
        kref_init(&test->refs);
        mutex_init(&test->lifecycle_lock);
        test->state = IDW_EDU_OWNER_INITIALIZING;

        ret = pci_enable_device_mem(pdev);
        if (ret)
                goto fail_state;
        test->pci_enabled = true;
        ret = pci_request_regions(pdev, "idw-edu-dma-test");
        if (ret)
                goto fail_disable;
        test->pci_regions = true;
        test->bar = pci_iomap(pdev, IDW_EDU_BAR, 0);
        if (!test->bar) {
                ret = -ENOMEM;
                goto fail_regions;
        }
        test->bar_mapped = true;

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
        ret = dma_set_mask(&pdev->dev, IDW_DMA_RESOURCE_ADDRESS_MASK);
        if (ret)
                goto fail_iomap;
        ret = dma_set_coherent_mask(&pdev->dev, IDW_DMA_ADDRESS_MASK);
        if (ret)
                goto fail_iomap;
        pr_info("IDW_QEMU_DMA: rejected 32-bit mask; accepted 39-bit streaming and 40-bit coherent masks\n");

        pci_set_master(pdev);
        test->pci_master = true;
        ret = idw_dma_provider_init(&test->provider, &pdev->dev,
                                    THIS_MODULE, 1);
        if (ret)
                goto fail_master;
        test->provider_ready = true;

        if (idw_edu_probe_failure_test) {
                ret = idw_dma_page_alloc(&test->provider,
                                         &test->probe_test_page);
                if (!ret)
                        test->probe_test_page_valid = true;
                else
                        goto fail_provider;
                ret = -EIO;
                goto fail_provider;
        }

        ret = idw_edu_test_pt_storage(test);
        if (ret)
                goto fail_provider;

        ret = idw_edu_test_dma_cycles(test);
        if (ret)
                goto fail_provider;

        ret = idw_edu_test_resource_backing(test);
        if (ret)
                goto fail_provider;
        ret = idw_edu_test_provider_removal(test);
        if (ret)
                goto fail_provider;
        ret = idw_edu_test_owner_lifecycle(test);
        if (ret)
                goto fail_provider;

        test->misc.minor = MISC_DYNAMIC_MINOR;
        test->misc.name = "idw-edu-dma";
        test->misc.fops = &idw_edu_fops;
        test->misc.parent = &pdev->dev;
        test->misc.mode = 0600;
        init_completion(&test->closed);
        mutex_init(&test->ops_lock);
        atomic_set(&test->open, 0);
        ret = misc_register(&test->misc);
        if (ret)
                goto fail_provider;
        test->misc_ready = true;
        WRITE_ONCE(idw_edu_active, test);

        pci_set_drvdata(pdev, test);
        test->state = IDW_EDU_OWNER_ACTIVE;
        idw_edu_remove_result = -EINPROGRESS;
        idw_edu_test_result = 0;
        pr_info("IDW_QEMU_DMA: probe passed; no outstanding provider pages retained\n");
        return 0;

fail_provider:
        ret = idw_edu_owner_cleanup(test);
        if (ret) {
                int reg_ret = idw_edu_registry_add(test);

                owner_retained = true;
                if (reg_ret)
                        pr_err("IDW_QEMU_DMA: owner registration failed: %d; cleanup remains blocked\n",
                               reg_ret);
                else
                        pr_err("IDW_QEMU_DMA: probe failure retained owner %llu state=%s count=%u\n",
                               (unsigned long long)test->owner_id,
                               idw_edu_owner_state_name(test->state),
                               idw_edu_owner_count);
        }
        goto fail_state;

fail_master:
fail_iomap:
fail_regions:
fail_disable:
        idw_edu_pci_cleanup(test);
fail_state:
        if (owner_retained) {
                if (test->registered)
                        idw_edu_owner_put(test);
                else
                        pr_err("IDW_QEMU_DMA: retained owner is unregistered; cleanup remains incomplete\n");
                idw_edu_test_result = ret;
                if (!idw_edu_probe_failure_test)
                        outb(IDW_QEMU_EXIT_FAIL, IDW_QEMU_DEBUG_EXIT_PORT);
                return ret;
        }
        idw_edu_owner_put(test);
        idw_edu_test_result = ret;
        pr_err("IDW_QEMU_DMA: probe test failed: %d\n", ret);
        outb(IDW_QEMU_EXIT_FAIL, IDW_QEMU_DEBUG_EXIT_PORT);
        return ret;
}

static void idw_edu_remove(struct pci_dev *pdev)
{
        struct idw_edu_test *test = pci_get_drvdata(pdev);
        bool outstanding;
        bool quarantined;
        int adapter_ret = 0;
        int ret = 0;

        if (!test)
                return;
        if (test->misc_ready) {
                misc_deregister(&test->misc);
                WRITE_ONCE(idw_edu_active, NULL);
                if (atomic_read(&test->open))
                        wait_for_completion(&test->closed);
                test->misc_ready = false;
        }
        mutex_lock(&test->ops_lock);
        if (test->pt_adapter_ready) {
                adapter_ret = idw_dma_pt_adapter_fini(&test->pt_adapter);
                if (!adapter_ret)
                        test->pt_adapter_ready = false;
        }
        outstanding = !list_empty(&test->provider.pages) ||
                      !list_empty(&test->provider.resources) ||
                      atomic_read(&test->provider.vma_refs);
        if (test->provider_ready)
                ret = idw_dma_provider_fini(&test->provider);
        quarantined = adapter_ret || ret || outstanding;
        if (quarantined) {
                int reg_ret;

                test->provider.removal_blocked = true;
                test->state = IDW_EDU_OWNER_QUARANTINED;
                mutex_unlock(&test->ops_lock);
                pci_set_drvdata(pdev, NULL);
                reg_ret = idw_edu_registry_add(test);
                if (!reg_ret)
                        idw_edu_owner_put(test);
                else
                        pr_err("IDW_QEMU_DMA: remove owner registration failed: %d; cleanup remains blocked\n",
                               reg_ret);
                idw_edu_remove_result = -EBUSY;
                pr_err("IDW_QEMU_DMA: remove quarantined outstanding resources adapter=%d provider=%d result=%d owner_count=%u\n",
                       adapter_ret, ret, idw_edu_remove_result,
                       idw_edu_owner_count);
                return;
        }
        test->provider_ready = false;
        test->pt_adapter_ready = false;
        test->state = IDW_EDU_OWNER_RELEASED;
        mutex_unlock(&test->ops_lock);
        idw_edu_pci_cleanup(test);
        pci_set_drvdata(pdev, NULL);
        idw_edu_owner_put(test);
        idw_edu_remove_result = 0;
        pr_info("IDW_QEMU_DMA: remove released all provider resources result=0\n");
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
	idw_edu_cleanup_wq = alloc_ordered_workqueue("idw_edu_cleanup",
							 WQ_MEM_RECLAIM);
	if (!idw_edu_cleanup_wq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&idw_edu_cleanup_work, idw_edu_cleanup_work_fn);
	ret = pci_register_driver(&idw_edu_driver);
	if (ret) {
		destroy_workqueue(idw_edu_cleanup_wq);
		idw_edu_cleanup_wq = NULL;
		return ret;
	}
	if (idw_edu_test_result == -EINPROGRESS) {
		pci_unregister_driver(&idw_edu_driver);
		destroy_workqueue(idw_edu_cleanup_wq);
		idw_edu_cleanup_wq = NULL;
		return -ENODEV;
	}
	return 0;
}

static void __exit idw_edu_exit(void)
{
	cancel_delayed_work_sync(&idw_edu_cleanup_work);
	pci_unregister_driver(&idw_edu_driver);
	if (idw_edu_cleanup_wq) {
		destroy_workqueue(idw_edu_cleanup_wq);
		idw_edu_cleanup_wq = NULL;
	}
	if (idw_edu_test_result == 0 && idw_edu_remove_result == 0) {
		pr_info("IDW_QEMU_DMA: driver removal completed successfully\n");
	} else if (idw_edu_probe_failure_test && idw_edu_owner_count == 0) {
		pr_info("IDW_QEMU_DMA: probe failure test recovered owner before unload\n");
	} else {
		pr_err("IDW_QEMU_DMA: FAIL: probe_result=%d remove_result=%d owner_count=%u\n",
		       idw_edu_test_result, idw_edu_remove_result,
		       idw_edu_owner_count);
		outb(IDW_QEMU_EXIT_FAIL, IDW_QEMU_DEBUG_EXIT_PORT);
	}
}

module_init(idw_edu_init);
module_exit(idw_edu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IDW DMA provider test using QEMU EDU PCI DMA");
