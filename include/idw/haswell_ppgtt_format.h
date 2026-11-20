/* SPDX-License-Identifier: MIT */
#ifndef IDW_HASWELL_PPGTT_FORMAT_H
#define IDW_HASWELL_PPGTT_FORMAT_H

#include "contracts/haswell_ppgtt.h"

/*
 * Upstream Gen6/7 intel_gtt.h encodes a 40-bit DMA page address into a 32-bit
 * PTE/PDE as addr | ((addr >> 28) & 0xff0). These helpers are shared by the
 * CPU model and the DMA-backed page-table storage tests so the wire layout has
 * one definition.
 */
static inline uint32_t idw_hsw_ppgtt_encode_address(uint64_t address)
{
    return (uint32_t)address | (uint32_t)((address >> 28) & 0xff0u);
}

static inline uint64_t idw_hsw_ppgtt_decode_address(uint32_t entry)
{
    return (uint64_t)(entry & 0xfffff000u) |
           ((uint64_t)(entry & 0x00000ff0u) << 28);
}

#endif
