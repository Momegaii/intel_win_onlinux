#ifndef IDW_IOCTL_H
#define IDW_IOCTL_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define IDW_IOCTL_MAGIC 0xD7
#define IDW_ABI_MAJOR 0
#define IDW_ABI_MINOR 4

#define IDW_FEATURE_CONTROL_PLANE (1ULL << 0)
#define IDW_FEATURE_I915_OBSERVE   (1ULL << 1)
#define IDW_FEATURE_MINIPORT_API   (1ULL << 2)
#define IDW_FEATURE_WDDM_SHIM      (1ULL << 3) /* reserved: not a Windows ABI */
#define IDW_FEATURE_RESOURCE_HANDLES (1ULL << 4) /* IDW metadata handles */
#define IDW_FEATURE_DMABUF_IMPORT    (1ULL << 5) /* retained dma-buf references; exporter owns backing */
#define IDW_FEATURE_SYSTEM_BACKING   (1ULL << 6) /* kernel system memory; not GPU-mapped */

#define IDW_ALLOCATION_CREATE_SYSTEM_BACKED (1U << 0)

#define IDW_STATE_CONTROL_ONLY     (1U << 0)
#define IDW_STATE_MINIPORT_PRESENT (1U << 1)
#define IDW_STATE_DEVICE_OWNED     (1U << 2) /* currently never set */

struct idw_allocation_create {
    __u32 struct_size;
    __u32 flags; /* IDW_ALLOCATION_CREATE_* */
    __u64 size;
    __u32 allocation_id; /* output */
    __u32 reserved;
};

struct idw_allocation_import_dmabuf {
    __u32 struct_size;
    __s32 dma_buf_fd; /* exported DRM/GEM dma-buf descriptor */
    __u32 allocation_id; /* output */
    __u32 flags; /* must be zero */
};

struct idw_allocation_destroy {
    __u32 struct_size;
    __u32 allocation_id;
};

struct idw_query {
    __u32 struct_size;
    __u32 abi_major;
    __u32 abi_minor;
    __u32 state;
    __u64 features;
    __u64 reserved[2];
};

#define IDW_IOCTL_QUERY _IOR(IDW_IOCTL_MAGIC, 0x00, struct idw_query)
#define IDW_IOCTL_ALLOCATION_CREATE _IOWR(IDW_IOCTL_MAGIC, 0x10, struct idw_allocation_create)
#define IDW_IOCTL_ALLOCATION_DESTROY _IOW(IDW_IOCTL_MAGIC, 0x11, struct idw_allocation_destroy)
#define IDW_IOCTL_ALLOCATION_IMPORT_DMABUF _IOWR(IDW_IOCTL_MAGIC, 0x12, struct idw_allocation_import_dmabuf)

/* For system-backed allocations, mmap offset is allocation_id * PAGE_SIZE.
 * The mapping is shared. Destroy returns -EBUSY while the allocation is
 * mapped; closing the file drops its handle reference, while a live VMA keeps
 * the memory backing alive. Metadata-only and dma-buf-import handles are not
 * mmap-able.
 */
#endif
