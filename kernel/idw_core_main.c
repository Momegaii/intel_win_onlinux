// SPDX-License-Identifier: GPL-2.0
/* IDW control plane + IDW-native miniport callback dispatcher.
 * No PCI binding, BAR mapping, DMA, MMIO, or GPU command submission here.
 */
#include <linux/dma-buf.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "../include/uapi/idw_ioctl.h"
#include "idw_miniport.h"

#define IDW_MAX_ALLOCATION_SIZE (256ULL * 1024 * 1024)
#define IDW_MAX_FILE_BYTES      (1024ULL * 1024 * 1024)

struct idw_allocation {
    u64 size;
    struct dma_buf *dmabuf;
    void *system_memory;
    struct kref ref;
    atomic_t mappings;
    bool published;
};

struct idw_file_ctx {
    struct mutex lock;
    struct idr allocations;
    u64 bytes;
    u32 next_allocation_id;
};

struct idw_dispatch_state {
    struct mutex lock;
    const struct idw_miniport *miniport;
    void *adapter_context;
    bool added;
    bool started;
};
static struct idw_dispatch_state idw_dispatch;

static void idw_allocation_release(struct kref *ref)
{
    struct idw_allocation *allocation =
        container_of(ref, struct idw_allocation, ref);

    if (allocation->dmabuf)
        dma_buf_put(allocation->dmabuf);
    vfree(allocation->system_memory);
    kfree(allocation);
}

static void idw_allocation_put(struct idw_allocation *allocation)
{
    kref_put(&allocation->ref, idw_allocation_release);
}

bool idw_miniport_is_registered(void)
{
    bool registered;
    mutex_lock(&idw_dispatch.lock);
    registered = idw_dispatch.miniport != NULL;
    mutex_unlock(&idw_dispatch.lock);
    return registered;
}
EXPORT_SYMBOL_GPL(idw_miniport_is_registered);

int idw_register_miniport(const struct idw_miniport *mp)
{
    int ret = 0;
    if (!mp || !mp->name || !mp->ops || !mp->ops->add_device ||
        !mp->ops->start_device || !mp->ops->create_allocation ||
        !mp->ops->stop_device || !mp->ops->remove_device)
        return -EINVAL;
    mutex_lock(&idw_dispatch.lock);
    if (idw_dispatch.miniport)
        ret = -EBUSY;
    else
        idw_dispatch.miniport = mp;
    mutex_unlock(&idw_dispatch.lock);
    if (!ret)
        pr_info("idw: miniport registered: %s\n", mp->name);
    return ret;
}
EXPORT_SYMBOL_GPL(idw_register_miniport);

void idw_unregister_miniport(const struct idw_miniport *mp)
{
    mutex_lock(&idw_dispatch.lock);
    if (idw_dispatch.miniport == mp) {
        if (idw_dispatch.started) {
            mp->ops->stop_device(idw_dispatch.adapter_context);
            idw_dispatch.started = false;
        }
        if (idw_dispatch.added) {
            mp->ops->remove_device(idw_dispatch.adapter_context);
            idw_dispatch.added = false;
        }
        idw_dispatch.adapter_context = NULL;
        idw_dispatch.miniport = NULL;
        pr_info("idw: miniport unregistered: %s\n", mp->name);
    }
    mutex_unlock(&idw_dispatch.lock);
}
EXPORT_SYMBOL_GPL(idw_unregister_miniport);

int idw_dispatch_add_device(void *device)
{
    int ret;
    mutex_lock(&idw_dispatch.lock);
    if (!idw_dispatch.miniport) ret = -ENODEV;
    else if (idw_dispatch.added) ret = -EALREADY;
    else {
        ret = idw_dispatch.miniport->ops->add_device(device, &idw_dispatch.adapter_context);
        if (!ret) idw_dispatch.added = true;
    }
    mutex_unlock(&idw_dispatch.lock);
    return ret;
}
EXPORT_SYMBOL_GPL(idw_dispatch_add_device);

