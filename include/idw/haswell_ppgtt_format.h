/* SPDX-License-Identifier: MIT */
#ifndef IDW_HASWELL_PPGTT_FORMAT_H
#define IDW_HASWELL_PPGTT_FORMAT_H

#include "contracts/haswell_ppgtt.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

/*
 * Haswell Gen7.5 uses different 32-bit entry layouts for PDEs and PTEs.
 *
 * PDE: bits 31:12 = page-table address 31:12
 *      bits 11:4  = page-table address 39:32
 *      bits 3:2   = reserved
 *      bit 1      = page size (0 = 4 KiB, 1 = 32 KiB)
 *      bit 0      = valid
 *
 * PTE: bits 31:12 = page address 31:12
 *      bit 11     = cacheability control bit 3
 *      bits 10:4  = page address 38:32
 *      bits 3:1   = cacheability control bits 2:0
 *      bit 0      = valid
 *
 * A page table may be addressed below 2^40. A mapped data page is limited to
 * 2^39 because Haswell repurposes PTE physical-address bit 39 as cacheability
 * control. Do not use the PDE transform for PTEs.
 */
#define IDW_HSW_PPGTT_PT_DMA_ADDRESS_BITS 40u
#define IDW_HSW_PPGTT_PAGE_DMA_ADDRESS_BITS 39u
#define IDW_HSW_PPGTT_PDE_VALID 0x00000001u
#define IDW_HSW_PPGTT_PDE_PAGE_SIZE_32K 0x00000002u
#define IDW_HSW_PPGTT_PDE_RESERVED_MASK 0x0000000cu
#define IDW_HSW_PPGTT_PTE_VALID 0x00000001u
#define IDW_HSW_PPGTT_PTE_CACHE_UC 0x0u

static inline int idw_hsw_ppgtt_encode_pde(uint64_t page_table_dma,
                                           uint32_t *entry)
{
    if (!entry)
        return -EINVAL;
    if (page_table_dma >= (1ULL << IDW_HSW_PPGTT_PT_DMA_ADDRESS_BITS))
        return -ERANGE;
    if (page_table_dma & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1))
        return -EINVAL;
    *entry = (uint32_t)(page_table_dma & 0xfffff000u) |
             (uint32_t)((page_table_dma >> 28) & 0x00000ff0u) |
             IDW_HSW_PPGTT_PDE_VALID;
    return 0;
}

static inline int idw_hsw_ppgtt_encode_pte(uint64_t page_dma,
                                           uint32_t cacheability,
                                           uint32_t *entry)
{
    uint32_t cache;

    if (!entry)
        return -EINVAL;
    if (cacheability > 0x0fu)
        return -EINVAL;
    if (page_dma >= (1ULL << IDW_HSW_PPGTT_PAGE_DMA_ADDRESS_BITS))
        return -ERANGE;
    if (page_dma & (IDW_HASWELL_PPGTT_PAGE_SIZE - 1))
        return -EINVAL;
    cache = ((cacheability & 0x7u) << 1) |
            ((cacheability & 0x8u) << 8);
    *entry = (uint32_t)(page_dma & 0xfffff000u) |
             (uint32_t)((page_dma >> 28) & 0x000007f0u) |
             cache |
             IDW_HSW_PPGTT_PTE_VALID;
    return 0;
}

static inline uint64_t idw_hsw_ppgtt_decode_pde(uint32_t entry)
{
    return (uint64_t)(entry & 0xfffff000u) |
           ((uint64_t)(entry & 0x00000ff0u) << 28);
}

static inline uint64_t idw_hsw_ppgtt_decode_pte(uint32_t entry)
{
    return (uint64_t)(entry & 0xfffff000u) |
           ((uint64_t)(entry & 0x000007f0u) << 28);
}

static inline uint32_t idw_hsw_ppgtt_pte_cacheability(uint32_t entry)
{
    return ((entry & 0x0000000eu) >> 1) |
           ((entry & 0x00000800u) >> 8);
}

static inline int idw_hsw_ppgtt_validate_pde_entry(uint32_t entry)
{
    if (entry & IDW_HSW_PPGTT_PDE_RESERVED_MASK)
        return -EINVAL;
    if (entry & IDW_HSW_PPGTT_PDE_PAGE_SIZE_32K)
        return -EOPNOTSUPP;
    return 0;
}

static inline int idw_hsw_ppgtt_validate_pte_entry(uint32_t entry)
{
    (void)entry;
    return 0;
}

#endif
