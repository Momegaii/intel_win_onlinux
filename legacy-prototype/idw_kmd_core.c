#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/atomic.h>

#include "idw_uapi.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("idw project");
MODULE_DESCRIPTION("Cross-kernel WDDM-to-Linux GTT and command submission shim");

#define IDW_GGTT_VA_START   0x0000000010000000ULL
#define IDW_GGTT_VA_END     0x0000000010100000ULL
#define IDW_PPGTT_VA_START  0x0000800000000000ULL
#define IDW_PPGTT_VA_END    0x0000800010000000ULL

struct idw_gtt_region {
    struct list_head node;
    uint64_t gpu_va;
    uint64_t size_bytes;
    uint64_t dma_iova;
    uint32_t domain;
    uint32_t flags;
};

struct idw_gtt_vm {
    uint64_t va_start;
    uint64_t va_end;
    uint64_t next_free_va;
    uint64_t next_dma_iova;
    uint64_t tlb_flush_seq;
    uint64_t active_regions;
    struct list_head regions;
};

struct idw_ring_slot {
    uint64_t fence_seq;
    uint32_t op_code;
    uint32_t bytes;
    uint8_t  payload[IDW_MAX_PAYLOAD];
};

struct idw_core_state {
    struct mutex lock;
    spinlock_t ring_lock;
    struct workqueue_struct *ack_wq;
    struct work_struct ack_work;

    struct idw_ring_slot ring[IDW_RING_SLOTS];
    uint32_t ring_head;
    uint32_t ring_tail;

    uint64_t submitted_fence;
    uint64_t completed_fence;
    atomic64_t next_gem_handle;

    struct idw_gtt_vm ggtt_vm;
};

struct idw_client_ctx {
    struct idw_gtt_vm ppgtt_vm;
};

static struct idw_core_state idw_state;

static uint64_t idw_align_up_4k(uint64_t val)
{
    return (val + 4095ULL) & ~4095ULL;
}

static void idw_vm_init(struct idw_gtt_vm *vm, uint64_t start, uint64_t end, uint64_t iova_base)
{
    vm->va_start = start;
    vm->va_end = end;
    vm->next_free_va = start;
    vm->next_dma_iova = iova_base;
    vm->tlb_flush_seq = 0ULL;
    vm->active_regions = 0ULL;
    INIT_LIST_HEAD(&vm->regions);
}

static void idw_vm_destroy(struct idw_gtt_vm *vm, const char *label)
{
    struct idw_gtt_region *entry, *tmp;
    uint64_t leaked = 0ULL;

    list_for_each_entry_safe(entry, tmp, &vm->regions, node) {
        leaked++;
        list_del(&entry->node);
        kfree(entry);
    }
    if (leaked > 0ULL) {
        pr_warn("idw_gtt_mapper: %s freed %llu un-unmapped regions on teardown\n",
                label, leaked);
    }
}

static void idw_deferred_ack_fn(struct work_struct *work)
{
    unsigned long flags;
    uint64_t target_fence = 0ULL;

    spin_lock_irqsave(&idw_state.ring_lock, flags);
    while (idw_state.ring_tail != idw_state.ring_head) {
        target_fence = idw_state.ring[idw_state.ring_tail].fence_seq;
        idw_state.ring_tail = (idw_state.ring_tail + 1U) % IDW_RING_SLOTS;
    }
    if (target_fence > idw_state.completed_fence) {
        idw_state.completed_fence = target_fence;
    }
    spin_unlock_irqrestore(&idw_state.ring_lock, flags);

    pr_debug("win-gpu fence ack: completed=%llu\n", idw_state.completed_fence);
}

static int idw_open(struct inode *inode, struct file *file)
{
    struct idw_client_ctx *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    mutex_lock(&idw_state.lock);
    idw_vm_init(&ctx->ppgtt_vm, IDW_PPGTT_VA_START, IDW_PPGTT_VA_END,
                0x0000000100000000ULL + (atomic64_inc_return(&idw_state.next_gem_handle) << 24));
    mutex_unlock(&idw_state.lock);

    file->private_data = ctx;
    pr_debug("initialized win-gpu per-client PPGTT context (%p)\n", ctx);
    return 0;
}

