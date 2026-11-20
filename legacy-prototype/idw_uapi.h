#ifndef IDW_UAPI_H
#define IDW_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define IDW_DEV_NAME "inteldriverwin_onlinux"
#define IDW_IOC_MAGIC 'I'

#define IDW_RING_SLOTS 64U
#define IDW_MAX_PAYLOAD 256U

#define IDW_GTT_DOMAIN_GGTT  1U
#define IDW_GTT_DOMAIN_PPGTT 2U

#define IDW_GTT_FLAG_READ  (1U << 0)
#define IDW_GTT_FLAG_WRITE (1U << 1)

#define IDW_OP_SUBMIT_CMD  1U
#define IDW_OP_WIN_ESCAPE  2U

#define IDW_WDDM_DDI_SUBMIT_RENDER 1U
#define IDW_WDDM_DDI_PRESENT       2U
#define IDW_WDDM_DDI_CREATE_ALLOC  3U

struct idw_submit_req {
    uint64_t cmd_buf_uaddr;
    uint32_t cmd_buf_bytes;
    uint32_t op_code;
    uint64_t fence_seq;
    int32_t  nt_status;
    uint32_t reserved;
};

struct idw_fence_query {
    uint64_t submitted_fence;
    uint64_t completed_fence;
    int32_t  nt_status;
    uint32_t reserved;
};

struct idw_gtt_map_req {
    uint64_t requested_gpu_va;
    uint64_t size_bytes;
    uint32_t domain;
    uint32_t flags;
    uint64_t mapped_gpu_va;
    uint64_t mapped_dma_iova;
    uint64_t tlb_flush_seq;
    int32_t  nt_status;
    uint32_t reserved;
};

struct idw_gtt_unmap_req {
    uint64_t mapped_gpu_va;
    uint32_t domain;
    uint32_t reserved;
    uint64_t tlb_flush_seq;
    int32_t  nt_status;
};

struct idw_wddm_translate_req {
    uint32_t wddm_ddi_kind;
    uint32_t irql_level;
    uint64_t wddm_fence_val;
    uint64_t wddm_alloc_handle;
    uint64_t linux_dma_fence;
    uint32_t drm_gem_handle;
    int32_t  nt_status;
};

struct idw_debug_stats {
    uint64_t submitted_fence;
    uint64_t completed_fence;
    uint64_t ggtt_tlb_flush_seq;
    uint64_t ppgtt_tlb_flush_seq;
    uint64_t active_ggtt_regions;
    uint64_t active_ppgtt_regions;
    uint32_t ring_head;
    uint32_t ring_tail;
    int32_t  nt_status;
    uint32_t reserved;
};

#define IDW_IOC_SUBMIT         _IOWR(IDW_IOC_MAGIC, 0x01, struct idw_submit_req)
#define IDW_IOC_QUERY_FENCE    _IOWR(IDW_IOC_MAGIC, 0x02, struct idw_fence_query)
#define IDW_IOC_GTT_MAP        _IOWR(IDW_IOC_MAGIC, 0x03, struct idw_gtt_map_req)
#define IDW_IOC_GTT_UNMAP      _IOWR(IDW_IOC_MAGIC, 0x04, struct idw_gtt_unmap_req)
#define IDW_IOC_WDDM_TRANSLATE _IOWR(IDW_IOC_MAGIC, 0x05, struct idw_wddm_translate_req)
#define IDW_IOC_QUERY_DEBUG    _IOWR(IDW_IOC_MAGIC, 0x06, struct idw_debug_stats)

#endif /* IDW_UAPI_H */
