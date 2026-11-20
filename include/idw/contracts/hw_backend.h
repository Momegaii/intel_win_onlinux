/* SPDX-License-Identifier: MIT */
#ifndef IDW_HW_BACKEND_CONTRACT_H
#define IDW_HW_BACKEND_CONTRACT_H
#include "base.h"

struct idw_adapter_identity {
    uint32_t size;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    uint8_t revision;
    uint8_t generation; /* 7 for Haswell family; 7.5 encoded by stepping/caps */
    uint16_t reserved;
};

struct idw_backend_caps {
    uint32_t size;
    uint32_t engine_mask;
    uint64_t gpu_va_min;
    uint64_t gpu_va_max;
    uint64_t page_size_mask;
    uint32_t max_contexts;
    uint32_t flags;
    uint32_t ppgtt_generation;
    uint32_t ppgtt_address_bits;
};

struct idw_hw_backend_ops {
    uint32_t size;
    uint32_t version;
    int (*probe)(struct idw_adapter_identity *identity);
    int (*start)(void);
    int (*query_caps)(struct idw_backend_caps *caps);
    int (*stop)(void);
};

/* No PCI binding or MMIO access is implemented by declaring this interface. */
#endif