int idw_dispatch_start_device(void)
{
    int ret;
    mutex_lock(&idw_dispatch.lock);
    if (!idw_dispatch.miniport || !idw_dispatch.added) ret = -ENODEV;
    else if (idw_dispatch.started) ret = -EALREADY;
    else {
        ret = idw_dispatch.miniport->ops->start_device(idw_dispatch.adapter_context);
        if (!ret) idw_dispatch.started = true;
    }
    mutex_unlock(&idw_dispatch.lock);
    return ret;
}
EXPORT_SYMBOL_GPL(idw_dispatch_start_device);

int idw_dispatch_create_allocation(u64 bytes, u64 *allocation_id)
{
    int ret;
    if (!bytes || !allocation_id || bytes > (1ULL << 30)) return -EINVAL;
    mutex_lock(&idw_dispatch.lock);
    if (!idw_dispatch.miniport || !idw_dispatch.started) ret = -ENODEV;
    else ret = idw_dispatch.miniport->ops->create_allocation(
                    idw_dispatch.adapter_context, bytes, allocation_id);
    mutex_unlock(&idw_dispatch.lock);
    return ret;
}
EXPORT_SYMBOL_GPL(idw_dispatch_create_allocation);

void idw_dispatch_stop_device(void)
{
    mutex_lock(&idw_dispatch.lock);
    if (idw_dispatch.miniport && idw_dispatch.started) {
        idw_dispatch.miniport->ops->stop_device(idw_dispatch.adapter_context);
        idw_dispatch.started = false;
    }
    mutex_unlock(&idw_dispatch.lock);
}
EXPORT_SYMBOL_GPL(idw_dispatch_stop_device);

void idw_dispatch_remove_device(void)
{
    mutex_lock(&idw_dispatch.lock);
    if (idw_dispatch.miniport) {
        if (idw_dispatch.started) {
            idw_dispatch.miniport->ops->stop_device(idw_dispatch.adapter_context);
            idw_dispatch.started = false;
        }
        if (idw_dispatch.added) {
            idw_dispatch.miniport->ops->remove_device(idw_dispatch.adapter_context);
            idw_dispatch.added = false;
        }
        idw_dispatch.adapter_context = NULL;
    }
    mutex_unlock(&idw_dispatch.lock);
}
EXPORT_SYMBOL_GPL(idw_dispatch_remove_device);

static int idw_open(struct inode *inode, struct file *file)
{
    struct idw_file_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    (void)inode;
    if (!ctx) return -ENOMEM;
    mutex_init(&ctx->lock);
    idr_init(&ctx->allocations);
    ctx->next_allocation_id = 1;
    file->private_data = ctx;
    return 0;
}

static int idw_release(struct inode *inode, struct file *file)
{
    struct idw_file_ctx *ctx = file->private_data;
    struct idw_allocation *allocation;
    int id;
    (void)inode;
    if (!ctx) return 0;
    idr_for_each_entry(&ctx->allocations, allocation, id) {
        idw_allocation_put(allocation);
    }
    idr_destroy(&ctx->allocations);
    kfree(ctx);
    return 0;
}

