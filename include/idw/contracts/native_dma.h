/* SPDX-License-Identifier: MIT */
#ifndef IDW_NATIVE_DMA_BACKEND_CONTRACT_H
#define IDW_NATIVE_DMA_BACKEND_CONTRACT_H

#include "page_table_memory.h"
#include "haswell_ppgtt.h"

#define IDW_NATIVE_DMA_CONTRACT_VERSION 1u

/*
 * Native backend contract. The device scope must come from the exclusively
 * owned PCI device's DMA lifecycle. A DMA API address from i915 or another
 * device is invalid here. The kernel DMA page provider is not yet wired to
 * this contract and does not authorize PCI binding, BAR access, or takeover.
 */
struct idw_native_dma_contract {
    uint32_t size;
    uint32_t version;
    uint32_t dma_address_bits;
    uint32_t flags;
    struct idw_pt_backend_desc backend;
    struct idw_pt_memory_ops memory;
    struct idw_gen75_ppgtt_ops ppgtt;
};

int idw_native_dma_contract_validate(
    const struct idw_native_dma_contract *contract);

#endif