static int idw_release(struct inode *inode, struct file *file)
{
    struct idw_client_ctx *ctx = file->private_data;

    if (ctx) {
        mutex_lock(&idw_state.lock);
        idw_vm_destroy(&ctx->ppgtt_vm, "per-client PPGTT");
        mutex_unlock(&idw_state.lock);
        kfree(ctx);
        pr_debug("cleaned up win-gpu per-client PPGTT context (%p)\n", ctx);
    }
    return 0;
}

static long idw_handle_submit(struct file *file, unsigned long arg)
{
    struct idw_submit_req req;
    uint8_t *kbuf = NULL;
    unsigned long flags;
    uint32_t next_head;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (req.cmd_buf_bytes == 0U || req.cmd_buf_bytes > IDW_MAX_PAYLOAD || req.cmd_buf_uaddr == 0ULL) {
        req.nt_status = -EINVAL;
        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;
        return -EINVAL;
    }

    kbuf = kzalloc(req.cmd_buf_bytes, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, (void __user *)(uintptr_t)req.cmd_buf_uaddr, req.cmd_buf_bytes)) {
        kfree(kbuf);
        return -EFAULT;
    }

    spin_lock_irqsave(&idw_state.ring_lock, flags);
    next_head = (idw_state.ring_head + 1U) % IDW_RING_SLOTS;
    if (next_head == idw_state.ring_tail) {
        spin_unlock_irqrestore(&idw_state.ring_lock, flags);
        kfree(kbuf);
        return -EBUSY;
    }

    idw_state.submitted_fence++;
    req.fence_seq = idw_state.submitted_fence;
    idw_state.ring[idw_state.ring_head].fence_seq = req.fence_seq;
    idw_state.ring[idw_state.ring_head].op_code = req.op_code;
    idw_state.ring[idw_state.ring_head].bytes = req.cmd_buf_bytes;
    memcpy(idw_state.ring[idw_state.ring_head].payload, kbuf, req.cmd_buf_bytes);
    idw_state.ring_head = next_head;
    spin_unlock_irqrestore(&idw_state.ring_lock, flags);

    kfree(kbuf);

    req.nt_status = 0;
    pr_debug("win-gpu submit: op=%u bytes=%u fence=%llu\n",
             req.op_code, req.cmd_buf_bytes, req.fence_seq);

    queue_work(idw_state.ack_wq, &idw_state.ack_work);

    if (copy_to_user((void __user *)arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;
}

static long idw_handle_query_fence(unsigned long arg)
{
    struct idw_fence_query q;
    unsigned long flags;

    if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
        return -EFAULT;

    spin_lock_irqsave(&idw_state.ring_lock, flags);
    q.submitted_fence = idw_state.submitted_fence;
    q.completed_fence = idw_state.completed_fence;
    spin_unlock_irqrestore(&idw_state.ring_lock, flags);

    q.nt_status = 0;
    if (copy_to_user((void __user *)arg, &q, sizeof(q)))
        return -EFAULT;
    return 0;
}

static long idw_handle_gtt_map(struct idw_client_ctx *ctx, unsigned long arg)
{
    struct idw_gtt_map_req req;
    struct idw_gtt_vm *vm;
    struct idw_gtt_region *reg;
    uint64_t aligned_sz, gpu_va;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    aligned_sz = idw_align_up_4k(req.size_bytes);
    if (aligned_sz == 0ULL)
        return -EINVAL;

    mutex_lock(&idw_state.lock);
    if (req.domain == IDW_GTT_DOMAIN_GGTT) {
        vm = &idw_state.ggtt_vm;
    } else if (req.domain == IDW_GTT_DOMAIN_PPGTT) {
        vm = &ctx->ppgtt_vm;
    } else {
        mutex_unlock(&idw_state.lock);
        return -EINVAL;
    }

    gpu_va = (req.requested_gpu_va != 0ULL) ? idw_align_up_4k(req.requested_gpu_va) : vm->next_free_va;
    if (gpu_va < vm->va_start || gpu_va + aligned_sz > vm->va_end) {
        mutex_unlock(&idw_state.lock);
        return -ENOSPC;
    }

    reg = kzalloc(sizeof(*reg), GFP_KERNEL);
    if (!reg) {
        mutex_unlock(&idw_state.lock);
        return -ENOMEM;
    }

    reg->gpu_va = gpu_va;
    reg->size_bytes = aligned_sz;
    reg->dma_iova = vm->next_dma_iova;
    reg->domain = req.domain;
    reg->flags = req.flags;
    list_add_tail(&reg->node, &vm->regions);

    vm->next_free_va = gpu_va + aligned_sz;
    vm->next_dma_iova += aligned_sz;
    vm->active_regions++;
    vm->tlb_flush_seq++;

    req.mapped_gpu_va = reg->gpu_va;
    req.mapped_dma_iova = reg->dma_iova;
    req.tlb_flush_seq = vm->tlb_flush_seq;
    req.nt_status = 0;

    pr_debug("idw_gtt_mapper: domain=%u va=0x%llx iova=0x%llx tlb_seq=%llu\n",
             req.domain, req.mapped_gpu_va, req.mapped_dma_iova, req.tlb_flush_seq);

    mutex_unlock(&idw_state.lock);

    if (copy_to_user((void __user *)arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;
}

static long idw_handle_gtt_unmap(struct idw_client_ctx *ctx, unsigned long arg)
{
    struct idw_gtt_unmap_req req;
    struct idw_gtt_vm *vm;
    struct idw_gtt_region *entry, *tmp;
    bool found = false;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    mutex_lock(&idw_state.lock);
    if (req.domain == IDW_GTT_DOMAIN_GGTT) {
        vm = &idw_state.ggtt_vm;
    } else if (req.domain == IDW_GTT_DOMAIN_PPGTT) {
        vm = &ctx->ppgtt_vm;
    } else {
        mutex_unlock(&idw_state.lock);
        return -EINVAL;
    }

    list_for_each_entry_safe(entry, tmp, &vm->regions, node) {
        if (entry->gpu_va == req.mapped_gpu_va) {
            list_del(&entry->node);
            kfree(entry);
            if (vm->active_regions > 0ULL)
                vm->active_regions--;
            vm->tlb_flush_seq++;
            req.tlb_flush_seq = vm->tlb_flush_seq;
            req.nt_status = 0;
            found = true;
            break;
        }
    }
    mutex_unlock(&idw_state.lock);

    if (!found)
        return -ENOENT;

    pr_debug("idw_gtt_mapper unmap: domain=%u va=0x%llx tlb_seq=%llu\n",
             req.domain, req.mapped_gpu_va, req.tlb_flush_seq);

    if (copy_to_user((void __user *)arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;
}

static long idw_handle_wddm_translate(unsigned long arg)
{
    struct idw_wddm_translate_req req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (req.irql_level != 0U && req.irql_level != 2U)
        return -EINVAL;

    if (req.wddm_ddi_kind < IDW_WDDM_DDI_SUBMIT_RENDER ||
        req.wddm_ddi_kind > IDW_WDDM_DDI_CREATE_ALLOC)
        return -EINVAL;

    req.linux_dma_fence = req.wddm_fence_val ^ 0x4C494E5558ULL;
    req.drm_gem_handle = (uint32_t)atomic64_inc_return(&idw_state.next_gem_handle);
    req.nt_status = 0;

    pr_debug("wddm_translate: ddi=%u irql=%u wddm_fence=%llu -> gem=%u\n",
             req.wddm_ddi_kind, req.irql_level, req.wddm_fence_val, req.drm_gem_handle);

    if (copy_to_user((void __user *)arg, &req, sizeof(req)))
        return -EFAULT;
    return 0;
}

static long idw_handle_query_debug(struct idw_client_ctx *ctx, unsigned long arg)
{
    struct idw_debug_stats dbg;
    unsigned long flags;

    if (copy_from_user(&dbg, (void __user *)arg, sizeof(dbg)))
        return -EFAULT;

    spin_lock_irqsave(&idw_state.ring_lock, flags);
    dbg.submitted_fence = idw_state.submitted_fence;
    dbg.completed_fence = idw_state.completed_fence;
    dbg.ring_head = idw_state.ring_head;
    dbg.ring_tail = idw_state.ring_tail;
    spin_unlock_irqrestore(&idw_state.ring_lock, flags);

    mutex_lock(&idw_state.lock);
    dbg.ggtt_tlb_flush_seq = idw_state.ggtt_vm.tlb_flush_seq;
    dbg.active_ggtt_regions = idw_state.ggtt_vm.active_regions;
    dbg.ppgtt_tlb_flush_seq = ctx->ppgtt_vm.tlb_flush_seq;
    dbg.active_ppgtt_regions = ctx->ppgtt_vm.active_regions;
    mutex_unlock(&idw_state.lock);

    dbg.nt_status = 0;
    if (copy_to_user((void __user *)arg, &dbg, sizeof(dbg)))
        return -EFAULT;
    return 0;
}

static long idw_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct idw_client_ctx *ctx = file->private_data;

    switch (cmd) {
    case IDW_IOC_SUBMIT:
        return idw_handle_submit(file, arg);
    case IDW_IOC_QUERY_FENCE:
        return idw_handle_query_fence(arg);
    case IDW_IOC_GTT_MAP:
        return idw_handle_gtt_map(ctx, arg);
    case IDW_IOC_GTT_UNMAP:
        return idw_handle_gtt_unmap(ctx, arg);
    case IDW_IOC_WDDM_TRANSLATE:
        return idw_handle_wddm_translate(arg);
    case IDW_IOC_QUERY_DEBUG:
        return idw_handle_query_debug(ctx, arg);
    default:
        return -ENOTTY;
    }
}

static const struct file_operations idw_fops = {
    .owner = THIS_MODULE,
    .open = idw_open,
    .release = idw_release,
    .unlocked_ioctl = idw_ioctl,
};

static struct miscdevice idw_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = IDW_DEV_NAME,
    .fops = &idw_fops,
    .mode = 0666,
};

static int __init idw_init(void)
{
    int ret;

    mutex_init(&idw_state.lock);
    spin_lock_init(&idw_state.ring_lock);
    atomic64_set(&idw_state.next_gem_handle, 100LL);
    idw_vm_init(&idw_state.ggtt_vm, IDW_GGTT_VA_START, IDW_GGTT_VA_END, 0x0000000020000000ULL);
    INIT_WORK(&idw_state.ack_work, idw_deferred_ack_fn);

    idw_state.ack_wq = alloc_workqueue("idw_ack_wq", WQ_UNBOUND, 1);
    if (!idw_state.ack_wq)
        return -ENOMEM;

    ret = misc_register(&idw_miscdev);
    if (ret) {
        destroy_workqueue(idw_state.ack_wq);
        return ret;
    }

    pr_info("win-gpu shim loaded (/dev/%s)\n", IDW_DEV_NAME);
    return 0;
}

static void __exit idw_exit(void)
{
    misc_deregister(&idw_miscdev);
    if (idw_state.ack_wq) {
        flush_workqueue(idw_state.ack_wq);
        destroy_workqueue(idw_state.ack_wq);
    }
    mutex_lock(&idw_state.lock);
    idw_vm_destroy(&idw_state.ggtt_vm, "shared GGTT");
    mutex_unlock(&idw_state.lock);
    pr_info("win-gpu shim unloaded\n");
}

module_init(idw_init);
module_exit(idw_exit);