static long idw_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct idw_file_ctx *ctx = file->private_data;
    void __user *up = (void __user *)arg;

    if (cmd == IDW_IOCTL_QUERY) {
        struct idw_query q = {
            .struct_size = sizeof(q), .abi_major = IDW_ABI_MAJOR,
            .abi_minor = IDW_ABI_MINOR,
            .state = IDW_STATE_CONTROL_ONLY,
            .features = IDW_FEATURE_CONTROL_PLANE | IDW_FEATURE_MINIPORT_API |
                        IDW_FEATURE_RESOURCE_HANDLES | IDW_FEATURE_DMABUF_IMPORT |
                        IDW_FEATURE_SYSTEM_BACKING,
        };
        if (idw_miniport_is_registered()) q.state |= IDW_STATE_MINIPORT_PRESENT;
        return copy_to_user(up, &q, sizeof(q)) ? -EFAULT : 0;
    }

    if (cmd == IDW_IOCTL_ALLOCATION_CREATE) {
        struct idw_allocation_create req;
        struct idw_allocation *allocation;
        u64 size;
        int id;
        if (copy_from_user(&req, up, sizeof(req))) return -EFAULT;
        if (req.struct_size != sizeof(req) ||
            (req.flags & ~IDW_ALLOCATION_CREATE_SYSTEM_BACKED) ||
            req.reserved || !req.size)
            return -EINVAL;
        if (req.size > IDW_MAX_ALLOCATION_SIZE || req.size > U64_MAX - (PAGE_SIZE - 1))
            return -E2BIG;
        size = PAGE_ALIGN(req.size);
        allocation = kzalloc(sizeof(*allocation), GFP_KERNEL);
        if (!allocation) return -ENOMEM;
        kref_init(&allocation->ref);
        atomic_set(&allocation->mappings, 0);
        allocation->size = size;
        if (req.flags & IDW_ALLOCATION_CREATE_SYSTEM_BACKED) {
            allocation->system_memory = vmalloc_user(size);
            if (!allocation->system_memory) {
                idw_allocation_put(allocation);
                return -ENOMEM;
            }
        }
        mutex_lock(&ctx->lock);
        if (size > IDW_MAX_FILE_BYTES - ctx->bytes) {
            mutex_unlock(&ctx->lock);
            idw_allocation_put(allocation);
            return -EDQUOT;
        }
        if (ctx->next_allocation_id >= INT_MAX) {
            mutex_unlock(&ctx->lock);
            idw_allocation_put(allocation);
            return -ENOSPC;
        }
        id = idr_alloc(&ctx->allocations, allocation,
                       ctx->next_allocation_id, INT_MAX, GFP_KERNEL);
        if (id >= 0) {
            ctx->bytes += size;
            ctx->next_allocation_id = id + 1;
            req.allocation_id = id;
            req.size = size;
        }
        mutex_unlock(&ctx->lock);
        if (id < 0) { idw_allocation_put(allocation); return id; }
        if (copy_to_user(up, &req, sizeof(req))) {
            struct idw_allocation *removed;
            mutex_lock(&ctx->lock);
            removed = idr_remove(&ctx->allocations, id);
            if (removed) ctx->bytes -= removed->size;
            mutex_unlock(&ctx->lock);
            if (removed) idw_allocation_put(removed);
            return -EFAULT;
        }
        mutex_lock(&ctx->lock);
        allocation->published = true;
        mutex_unlock(&ctx->lock);
        return 0;
    }

    if (cmd == IDW_IOCTL_ALLOCATION_IMPORT_DMABUF) {
        struct idw_allocation_import_dmabuf req;
        struct idw_allocation *allocation;
        struct dma_buf *dmabuf;
        int id;
        if (copy_from_user(&req, up, sizeof(req))) return -EFAULT;
        if (req.struct_size != sizeof(req) || req.dma_buf_fd < 0 || req.flags)
            return -EINVAL;
        dmabuf = dma_buf_get(req.dma_buf_fd);
        if (IS_ERR(dmabuf)) return PTR_ERR(dmabuf);
        if (!dmabuf->size || dmabuf->size > IDW_MAX_ALLOCATION_SIZE) {
            dma_buf_put(dmabuf);
            return -E2BIG;
        }
        allocation = kzalloc(sizeof(*allocation), GFP_KERNEL);
        if (!allocation) { dma_buf_put(dmabuf); return -ENOMEM; }
        kref_init(&allocation->ref);
        atomic_set(&allocation->mappings, 0);
        allocation->size = dmabuf->size;
        allocation->dmabuf = dmabuf;
        mutex_lock(&ctx->lock);
        if (allocation->size > IDW_MAX_FILE_BYTES - ctx->bytes) {
            mutex_unlock(&ctx->lock);
            idw_allocation_put(allocation);
            return -EDQUOT;
        }
        if (ctx->next_allocation_id >= INT_MAX) {
            mutex_unlock(&ctx->lock);
            idw_allocation_put(allocation);
            return -ENOSPC;
        }
        id = idr_alloc(&ctx->allocations, allocation,
                       ctx->next_allocation_id, INT_MAX, GFP_KERNEL);
        if (id >= 0) {
            ctx->bytes += allocation->size;
            ctx->next_allocation_id = id + 1;
            req.allocation_id = id;
        }
        mutex_unlock(&ctx->lock);
        if (id < 0) {
            idw_allocation_put(allocation);
            return id;
        }
        if (copy_to_user(up, &req, sizeof(req))) {
            struct idw_allocation *removed;
            mutex_lock(&ctx->lock);
            removed = idr_remove(&ctx->allocations, id);
            if (removed) ctx->bytes -= removed->size;
            mutex_unlock(&ctx->lock);
            if (removed) {
                idw_allocation_put(removed);
            }
            return -EFAULT;
        }
        mutex_lock(&ctx->lock);
        allocation->published = true;
        mutex_unlock(&ctx->lock);
        return 0;
    }

    if (cmd == IDW_IOCTL_ALLOCATION_DESTROY) {
        struct idw_allocation_destroy req;
        struct idw_allocation *allocation;
        if (copy_from_user(&req, up, sizeof(req))) return -EFAULT;
        if (req.struct_size != sizeof(req) || !req.allocation_id) return -EINVAL;
        mutex_lock(&ctx->lock);
        allocation = idr_find(&ctx->allocations, req.allocation_id);
        if (allocation && !allocation->published) {
            mutex_unlock(&ctx->lock);
            return -ENOENT;
        }
        if (allocation && atomic_read(&allocation->mappings)) {
            mutex_unlock(&ctx->lock);
            return -EBUSY;
        }
        allocation = idr_remove(&ctx->allocations, req.allocation_id);
        if (allocation) ctx->bytes -= allocation->size;
        mutex_unlock(&ctx->lock);
        if (!allocation) return -ENOENT;
        idw_allocation_put(allocation);
        return 0;
    }
    return -ENOTTY;
}

static void idw_vma_open(struct vm_area_struct *vma)
{
    struct idw_allocation *allocation = vma->vm_private_data;

    atomic_inc(&allocation->mappings);
    kref_get(&allocation->ref);
}

static void idw_vma_close(struct vm_area_struct *vma)
{
    struct idw_allocation *allocation = vma->vm_private_data;

    atomic_dec(&allocation->mappings);
    idw_allocation_put(allocation);
}

static const struct vm_operations_struct idw_vm_ops = {
    .open = idw_vma_open,
    .close = idw_vma_close,
};

static int idw_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct idw_file_ctx *ctx = file->private_data;
    struct idw_allocation *allocation;
    unsigned long length = vma->vm_end - vma->vm_start;
    unsigned long id = vma->vm_pgoff;
    int ret;

    if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC) ||
        id > INT_MAX)
        return -EINVAL;

    mutex_lock(&ctx->lock);
    allocation = idr_find(&ctx->allocations, (int)id);
    if (!allocation || !allocation->published || !allocation->system_memory ||
        length > allocation->size) {
        mutex_unlock(&ctx->lock);
        return -EINVAL;
    }
    kref_get(&allocation->ref);
    atomic_inc(&allocation->mappings);
    mutex_unlock(&ctx->lock);

    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
    ret = remap_vmalloc_range(vma, allocation->system_memory, 0);
    if (ret) {
        atomic_dec(&allocation->mappings);
        idw_allocation_put(allocation);
        return ret;
    }

    vma->vm_ops = &idw_vm_ops;
    vma->vm_private_data = allocation;
    return 0;
}

static const struct file_operations idw_fops = {
    .owner = THIS_MODULE, .open = idw_open, .release = idw_release, .unlocked_ioctl = idw_ioctl,
    .mmap = idw_mmap,
#ifdef CONFIG_COMPAT
    .compat_ioctl = compat_ptr_ioctl,
#endif
};
static struct miscdevice idw_misc = {
    .minor = MISC_DYNAMIC_MINOR, .name = "idw", .fops = &idw_fops, .mode = 0600,
};
static int __init idw_init(void)
{
    int ret;
    mutex_init(&idw_dispatch.lock);
    ret = misc_register(&idw_misc);
    if (!ret) pr_info("idw: control plane + miniport dispatcher ready; GPU remains owned by i915\n");
    return ret;
}
static void __exit idw_exit(void)
{
    idw_dispatch_remove_device();
    misc_deregister(&idw_misc);
}
module_init(idw_init);
module_exit(idw_exit);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_AUTHOR("IDW Project");
MODULE_DESCRIPTION("IDW resource manager and native miniport callback dispatcher");
MODULE_VERSION("1.0.0");
